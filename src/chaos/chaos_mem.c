#include "playground/chaos_mem.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* ---- on-disk (well, in-arena) layout ----
 *  [block_hdr (8B)][pre-redzone (16B)][user data (round8(size) B)][post-redzone (16B)]
 *  user pointer returned to caller points at the start of "user data".
 */

#define ARENA_MAGIC_LIVE   0xA11A11AAu
#define ARENA_MAGIC_FREE   0xFEEDFEEDu
#define REDZONE_SIZE       16u
#define REDZONE_BYTE       0xCDu
#define POISON_BYTE        0xDEu

typedef struct {
    uint32_t magic;
    uint32_t size;     /* user-requested */
} block_hdr_t;

struct chaos_arena {
    uint8_t *base;
    size_t   total;
    size_t   used;
    unsigned flags;
};

static inline size_t round8(size_t n) { return (n + 7u) & ~(size_t)7u; }

static inline size_t block_total(size_t user_size) {
    return sizeof(block_hdr_t) + REDZONE_SIZE + round8(user_size) + REDZONE_SIZE;
}

chaos_arena_t *chaos_arena_create(size_t bytes, unsigned flags) {
    if (bytes < 4096) bytes = 4096;
    chaos_arena_t *a = (chaos_arena_t *)calloc(1, sizeof(*a));
    if (!a) return NULL;
    void *m = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_ANON | MAP_PRIVATE, -1, 0);
    if (m == MAP_FAILED) { free(a); return NULL; }
    a->base  = (uint8_t *)m;
    a->total = bytes;
    a->used  = 0;
    a->flags = flags;
    return a;
}

void chaos_arena_destroy(chaos_arena_t *a) {
    if (!a) return;
    if (a->base) munmap(a->base, a->total);
    free(a);
}

void *chaos_arena_alloc(chaos_arena_t *a, size_t n) {
    if (!a) return NULL;
    size_t need = block_total(n);
    if (a->used + need > a->total) return NULL;

    uint8_t     *p   = a->base + a->used;
    block_hdr_t *h   = (block_hdr_t *)p;
    h->magic = ARENA_MAGIC_LIVE;
    h->size  = (uint32_t)n;

    uint8_t *pre  = p + sizeof(*h);
    uint8_t *user = pre + REDZONE_SIZE;
    uint8_t *post = user + round8(n);

    if (a->flags & CHAOS_ARENA_REDZONES) {
        memset(pre,  REDZONE_BYTE, REDZONE_SIZE);
        memset(post, REDZONE_BYTE, REDZONE_SIZE);
    }

    a->used += need;
    return user;
}

int chaos_arena_free(chaos_arena_t *a, void *p) {
    if (!a || !p) return -1;
    uint8_t     *u = (uint8_t *)p;
    block_hdr_t *h = (block_hdr_t *)(u - REDZONE_SIZE - sizeof(*h));

    if ((uint8_t *)h < a->base || (uint8_t *)h >= a->base + a->total) return -1;
    if (h->magic == ARENA_MAGIC_FREE) return -1;   /* double-free */
    if (h->magic != ARENA_MAGIC_LIVE) return -1;   /* wild pointer */

    h->magic = ARENA_MAGIC_FREE;
    /* Poison user data so subsequent UAF reads return obvious garbage. */
    memset(u, POISON_BYTE, h->size);
    return 0;
}

int chaos_arena_check(chaos_arena_t *a) {
    if (!a) return -1;
    if (!(a->flags & CHAOS_ARENA_REDZONES)) return 0;

    int    corrupted = 0;
    size_t off       = 0;
    while (off < a->used) {
        block_hdr_t *h = (block_hdr_t *)(a->base + off);
        if (h->magic != ARENA_MAGIC_LIVE && h->magic != ARENA_MAGIC_FREE) {
            return -1;   /* arena structure itself is wrecked */
        }
        size_t pad   = round8(h->size);
        size_t blksz = sizeof(*h) + REDZONE_SIZE + pad + REDZONE_SIZE;

        uint8_t *pre  = a->base + off + sizeof(*h);
        uint8_t *post = pre + REDZONE_SIZE + pad;

        for (size_t i = 0; i < REDZONE_SIZE; ++i) {
            if (pre[i]  != REDZONE_BYTE) { corrupted++; break; }
        }
        for (size_t i = 0; i < REDZONE_SIZE; ++i) {
            if (post[i] != REDZONE_BYTE) { corrupted++; break; }
        }
        off += blksz;
    }
    return corrupted;
}
