/* Unit tests for chaos_thread (spawn, join, cooperative cancel). */

#include "playground/chaos_thread.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

/* ---- basic spawn + join ---- */

static void *write_42(void *arg) {
    int *p = (int *)arg;
    *p = 42;
    return NULL;
}

static void test_spawn_join(void) {
    int v = 0;
    chaos_thread_t *t = chaos_thread_spawn(write_42, &v, "writer");
    assert(t);
    assert(chaos_thread_join(t) == 0);
    assert(v == 42);
}

/* ---- cooperative cancel via chaos_thread_should_stop ---- */

typedef struct {
    chaos_thread_t *self;       /* set by main after spawn returns */
    int             ran;
    pthread_mutex_t mu;
    pthread_cond_t  ready;
    int             ready_flag;
} spinner_arg_t;

static void *spinner(void *arg) {
    /* This test exercises the COOPERATIVE path specifically. Disable
     * pthread_cancel so it can't shortcut the cooperative flag check. */
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    spinner_arg_t *a = (spinner_arg_t *)arg;
    /* wait for main to publish self */
    pthread_mutex_lock(&a->mu);
    while (!a->ready_flag) pthread_cond_wait(&a->ready, &a->mu);
    pthread_mutex_unlock(&a->mu);

    while (!chaos_thread_should_stop(a->self)) {
        usleep(500);
    }
    a->ran = 1;
    return NULL;
}

static void test_cooperative_cancel(void) {
    spinner_arg_t a = {
        .self       = NULL,
        .ran        = 0,
        .mu         = PTHREAD_MUTEX_INITIALIZER,
        .ready      = PTHREAD_COND_INITIALIZER,
        .ready_flag = 0,
    };
    a.self = chaos_thread_spawn(spinner, &a, "spinner");
    assert(a.self);

    /* before cancel, flag is 0 */
    assert(chaos_thread_should_stop(a.self) == 0);

    pthread_mutex_lock(&a.mu);
    a.ready_flag = 1;
    pthread_cond_broadcast(&a.ready);
    pthread_mutex_unlock(&a.mu);

    usleep(5000);

    chaos_thread_cancel(a.self);   /* sets flag + pthread_cancel */
    assert(chaos_thread_join(a.self) == 0);
    assert(a.ran == 1);
}

int main(void) {
    test_spawn_join();
    test_cooperative_cancel();
    printf("test_thread: ok\n");
    return 0;
}
