/* Linux only: thread stacks that are too small for this program get a second try with a bigger one.
 *
 * GT5's per-thread data (static TLS) is large, so glibc's smallest thread stack in this program is about
 * 344 KiB. A library that asks for less gets EINVAL from pthread_create. SDL 2.30.0's PulseAudio hotplug thread
 * asks for 256 KiB and then waits forever for a thread that never started, so the game stayed on a black screen
 * at start with sound through PulseAudio or PipeWire (GitHub #20; found by melroy89, SDL fixed it in 2.30.12).
 *
 * This pthread_create is the program's own, and a program's symbol comes before libc's for every library it
 * loads, SDL included. It passes each call on unchanged; only when that fails with EINVAL does it try once more
 * with the same settings and a bigger stack (1 MiB, or twice the request if that is more), and says so in the
 * log. A call that is fine the first time behaves exactly as before. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

typedef int (*gt5_create_fn)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);

__attribute__((visibility("default")))
int pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg)
{
    static gt5_create_fn real;
    gt5_create_fn fn = __atomic_load_n(&real, __ATOMIC_ACQUIRE);
    if (!fn) {
        fn = (gt5_create_fn)dlsym(RTLD_NEXT, "pthread_create");
        if (!fn) return EAGAIN;
        __atomic_store_n(&real, fn, __ATOMIC_RELEASE);
    }
    int rc = fn(thread, attr, start, arg);
    if (rc != EINVAL || !attr) return rc;

    size_t asked = 0;
    if (pthread_attr_getstacksize(attr, &asked) != 0) return rc;
    const size_t bigger = asked * 2 > ((size_t)1 << 20) ? asked * 2 : ((size_t)1 << 20);

    pthread_attr_t again;
    if (pthread_attr_init(&again) != 0) return rc;
    int v = 0;
    size_t guard = 0;
    struct sched_param param;
    if (pthread_attr_getdetachstate(attr, &v) == 0) pthread_attr_setdetachstate(&again, v);
    if (pthread_attr_getguardsize(attr, &guard) == 0) pthread_attr_setguardsize(&again, guard);
    if (pthread_attr_getscope(attr, &v) == 0) pthread_attr_setscope(&again, v);
    if (pthread_attr_getinheritsched(attr, &v) == 0) {
        pthread_attr_setinheritsched(&again, v);
        if (v == PTHREAD_EXPLICIT_SCHED) {
            if (pthread_attr_getschedpolicy(attr, &v) == 0) pthread_attr_setschedpolicy(&again, v);
            if (pthread_attr_getschedparam(attr, &param) == 0) pthread_attr_setschedparam(&again, &param);
        }
    }
    pthread_attr_setstacksize(&again, bigger);
    int rc2 = fn(thread, &again, start, arg);
    pthread_attr_destroy(&again);
    if (rc2 == 0)
        fprintf(stderr, "[threads] a library asked for a %zu KiB thread stack, under this program's smallest (its "
                        "per-thread data is large): started it with %zu KiB instead\n", asked >> 10, bigger >> 10);
    return rc2 == 0 ? 0 : rc;
}
