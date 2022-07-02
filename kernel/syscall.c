/*
 * contains the implementation of all syscalls.
 */

#include <stdint.h>
#include <errno.h>

#include "util/types.h"
#include "syscall.h"
#include "string.h"
#include "process.h"
#include "util/functions.h"
#include "pmm.h"
#include "vmm.h"
#include "malloc.h"
#include "memlayout.h"
#include "spike_interface/spike_utils.h"

//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char* buf, size_t n) {
  // buf is now an address in user space of the given app's user stack,
  // so we have to transfer it into phisical address (kernel is running in direct mapping).
  assert( current );
  char* pa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), (void*)buf);
  sprint(pa);
  return 0;
}

//
// implement the SYS_user_exit syscall
//
ssize_t sys_user_exit(uint64 code) {
  sprint("User exit with code:%d.\n", code);
  // in lab1, PKE considers only one app (one process). 
  // therefore, shutdown the system when the app calls exit()
  shutdown(code);
}

//
// maybe, the simplest implementation of malloc in the world ... added @lab2_2
//
uint64 sys_user_allocate_page(uint64 size) {
  // sprint("size: %x\n", size);
  malloc_chunk *mc = (malloc_chunk *)current->first_free_chunk;
  // sprint("first_free_chunk: %lx\n", current->first_free_chunk);
  malloc_chunk *mc_pa = (malloc_chunk *)lookup_pa((pagetable_t)current->pagetable, (uint64)mc);
  if (mc_pa->mchunk_size - sizeof(malloc_chunk) - 8 - 8 < ROUNDUP(size, 8)) {
    panic("out of memory");
  }
  malloc_chunk * new_alloced = (malloc_chunk *)((uint64)(mc) + UNMASK(mc_pa->mchunk_size) - ROUNDUP(size, 8) - 8 - 8);
  // uint64 begin_va = (uint64)new_alloced - 8, end_va = (uint64)(mc) + UNMASK(mc_pa->mchunk_size);
  uint64 begin_va = ROUNDDOWN(((uint64)new_alloced), PGSIZE), end_va = ROUNDDOWN(((uint64)(mc) + UNMASK(mc_pa->mchunk_size) - 8), PGSIZE);
  // sprint("begin_va: %lx\n", begin_va);
  // sprint("end_va: %lx\n", end_va);
  if (begin_va == ROUNDDOWN((uint64)(mc) + sizeof(malloc_chunk), PGSIZE)) {
    begin_va += PGSIZE;
  }
  uint64 new_alloced_pa;
  if (begin_va < end_va) {
    new_alloced_pa = (uint64)alloc_page();
    user_vm_map((pagetable_t)current->pagetable, begin_va, end_va - begin_va, new_alloced_pa, prot_to_type(PROT_READ | PROT_WRITE, 1));
  }
  mc_pa->mchunk_size -= ROUNDUP(size, 8) + 8;
  new_alloced_pa = lookup_pa((pagetable_t)current->pagetable, (uint64)new_alloced);
  *((uint64*)(new_alloced_pa)) = mc_pa->mchunk_size;
  ((malloc_chunk *)(new_alloced_pa + 8))->mchunk_size = ROUNDUP(size, 8) + 8;
  ((malloc_chunk *)(new_alloced_pa + 8))->mchunk_size |= (2ULL);
  uint64 next_chunk = ((uint64)new_alloced + 8 + UNMASK(((malloc_chunk *)(new_alloced_pa + 8))->mchunk_size));
  if (next_chunk < USER_HEAP_TOP) {
    *(uint64*)(lookup_pa((pagetable_t)current->pagetable, next_chunk)) &= ~2ULL;
  }

  pushdown(&current->first_free_chunk);
  // malloc_chunk *aaaa = (malloc_chunk *)(new_alloced_pa + 8);
  // sprint("%lx %lx %lx %lx\n", aaaa->mchunk_size, aaaa->bk, aaaa->fd, aaaa->prev);
  
  // sprint("new_alloced: %lx\n", new_alloced);
  // sprint("new_alloced_pa: %lx\n", new_alloced_pa);
  // sprint("new_alloced_pa->mchunk_size: %lx\n", ((malloc_chunk *)(new_alloced_pa + 8))->mchunk_size);
  // sprint("allofooter: %lx\n", (uint64)new_alloced + 8 + UNMASK(((malloc_chunk *)(new_alloced_pa + 8))->mchunk_size) - 8ULL);
  return (uint64)(new_alloced) + 16;
}

//
// reclaim a page, indicated by "va". added @lab2_2
//
uint64 sys_user_free_page(uint64 va) {
  malloc_chunk *mc = (malloc_chunk *)(va - 8);
  malloc_chunk *mc_pa = (malloc_chunk *)lookup_pa((pagetable_t)current->pagetable, (uint64)mc);
  // sprint("mc: %lx\n", mc);
  // sprint("mc_pa: %lx\n", mc_pa);
  // sprint("chunksize: %lx\n", mc_pa->mchunk_size);
  uint64 begin_va = ROUNDUP((uint64)mc + sizeof(malloc_chunk), PGSIZE), end_va = ROUNDDOWN((uint64)(mc) + UNMASK(mc_pa->mchunk_size) - 8, PGSIZE) - PGSIZE;
  // sprint("BUG? %lx\n", begin_va);
  // // sprint("BUG? %lx\n", ROUNDDOWN((uint64)(mc) + UNMASK(mc_pa->mchunk_size) - 8, PGSIZE) - PGSIZE);
  // sprint("BUG!\n");
  user_vm_unmap((pagetable_t)current->pagetable, begin_va, end_va - begin_va, 1);
  mc_pa->mchunk_size |= 1ULL;
  *(uint64*)(lookup_pa((pagetable_t)current->pagetable, ((uint64)(mc) + UNMASK(mc_pa->mchunk_size) - 8ULL))) = mc_pa->mchunk_size;
  insert(&current->first_free_chunk, mc);
  if ((uint64)(mc) + mc_pa->mchunk_size < USER_HEAP_TOP && *((uint64*)(lookup_pa((pagetable_t)current->pagetable, (uint64)(mc) + UNMASK(mc_pa->mchunk_size)))) & 1ULL) {
    malloc_chunk *next_chunk = (malloc_chunk *)((uint64)(mc) + UNMASK(mc_pa->mchunk_size));
    merge_back(next_chunk);
  }
  if (mc_pa->mchunk_size & 2ULL) {
    merge_back(mc);
  } 
  return 0;
}

//
// [a0]: the syscall number; [a1] ... [a7]: arguments to the syscalls.
// returns the code of success, (e.g., 0 means success, fail for otherwise)
//
long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
  switch (a0) {
    case SYS_user_print:
      return sys_user_print((const char*)a1, a2);
    case SYS_user_exit:
      return sys_user_exit(a1);
    // added @lab2_2
    case SYS_user_allocate_page:
      return sys_user_allocate_page(a1);
    case SYS_user_free_page:
      return sys_user_free_page(a1);
    default:
      panic("Unknown syscall %ld \n", a0);
  }
}
