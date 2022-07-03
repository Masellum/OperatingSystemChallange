/*
 * Supervisor-mode startup codes
 */

#include "riscv.h"
#include "string.h"
#include "elf.h"
#include "process.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "memlayout.h"
#include "spike_interface/spike_utils.h"

//
// trap_sec_start points to the beginning of S-mode trap segment (i.e., the entry point of
// S-mode trap vector). added @lab2_1
//
extern char trap_sec_start[];

//
// turn on paging. added @lab2_1
//
void enable_paging() {
  // write the pointer to kernel page (table) directory into the CSR of "satp".
  write_csr(satp, MAKE_SATP(g_kernel_pagetable));

  // refresh tlb to invalidate its content.
  flush_tlb();
}

//
// load the elf, and construct a "process" (with only a trapframe).
// load_bincode_from_host_elf is defined in elf.c
//
process* load_user_program() {
  process* proc;

  proc = alloc_process();
  sprint("User application is loading.\n");

  load_bincode_from_host_elf(proc);
  return proc;
}

void init_idle() {
  idle = &procs[0];
  idle->kstack = (uint64)alloc_page() + PGSIZE;
  idle->pagetable = g_kernel_pagetable;
  idle->trapframe = (trapframe *)alloc_page();
  memset(idle->trapframe, 0, sizeof(trapframe));
  idle->trapframe->regs.sp = idle->kstack;
  idle->trapframe->epc = (uint64)do_idle;
  // idle->mapped_info = (mapped_region *)alloc_page();
  idle->mapped_info = NULL;
  idle->total_mapped_region = 0;
  idle->pid = 0;
  idle->status = READY;
  idle->parent = NULL;
  idle->queue_next = NULL;
  idle->waiting_queue_next = NULL;
  idle->waiting_pid = 0;
  idle->tick_count = 0;
}

void clean_idle() {
  // free_page((void *)idle->mapped_info);
  user_vm_unmap(g_kernel_pagetable, (uint64)idle->trapframe, PGSIZE, 1);
  free_page((void *)idle->kstack - PGSIZE);
}
//
// s_start: S-mode entry point of riscv-pke OS kernel.
//
int s_start(void) {
  sprint("Enter supervisor mode...\n");
  // in the beginning, we use Bare mode (direct) memory mapping as in lab1.
  // but now, we are going to switch to the paging mode @lab2_1.
  // note, the code still works in Bare mode when calling pmm_init() and kern_vm_init().
  write_csr(satp, 0);

  // init phisical memory manager
  pmm_init();

  // build the kernel page table
  kern_vm_init();

  // now, switch to paging mode by turning on paging (SV39)
  enable_paging();
  // the code now formally works in paging mode, meaning the page table is now in use.
  sprint("kernel page table is on \n");

  // added @lab3_1
  init_proc_pool();
  init_idle();

  sprint("Switch to user mode...\n");
  // the application code (elf) is first loaded into memory, and then put into execution
  // added @lab3_1
  insert_to_ready_queue( load_user_program() );
  schedule();

  // we should never reach here.
  return 0;
}
