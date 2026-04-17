#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct chaos_arena chaos_arena_t;

/* Flags for chaos_arena_create. */
#define CHAOS_ARENA_REDZONES   0x1u   /* 16-byte poisoned guard before & after each alloc */

/* Create an arena that owns a private mmap'd region of `bytes`. All allocations
 * within the arena are bounded by it — corruption stays inside the arena,
 * never touches libc's heap or other process state. Returns NULL on failure. */
chaos_arena_t *chaos_arena_create(size_t bytes, unsigned flags);

/* Bump-allocate `n` bytes from the arena. Allocations are 8-byte aligned and
 * (if CHAOS_ARENA_REDZONES) wrapped in poisoned guard bytes. NULL on OOM. */
void *chaos_arena_alloc(chaos_arena_t *a, size_t n);

/* Mark a block freed and overwrite its user data with 0xDE poison so subsequent
 * use-after-free reads return obvious garbage. Returns 0 on success, -1 if the
 * pointer isn't from this arena, is already freed (double-free), or is wild. */
int chaos_arena_free(chaos_arena_t *a, void *p);

/* Walk every allocated block and verify its redzones are intact. Returns the
 * number of corrupted redzones found (0 = clean). Use after any operation that
 * might have written out-of-bounds. */
int chaos_arena_check(chaos_arena_t *a);

/* munmap the region and free the arena descriptor. */
void chaos_arena_destroy(chaos_arena_t *a);
