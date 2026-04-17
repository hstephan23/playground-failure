/* Unit tests for chaos_mem (arena, redzones, free semantics).
 * Plain assert() — exit non-zero on first failure. */

#include "playground/chaos_mem.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    /* basic alloc + free */
    chaos_arena_t *a = chaos_arena_create(64 * 1024, CHAOS_ARENA_REDZONES);
    assert(a);

    void *p = chaos_arena_alloc(a, 32);
    assert(p);
    /* user data is writable */
    memset(p, 'A', 32);
    assert(chaos_arena_check(a) == 0);

    /* clean free returns 0 */
    assert(chaos_arena_free(a, p) == 0);

    /* double-free returns -1 */
    assert(chaos_arena_free(a, p) == -1);

    /* OOB write past 32-byte alloc into post-redzone is detected */
    void *q = chaos_arena_alloc(a, 32);
    assert(q);
    memset(q, 'X', 64);   /* 32 bytes too many */
    int corrupted = chaos_arena_check(a);
    assert(corrupted >= 1);

    /* destroying an arena with corrupted blocks is fine */
    chaos_arena_destroy(a);

    /* tiny arena: rejected with EINVAL (Phase 3.2 tightened the contract) */
    chaos_arena_t *small = chaos_arena_create(128, CHAOS_ARENA_REDZONES);
    assert(small == NULL);

    printf("test_arena: ok\n");
    return 0;
}
