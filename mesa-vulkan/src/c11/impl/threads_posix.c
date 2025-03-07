/*
 * SPDX-License-Identifier: BSL-1.0
 * Copyright yohhoy 2012.
 */
#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <stdint.h> /* for intptr_t */

#include "c11/threads.h"

/*
Configuration macro:

  EMULATED_THREADS_USE_NATIVE_TIMEDLOCK
    Use pthread_mutex_timedlock() for `mtx_timedlock()'
    Otherwise use mtx_trylock() + *busy loop* emulation.
*/
#if !defined(__CYGWIN__) && !defined(__APPLE__) && !defined(__NetBSD__)
#define EMULATED_THREADS_USE_NATIVE_TIMEDLOCK
#endif

/*---------------------------- types ----------------------------*/

/*
Implementation limits:
  - Conditionally emulation for "mutex with timeout"
    (see EMULATED_THREADS_USE_NATIVE_TIMEDLOCK macro)
*/
struct impl_thrd_param {
    thrd_start_t func;
    void *arg;
};

static void *
impl_thrd_routine(void *p)
{
    struct impl_thrd_param pack = *((struct impl_thrd_param *)p);
    free(p);
    return (void*)(intptr_t)pack.func(pack.arg);
}


/*--------------- 7.25.2 Initialization functions ---------------*/
// 7.25.2.1
void
call_once(once_flag *flag, void (*func)(void))
{
    pthread_once(flag, func);
}


/*------------- 7.25.3 Condition variable functions -------------*/
// 7.25.3.1
int
cnd_broadcast(cnd_t *cond)
{
    assert(cond != NULL);
    return (pthread_cond_broadcast(cond) == 0) ? thrd_success : thrd_error;
}

// 7.25.3.2
void
cnd_destroy(cnd_t *cond)
{
    assert(cond);
    pthread_cond_destroy(cond);
}

// 7.25.3.3
int
cnd_init(cnd_t *cond)
{
    assert(cond != NULL);
    return (pthread_cond_init(cond, NULL) == 0) ? thrd_success : thrd_error;
}

// 7.25.3.4
int
cnd_signal(cnd_t *cond)
{
    assert(cond != NULL);
    return (pthread_cond_signal(cond) == 0) ? thrd_success : thrd_error;
}

// 7.25.3.5
int
cnd_timedwait(cnd_t *cond, mtx_t *mtx, const struct timespec *abs_time)
{
    int rt;

    assert(mtx != NULL);
    assert(cond != NULL);
    assert(abs_time != NULL);

    rt = pthread_cond_timedwait(cond, mtx, abs_time);
    if (rt == ETIMEDOUT)
        return thrd_timedout;
    return (rt == 0) ? thrd_success : thrd_error;
}

// 7.25.3.6
int
cnd_wait(cnd_t *cond, mtx_t *mtx)
{
    assert(mtx != NULL);
    assert(cond != NULL);
    return (pthread_cond_wait(cond, mtx) == 0) ? thrd_success : thrd_error;
}


/*-------------------- 7.25.4 Mutex functions --------------------*/
// 7.25.4.1
void
mtx_destroy(mtx_t *mtx)
{
    assert(mtx != NULL);
    pthread_mutex_destroy(mtx);
}

/*
 * XXX: Workaround when building with -O0 and without pthreads link.
 *
 * In such cases constant folding and dead code elimination won't be
 * available, thus the compiler will always add the pthread_mutexattr*
 * functions into the binary. As we try to link, we'll fail as the
 * symbols are unresolved.
 *
 * Ideally we'll enable the optimisations locally, yet that does not
 * seem to work.
 *
 * So the alternative workaround is to annotate the symbols as weak.
 * Thus the linker will be happy and things don't clash when building
 * with -O1 or greater.
 */
#if defined(HAVE_FUNC_ATTRIBUTE_WEAK) && !defined(__CYGWIN__)
__attribute__((weak))
int pthread_mutexattr_init(pthread_mutexattr_t *attr);

__attribute__((weak))
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);

__attribute__((weak))
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
#endif

// 7.25.4.2
int
mtx_init(mtx_t *mtx, int type)
{
    pthread_mutexattr_t attr;
    assert(mtx != NULL);
    if (type != mtx_plain && type != mtx_timed
      && type != (mtx_plain|mtx_recursive)
      && type != (mtx_timed|mtx_recursive))
        return thrd_error;

    if ((type & mtx_recursive) == 0) {
        pthread_mutex_init(mtx, NULL);
        return thrd_success;
    }

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(mtx, &attr);
    pthread_mutexattr_destroy(&attr);
    return thrd_success;
}

// 7.25.4.3
int
mtx_lock(mtx_t *mtx)
{
    assert(mtx != NULL);
    return (pthread_mutex_lock(mtx) == 0) ? thrd_success : thrd_error;
}

static int
threads_timespec_compare(const struct timespec *a, const struct timespec *b)
{
    if (a->tv_sec < b->tv_sec) {
        return -1;
    } else if (a->tv_sec > b->tv_sec) {
        return 1;
    } else if (a->tv_nsec < b->tv_nsec) {
        return -1;
    } else if (a->tv_nsec > b->tv_nsec) {
        return 1;
    }
    return 0;
}

// 7.25.4.4
int
mtx_timedlock(mtx_t *mtx, const struct timespec *ts)
{
    assert(mtx != NULL);
    assert(ts != NULL);

    {
#ifdef EMULATED_THREADS_USE_NATIVE_TIMEDLOCK
    int rt;
    rt = pthread_mutex_timedlock(mtx, ts);
    if (rt == 0)
        return thrd_success;
    return (rt == ETIMEDOUT) ? thrd_timedout : thrd_error;
#else
    while (mtx_trylock(mtx) != thrd_success) {
        struct timespec now;
        if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
            return thrd_error;
        }
        if (threads_timespec_compare(ts, &now) < 0)
            return thrd_timedout;
        // busy loop!
        thrd_yield();
    }
    return thrd_success;
#endif
    }
}

// 7.25.4.5
int
mtx_trylock(mtx_t *mtx)
{
    assert(mtx != NULL);
    return (pthread_mutex_trylock(mtx) == 0) ? thrd_success : thrd_busy;
}

// 7.25.4.6
int
mtx_unlock(mtx_t *mtx)
{
    assert(mtx != NULL);
    return (pthread_mutex_unlock(mtx) == 0) ? thrd_success : thrd_error;
}


/*------------------- 7.25.5 Thread functions -------------------*/
// 7.25.5.1
int
thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
    struct impl_thrd_param *pack;
    assert(thr != NULL);
    pack = (struct impl_thrd_param *)malloc(sizeof(struct impl_thrd_param));
    if (!pack) return thrd_nomem;
    pack->func = func;
    pack->arg = arg;
    if (pthread_create(thr, NULL, impl_thrd_routine, pack) != 0) {
        free(pack);
        return thrd_error;
    }
    return thrd_success;
}

// 7.25.5.2
thrd_t
thrd_current(void)
{
    return pthread_self();
}

// 7.25.5.3
int
thrd_detach(thrd_t thr)
{
    return (pthread_detach(thr) == 0) ? thrd_success : thrd_error;
}

// 7.25.5.4
int
thrd_equal(thrd_t thr0, thrd_t thr1)
{
    return pthread_equal(thr0, thr1);
}

// 7.25.5.5
_Noreturn
void
thrd_exit(int res)
{
    pthread_exit((void*)(intptr_t)res);
}

// 7.25.5.6
int
thrd_join(thrd_t thr, int *res)
{
    void *code;
    if (pthread_join(thr, &code) != 0)
        return thrd_error;
    if (res)
        *res = (int)(intptr_t)code;
    return thrd_success;
}

// 7.25.5.7
int
thrd_sleep(const struct timespec *time_point, struct timespec *remaining)
{
    assert(time_point != NULL);
    return nanosleep(time_point, remaining);
}

// 7.25.5.8
void
thrd_yield(void)
{
    sched_yield();
}


/*----------- 7.25.6 Thread-specific storage functions -----------*/
// 7.25.6.1
int
tss_create(tss_t *key, tss_dtor_t dtor)
{
    assert(key != NULL);
    return (pthread_key_create(key, dtor) == 0) ? thrd_success : thrd_error;
}

// 7.25.6.2
void
tss_delete(tss_t key)
{
    pthread_key_delete(key);
}

// 7.25.6.3
void *
tss_get(tss_t key)
{
    return pthread_getspecific(key);
}

// 7.25.6.4
int
tss_set(tss_t key, void *val)
{
    return (pthread_setspecific(key, val) == 0) ? thrd_success : thrd_error;
}
