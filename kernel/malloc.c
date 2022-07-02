#include "malloc.h"
#include "vmm.h"
#include "process.h"
#include "spike_interface/spike_utils.h"
#include "util/functions.h"

// #define swap(a, b) do { t = a; a = b; b = t; } while (0)
#define swap(a, b) \
  do {             \
    *a ^= *b;      \
    *b ^= *a;      \
    *a ^= *b;      \
  } while (0)

#define toswap(x) ((uint64*)(&(x)))
#define getpa(x) ((malloc_chunk *)(lookup_pa((pagetable_t)current->pagetable, (uint64)(x))))

static void swap_chunk(malloc_chunk *a, malloc_chunk *b) {
    malloc_chunk *a_pa = getpa(a), *b_pa = getpa(b);
    swap(toswap(a_pa->prev), toswap(b_pa->prev));
    swap(toswap(a_pa->bk), toswap(b_pa->bk));
    swap(toswap(a_pa->fd), toswap(b_pa->fd));
}

static void swap_parent_child(malloc_chunk * const p, malloc_chunk * const c) {
    malloc_chunk *p_pa = (malloc_chunk *)lookup_pa((pagetable_t)current->pagetable, (uint64)p);
    malloc_chunk *c_pa = (malloc_chunk *)lookup_pa((pagetable_t)current->pagetable, (uint64)c);
    if (c == p_pa->bk) {
        if (c_pa->bk != NULL) {
            getpa(c_pa->bk)->prev = p;
            // ((malloc_chunk *)(lookup_pa((pagetable_t)current->pagetable, c_pa->bk)))->prev = p;
            // c_pa->bk->prev = p;
        }
        if (c_pa->fd != NULL) { 
            getpa(c_pa->fd)->prev = p;
            // ((malloc_chunk *)(lookup_pa((pagetable_t)current->pagetable, c_pa->fd)))->prev = p;
            // c->fd->prev = p;
        }
        if (p_pa->fd != NULL) {
            getpa(p_pa->fd)->prev = p;
            // ((malloc_chunk *)(lookup_pa((pagetable_t)current->pagetable, p_pa->fd)))->prev = p;
            // p->fd->prev = c;
        }
        if (p_pa->prev != NULL) {
            if (p == getpa(p_pa->prev)->bk) {
            // if (p == ((malloc_chunk *)(lookup_pa((pagetable_t)current->pagetable, p_pa->prev)))->bk) {
            // if (p == p->prev->bk) {
                getpa(p_pa->prev)->bk = c;
                // p->prev->bk = c;
            } else {
                getpa(p_pa->fd)->fd = c;
                // p->prev->fd = c;
            }
        }
        c_pa->prev = p_pa->prev;
        p_pa->prev = c;
        p_pa->bk = c_pa->bk;
        c_pa->bk = p;
        swap(toswap(p_pa->fd), toswap(c_pa->fd));
    } else {
        if (c_pa->fd != NULL) {
            getpa(c_pa->fd)->prev = p;
            // c->fd->prev = p;
        }
        if (c_pa->bk != NULL) {
            getpa(c_pa->bk)->prev = p;
            c->bk->prev = p;
        }
        if (p_pa->bk != NULL) {
            getpa(p_pa->bk)->prev = c;
            // p->bk->prev = c;
        }
        if (p_pa->prev != NULL) {
            if (p == getpa(p_pa->prev)->fd) {
                getpa(p_pa->prev)->fd = c;
            } else {
                getpa(p_pa->prev)->bk = c;
            }
        }
        c_pa->prev = p_pa->prev;
        p_pa->prev = c;
        p_pa->fd = c_pa->fd;
        c_pa->fd = p;
        swap(toswap(p_pa->bk), toswap(p_pa->bk));
    }
}

void pushdown(malloc_chunk **mc) {
    malloc_chunk *mc_pa = (malloc_chunk *)lookup_pa((pagetable_t)current->pagetable, (uint64)(*mc));
    malloc_chunk *bk = (malloc_chunk *)lookup_pa((pagetable_t)current->pagetable, (uint64)(mc_pa)->bk);
    malloc_chunk *fd = (malloc_chunk *)lookup_pa((pagetable_t)current->pagetable, (uint64)(mc_pa)->fd);
    if (((bk == NULL) || (UNMASK(bk->mchunk_size) <= UNMASK(mc_pa->mchunk_size))) && 
        ((fd == NULL) || (UNMASK(fd->mchunk_size) <= UNMASK(mc_pa->mchunk_size)))) {
            return;
    }
    if ( (bk == NULL) && (fd != NULL) ) {
        swap_parent_child(*mc, mc_pa->fd);
        *mc = mc_pa->prev;
        mc_pa = getpa(*mc);
        pushdown(&mc_pa->fd);
        return;
    }
    if ( (fd == NULL) && (bk != NULL) ) {
        swap_parent_child(*mc, mc_pa->bk);
        *mc = mc_pa->prev;
        mc_pa = getpa(*mc);
        pushdown(&mc_pa->bk);
        return;
    }
    if ( (UNMASK(bk->mchunk_size) > UNMASK(mc_pa->mchunk_size)) && (UNMASK(fd->mchunk_size) > UNMASK(mc_pa->mchunk_size)) ) {
        if ( UNMASK(bk->mchunk_size) > UNMASK(fd->mchunk_size) ) {
            swap_parent_child(*mc, mc_pa->bk);
            *mc = mc_pa->prev;
            mc_pa = getpa(*mc);
            pushdown(&mc_pa->bk);
        } else {
            swap_parent_child(*mc, mc_pa->fd);
            *mc = mc_pa->prev;
            mc_pa = getpa(*mc);
            pushdown(&mc_pa->fd);
        }
        return;
    }
    if ( (UNMASK(fd->mchunk_size) < (UNMASK(bk->mchunk_size))) ) {
        swap_parent_child(*mc, mc_pa->bk);
        *mc = mc_pa->prev;
        mc_pa = getpa(*mc);
        pushdown(&mc_pa->bk);
    } else {
        swap_parent_child(*mc, mc_pa->fd);
        *mc = mc_pa->prev;
        mc_pa = getpa(*mc);
        pushdown(&mc_pa->fd);
    }
}

malloc_chunk* pushup(malloc_chunk * const *mc) {
    malloc_chunk *mc_pa = getpa(*mc);
    while (mc_pa->prev != NULL && UNMASK(getpa(mc_pa->prev)->mchunk_size) < UNMASK(mc_pa->mchunk_size)) {
        swap_parent_child(mc_pa->prev, *mc);
        mc_pa = getpa(mc);
    }
    if (mc_pa->prev == NULL) {
        return *mc;
    } else {
        return NULL;
    }
}

void merge_back(malloc_chunk *mc) {
    malloc_chunk *mc_pa = getpa(mc);
    uint64* prev_foot = (uint64*)lookup_pa((pagetable_t)current->pagetable, (uint64)(mc) - 8);
    malloc_chunk *prev = (malloc_chunk *)((uint64)(mc) - UNMASK(*prev_foot));
    malloc_chunk *prev_pa = getpa(prev);
    remove(mc);
    uint64 begin_va = ROUNDDOWN((uint64)(mc) - 8, PGSIZE), end_va = ROUNDUP((uint64)(mc) + sizeof(malloc_chunk), PGSIZE);
    if (begin_va == ROUNDDOWN((uint64)(prev), PGSIZE)) {
        begin_va += PGSIZE;
    }
    if (end_va == ROUNDUP((uint64)(mc) + UNMASK(mc_pa->mchunk_size) - 8ULL, PGSIZE)) {
        end_va -= PGSIZE;
    }
    user_vm_unmap((pagetable_t)current->pagetable, begin_va, end_va - begin_va, 1);
    prev_pa->mchunk_size += UNMASK(mc_pa->mchunk_size);
    *((uint64*)(lookup_pa((pagetable_t)current->pagetable, ((uint64)mc + UNMASK(mc_pa->mchunk_size) - 8ULL)))) = prev_pa->mchunk_size;
    if (pushup(&prev)) {
        current->first_free_chunk = prev;
    }
}

void insert(malloc_chunk **root, malloc_chunk *new_chunk) {
    malloc_chunk *p = *root, *e;
    e = getpa(p)->fd;
    while (e != NULL) {
        p = e;
        e = getpa(p)->fd;
    }
    getpa(p)->fd = new_chunk;
    malloc_chunk *new_chunk_pa = getpa(new_chunk);
    new_chunk_pa->prev = p;
    new_chunk_pa->bk = new_chunk_pa->fd = NULL;
    uint64 footer_pa = (lookup_pa((pagetable_t)current->pagetable, ((uint64)new_chunk + UNMASK(new_chunk_pa->mchunk_size) - 8ULL)));
    *((uint64*)(lookup_pa((pagetable_t)current->pagetable, ((uint64)new_chunk + UNMASK(new_chunk_pa->mchunk_size) - 8ULL)))) = new_chunk_pa->mchunk_size;
    if (pushup(&new_chunk)) {
        *root = new_chunk;
    }
}

void remove(malloc_chunk *to_be_removed) {
    malloc_chunk *p = to_be_removed, *e;
    malloc_chunk *tbr_pa = getpa(to_be_removed), *p_pa;
    e = getpa(p)->fd;
    while (e != NULL) {
        p = e;
        e = getpa(p)->fd;
    }
    if (p == to_be_removed) {
        if (tbr_pa->bk != NULL) {
            getpa(tbr_pa->bk)->prev = tbr_pa->prev;
        }
        if (tbr_pa->prev == NULL) {
            current->first_free_chunk = tbr_pa->bk;
        } else {
            if (p == getpa(tbr_pa->prev)->bk) {
                getpa(tbr_pa->prev)->bk = tbr_pa->bk;
            } else {
                getpa(tbr_pa->prev)->fd = tbr_pa->bk;
            }
        }
    } else if (p == getpa(to_be_removed)->fd) {
        p_pa = getpa(p);
        swap_parent_child(to_be_removed, p);
        p_pa->fd = tbr_pa->bk;
        if (p_pa->fd != NULL) getpa(p_pa->fd)->prev = p;
        pushdown(&p);
        p_pa = getpa(p);
        if (p_pa->prev == NULL) {
            current->first_free_chunk = p;
        }
    } else {
        // p_pa->fd == NULL
        // p_pa->prev != NULL
        // tbr_pa->fd != NULL
        p_pa = getpa(p);
        if (p == getpa(p_pa->prev)->bk) { 
            getpa(p_pa->prev)->bk = to_be_removed; 
        } else {
            getpa(p_pa->prev)->fd = to_be_removed; 
        }
        if (p_pa->bk != NULL) {
            getpa(p_pa->bk)->prev = to_be_removed;
        }

        if (tbr_pa->prev != NULL) {
            if (to_be_removed == getpa(tbr_pa->prev)->bk) {
                getpa(tbr_pa->prev)->bk = p;
            } else {
                getpa(tbr_pa->prev)->fd = p;
            }
        }
        if (tbr_pa->bk != NULL) {
            getpa(tbr_pa->bk)->prev = p;
        }
        getpa(tbr_pa->fd)->prev = p;
        swap(toswap(p_pa->bk), toswap(tbr_pa->bk));
        swap(toswap(p_pa->fd), toswap(tbr_pa->fd));
        swap(toswap(p_pa->prev), toswap(tbr_pa->prev));
        getpa(tbr_pa->prev)->fd = tbr_pa->bk;
        getpa(tbr_pa->bk)->prev = tbr_pa->prev;
        pushdown(&p);
        p_pa = getpa(p);
        if (p_pa->prev == NULL) {
            current->first_free_chunk = p;
        }
    }
}
