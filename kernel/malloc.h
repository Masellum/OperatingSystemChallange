#ifndef _MALLOC_H_
#define _MALLOC_H_

#include "util/types.h"

#define CHUNK_FREE (1U);
#define CHUNK_PREV_FREE (1U);

#define UNMASK(msize) ((msize) & ~07ULL)
#define FOOTER(chunk) ((uint64)chunk + UNMASK(chunk->mchunk_size) - 8ULL)

typedef struct malloc_chunk
{
    // uint64 mchunk_prev_size;
    uint64 mchunk_size;
    struct malloc_chunk * fd;
    struct malloc_chunk * bk;
    struct malloc_chunk * prev;
} malloc_chunk;

void pushdown(malloc_chunk **mc);
malloc_chunk* pushup(malloc_chunk * const *mc);

void merge_back(malloc_chunk *mc);

void insert(malloc_chunk **root, malloc_chunk *element);

void remove(malloc_chunk *element);

#endif // _MALLOC_H_