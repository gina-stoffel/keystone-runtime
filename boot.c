#include <asm/csr.h>

#include "printf.h"
#include "interrupt.h"
#include "syscall.h"
#include "vm.h"
#include "string.h"
#include "sbi.h"
#include "freemem.h"
#include "mm.h"
#include "env.h"
#include "paging.h"
#include "process_snapshot.h"
#include "malloc.h"
#include "uaccess.h"

/* defined in vm.h */
extern uintptr_t shared_buffer;
extern uintptr_t shared_buffer_size;
extern uintptr_t utm_paddr_start; 

/* initial memory layout */
uintptr_t utm_base;
size_t utm_size;

/* defined in entry.S */
extern void* encl_trap_handler;

/* Snapshot of user processs*/
struct proc_snapshot snapshot;

#ifdef USE_FREEMEM


/* map entire enclave physical memory so that
 * we can access the old page table and free memory */
/* remap runtime kernel to a new root page table */
void
map_physical_memory(uintptr_t dram_base,
                    uintptr_t dram_size)
{
  uintptr_t ptr = EYRIE_LOAD_START;
  /* load address should not override kernel address */
  assert(RISCV_GET_PT_INDEX(ptr, 1) != RISCV_GET_PT_INDEX(runtime_va_start, 1));
  map_with_reserved_page_table(dram_base, dram_size,
      ptr, load_l2_page_table, load_l3_page_table);
}

void
remap_kernel_space(uintptr_t runtime_base,
                   uintptr_t runtime_size)
{
  /* eyrie runtime is supposed to be smaller than a megapage */

  #if __riscv_xlen == 64
  assert(runtime_size <= RISCV_GET_LVL_PGSIZE(2));
  #elif __riscv_xlen == 32
  assert(runtime_size <= RISCV_GET_LVL_PGSIZE(1));
  #endif

  map_with_reserved_page_table(runtime_base, runtime_size,
     runtime_va_start, kernel_l2_page_table, kernel_l3_page_table);
}

void
map_untrusted_memory(uintptr_t base,
                     uintptr_t size)
{
  uintptr_t ptr = EYRIE_UNTRUSTED_START;

  /* untrusted memory is smaller than a megapage (2 MB in RV64, 4MB in RV32) */
  #if __riscv_xlen == 64
  assert(size <= RISCV_GET_LVL_PGSIZE(2));
  #elif __riscv_xlen == 32
  assert(size <= RISCV_GET_LVL_PGSIZE(1));
  #endif

  map_with_reserved_page_table(base, size,
      ptr, utm_l2_page_table, utm_l3_page_table);

  shared_buffer = ptr;
  shared_buffer_size = size;
}

void
copy_root_page_table()
{
  /* the old table lives in the first page */
  pte* old_root_page_table = (pte*) EYRIE_LOAD_START;
  int i;

  /* copy all valid entries of the old root page table */
  for (i = 0; i < BIT(RISCV_PT_INDEX_BITS); i++) {
    if (old_root_page_table[i] & PTE_V &&
        !(root_page_table[i] & PTE_V)) {
      root_page_table[i] = old_root_page_table[i];
    }
  }
}

/* initialize free memory with a simple page allocator*/
void
init_freemem()
{
  spa_init(freemem_va_start, freemem_size);
}

#endif // USE_FREEMEM

/* initialize user stack */
void
init_user_stack_and_env(bool is_fork)
{
  void* user_sp = (void*) EYRIE_USER_STACK_START;

#ifdef USE_FREEMEM
if(!is_fork){
  size_t count;
  uintptr_t stack_end = EYRIE_USER_STACK_END;
  size_t stack_count = EYRIE_USER_STACK_SIZE >> RISCV_PAGE_BITS;


  // allocated stack pages right below the runtime
  count = alloc_pages(vpn(stack_end), stack_count,
      PTE_R | PTE_W | PTE_D | PTE_A | PTE_U);

  assert(count == stack_count);
}

#endif // USE_FREEMEM

  // setup user stack env/aux
  user_sp = setup_start(user_sp);

  // prepare user sp
  csr_write(sscratch, user_sp);
}

int remap_freemem(struct proc_snapshot *snapshot, int level, pte* tb, uintptr_t vaddr) {
  pte* walk;
  int i;
  uintptr_t parent_freemem_start = snapshot->freemem_pa_start;
  uintptr_t parent_freemem_end = snapshot->freemem_pa_end;

  /* iterate over PTEs */
  for (walk = tb, i = 0; walk < tb + (RISCV_PAGE_SIZE / sizeof(pte));
       walk += 1, i++) {

    if ((*walk) == 0) {
      continue;
    }

    uintptr_t vpn;
    uintptr_t phys_addr = ((*walk) >> PTE_PPN_SHIFT) << RISCV_PAGE_BITS;

    /* propagate the highest bit of the VA */
    if (level == RISCV_PGLEVEL_TOP && i & RISCV_PGTABLE_HIGHEST_BIT)
      vpn = ((-1UL << RISCV_PT_INDEX_BITS) | (i & PTE_FLAG_MASK));
    else
      vpn = ((vaddr << RISCV_PT_INDEX_BITS) | (i & PTE_FLAG_MASK));

    // uintptr_t va_start = vpn << RISCV_PAGE_BITS;

    if (level == 1) {
      /* if PTE is leaf, extend hash for the page */
      int in_freemem =
                ((phys_addr < parent_freemem_end) && (phys_addr >= parent_freemem_start));

      if(in_freemem){
          uintptr_t new_phys_addr = load_pa_start + (phys_addr - parent_freemem_start);
          *walk = pte_create(new_phys_addr >> RISCV_PAGE_BITS, (*walk) & PTE_FLAG_MASK); 
      }
      
      // printf("user PAGE hashed: 0x%lx (pa: 0x%lx)\n", vpn << RISCV_PAGE_BITS, phys_addr);


    } else {
      /* otherwise, recurse on a lower level */
      pte* mapped_paddr = (pte *) __va(phys_addr);
      remap_freemem(snapshot, level - 1, mapped_paddr, vpn);
    }
  }
  return 0;
}

struct proc_snapshot * 
handle_fork(void* buffer, struct proc_snapshot *ret){

  uintptr_t *user_va =(uintptr_t *) __va(user_paddr_start);
  
  struct edge_call* edge_call = (struct edge_call*)buffer;

  uintptr_t call_args;
  size_t args_len;

  if(!edge_call->call_id){
    return NULL; 
  }

  if (edge_call_args_ptr(edge_call, &call_args, &args_len) != 0) {
    edge_call->return_data.call_status = CALL_STATUS_BAD_OFFSET;
    return NULL;
  }

  memcpy(ret, (void *) call_args, sizeof(struct proc_snapshot));

  uintptr_t snapshot_payload = (uintptr_t) call_args;
  snapshot_payload += sizeof(struct proc_snapshot); 
  memcpy(user_va, (void *) snapshot_payload, args_len - sizeof(struct proc_snapshot));

  remap_freemem(ret, RISCV_PT_LEVELS, root_page_table, 0);

  printf("check :%x, %x\n", *user_va, *(uintptr_t *)snapshot_payload);

  //Clear out the snapshot after use
  // memset((vodcall_args, 0, args_len);

  return (struct proc_snapshot *) ret;
}

uintptr_t
eyrie_boot(uintptr_t dummy, // $a0 contains the return value from the SBI
           uintptr_t dram_base,
           uintptr_t dram_size,
           uintptr_t runtime_paddr,
           uintptr_t user_paddr,
           uintptr_t free_paddr,
           uintptr_t utm_paddr,
           uintptr_t utm_size)
{
  /* set initial values */
  load_pa_start = dram_base;
  load_pa_child_start = dram_base;
  runtime_va_start = (uintptr_t) &rt_base;
  kernel_offset = runtime_va_start - runtime_paddr;
  user_paddr_start = user_paddr;
  user_paddr_end = free_paddr;
  utm_paddr_start = utm_paddr; 

  shared_buffer = EYRIE_UNTRUSTED_START;
  shared_buffer_size = utm_size; 

  debug("UTM : 0x%lx-0x%lx (%u KB)", utm_paddr, utm_paddr+utm_size, utm_size/1024);
  debug("DRAM: 0x%lx-0x%lx (%u KB)", dram_base, dram_base + dram_size, dram_size/1024);

#ifdef USE_FREEMEM
  freemem_va_start = __va(free_paddr);
  freemem_size = dram_base + dram_size - free_paddr;

  debug("FREE: 0x%lx-0x%lx (%u KB), va 0x%lx", free_paddr, dram_base + dram_size, freemem_size/1024, freemem_va_start);

  /* remap kernel VA */
  remap_kernel_space(runtime_paddr, user_paddr - runtime_paddr);
  map_physical_memory(dram_base, dram_size);

  /* switch to the new page table */
  csr_write(satp, satp_new(kernel_va_to_pa(root_page_table)));

  /* copy valid entries from the old page table */
  copy_root_page_table();

  map_untrusted_memory(utm_paddr, utm_size);

  /* initialize free memory */
  init_freemem();

  //TODO: This should be set by walking the userspace vm and finding
  //highest used addr. Instead we start partway through the anon space
  set_program_break(EYRIE_ANON_REGION_START + (1024 * 1024 * 1024));

  #ifdef USE_PAGING
  init_paging(user_paddr, free_paddr);
  #endif /* USE_PAGING */
#endif /* USE_FREEMEM */

  /* prepare edge & system calls */
  init_edge_internals();

  bool is_fork = handle_fork((void *) shared_buffer, &snapshot); 

  /* initialize user stack */
  init_user_stack_and_env(is_fork);

  /* set trap vector */
  csr_write(stvec, &encl_trap_handler);

  /* set timer */
  init_timer();

  /* Enable the FPU */
  csr_write(sstatus, csr_read(sstatus) | 0x6000);

  if(is_fork){
    //This will be non-zero in the cases of fork() 
    csr_write(sepc, snapshot.ctx.regs.sepc + 4);
    //Set return value of fork() to be 0 (indicates child)
    snapshot.ctx.regs.a0 = 0; 
  }
  
  debug("eyrie boot finished. drop to the user land ...");
  /* booting all finished, droping to the user land */

  uintptr_t ret = (uintptr_t) &snapshot.ctx.regs;
  return ret;
}
