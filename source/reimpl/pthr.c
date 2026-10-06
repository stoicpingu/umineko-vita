/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022      GrapheneCt
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/*
 * Engine-facing pthread primitives are implemented on raw SceKernel
 * objects, NOT on vitasdk/newlib's pthread (pte). Threads created through
 * Vita SDL2's SDL_CreateThread (all of the engine's async queue workers:
 * loadVideoFramesQueue, playSoundQueue, loadImageQueue, ...) are plain
 * sceKernelCreateThread threads with no pte per-thread data. Any pte
 * primitive touched from such a thread either data-aborts
 * (pte_osSemaphoreCancellablePend dereferences NULL thread data — the
 * video-start crash) or silently corrupts state (pte mutex ownership
 * tracking — the historical "heap corruption" class). Kernel mutexes,
 * semaphores and TID-keyed TLS work from ANY thread.
 *
 * pthread_create/join/detach still use vitasdk pthread: handles from
 * pthread_create are only ever passed back to join/detach, and pte
 * threads can use the raw primitives below just fine.
 */

#include "reimpl/pthr.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "utils/utils.h"
#include "utils/logger.h"

#define BIONIC_PTHREAD_COND_INITIALIZER              0
#define BIONIC_PTHREAD_MUTEX_INITIALIZER             0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER   0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER  0x8000

enum {
    BIONIC_PTHREAD_MUTEX_NORMAL = 0,
    BIONIC_PTHREAD_MUTEX_RECURSIVE = 1,
    BIONIC_PTHREAD_MUTEX_ERRORCHECK = 2,

    BIONIC_PTHREAD_MUTEX_ERRORCHECK_NP = BIONIC_PTHREAD_MUTEX_ERRORCHECK,
    BIONIC_PTHREAD_MUTEX_RECURSIVE_NP  = BIONIC_PTHREAD_MUTEX_RECURSIVE,

    BIONIC_PTHREAD_MUTEX_DEFAULT = BIONIC_PTHREAD_MUTEX_NORMAL
};

#define PTHR_INLINE static inline __attribute__((always_inline))

#define PMTX_MAGIC 0x584d5450u /* 'PTMX' */
#define PCND_MAGIC 0x444e4350u /* 'PCND' */

/*
 * Mutex/cond structs are TYPE-STABLE: destroy parks them on a recycle
 * list instead of free()ing. A waiter can legally still be inside
 * pthread_cond_wait when another thread signals + destroys the cond; if
 * the struct returned to the heap, the waiter's bookkeeping writes would
 * corrupt freed chunks. Parked structs only ever hold these structs, so
 * a late toucher hits valid (worst case recycled) memory: a spurious
 * wakeup, never heap corruption.
 */
typedef struct vita_mutex_s {
    uint32_t magic;
    SceUID uid;
    int kind;
    struct vita_mutex_s *next_free;
} vita_mutex_t;

typedef struct vita_cond_s {
    uint32_t magic;
    SceUID sem;
    int waiters;
    struct vita_cond_s *next_free;
    int lock_inited; /* the lw-mutex survives recycling */
    SceKernelLwMutexWork lock __attribute__((aligned(8)));
} vita_cond_t;

static vita_mutex_t *mutex_recycle_list;
static vita_cond_t *cond_recycle_list;

static SceKernelLwMutexWork pthr_mutex;
static volatile short int pthr_mutex_inited = 0;

#define PTHR_LOCK \
    if (!pthr_mutex_inited) { \
        int ret = sceKernelCreateLwMutex(&pthr_mutex, "pthr_lock", 0, 0, NULL); \
        if (ret < 0) { \
            sceClibPrintf("Error: failed to create pthr mutex: 0x%x\n", ret); \
            return 0; \
        } \
        pthr_mutex_inited = 1; \
    } \
    sceKernelLockLwMutex(&pthr_mutex, 1, NULL);

#define PTHR_UNLOCK \
    if (pthr_mutex_inited) { \
        sceKernelUnlockLwMutex(&pthr_mutex, 1); \
    }

/* Bionic CLOCK_REALTIME absolute deadline -> remaining microseconds. */
static long long abstime_remaining_us(const struct timespec *abstime) {
    struct timeval now;
    gettimeofday(&now, NULL);
    long long now_us = (long long) now.tv_sec * 1000000LL + now.tv_usec;
    long long deadline_us = (long long) abstime->tv_sec * 1000000LL +
                            abstime->tv_nsec / 1000;
    return deadline_us - now_us;
}

/* ------------------------------------------------------------------ */
/* Mutexes                                                            */
/* ------------------------------------------------------------------ */

static int bionic_mutex_static_kind(uint32_t word, int *kind) {
    switch (word) {
        case BIONIC_PTHREAD_MUTEX_INITIALIZER:
            *kind = BIONIC_PTHREAD_MUTEX_NORMAL;
            return 1;
        case BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER:
            *kind = BIONIC_PTHREAD_MUTEX_RECURSIVE;
            return 1;
        case BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER:
            *kind = BIONIC_PTHREAD_MUTEX_ERRORCHECK;
            return 1;
        default:
            return 0;
    }
}

static vita_mutex_t *vita_mutex_create(int kind) {
    vita_mutex_t *m = NULL;

    PTHR_LOCK
    if (mutex_recycle_list) {
        m = mutex_recycle_list;
        mutex_recycle_list = m->next_free;
    }
    PTHR_UNLOCK
    if (!m) {
        m = calloc(1, sizeof(vita_mutex_t));
        if (!m)
            return NULL;
    }

    unsigned int attr = 0;
    if (kind == BIONIC_PTHREAD_MUTEX_RECURSIVE)
        attr |= SCE_KERNEL_MUTEX_ATTR_RECURSIVE;

    m->uid = sceKernelCreateMutex("so_mtx", attr, 0, NULL);
    if (m->uid < 0) {
        PTHR_LOCK
        m->next_free = mutex_recycle_list;
        mutex_recycle_list = m;
        PTHR_UNLOCK
        return NULL;
    }
    m->kind = kind;
    m->next_free = NULL;
    m->magic = PMTX_MAGIC;
    return m;
}

/*
 * The bionic mutex is a single 32-bit word. After init it holds a pointer
 * to our vita_mutex_t (heap pointers never collide with the static
 * initializer values 0/0x4000/0x8000).
 */
static vita_mutex_t *vita_mutex_get(pthread_mutex_t_bionic *mutex, int create) {
    uint32_t word = (uint32_t) (uintptr_t) mutex->real_ptr;
    int kind;

    if (!bionic_mutex_static_kind(word, &kind)) {
        vita_mutex_t *m = (vita_mutex_t *) (uintptr_t) word;
        if (m && m->magic == PMTX_MAGIC)
            return m;
        return NULL;
    }

    if (!create)
        return NULL;

    PTHR_LOCK
    /* Re-check: another thread may have initialized it meanwhile. */
    word = (uint32_t) (uintptr_t) mutex->real_ptr;
    if (!bionic_mutex_static_kind(word, &kind)) {
        PTHR_UNLOCK
        vita_mutex_t *m = (vita_mutex_t *) (uintptr_t) word;
        if (m && m->magic == PMTX_MAGIC)
            return m;
        return NULL;
    }
    vita_mutex_t *m = vita_mutex_create(kind);
    if (m)
        mutex->real_ptr = (pthread_mutex_t *) m;
    PTHR_UNLOCK
    return m;
}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr) {
    if (!attr) return EINVAL;
    *(int *) attr = BIONIC_PTHREAD_MUTEX_NORMAL;
    return 0;
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type) {
    if (!attr) return EINVAL;
    if (type < BIONIC_PTHREAD_MUTEX_NORMAL || type > BIONIC_PTHREAD_MUTEX_ERRORCHECK)
        return EINVAL;
    *(int *) attr = type;
    return 0;
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr) {
    if (!attr) return EINVAL;
    *(int *) attr = 0;
    return 0;
}

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *mutex,
                                const pthread_mutexattr_t *attr) {
    if (!mutex) return EINVAL;

    int kind = BIONIC_PTHREAD_MUTEX_NORMAL;
    if (attr)
        kind = *(const int *) attr;

    vita_mutex_t *m = vita_mutex_create(kind);
    if (!m)
        return ENOMEM;
    mutex->real_ptr = (pthread_mutex_t *) m;
    return 0;
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex) {
    if (!mutex) return 0;
    vita_mutex_t *m = vita_mutex_get(mutex, 0);
    mutex->real_ptr = NULL;
    if (m) {
        m->magic = 0;
        sceKernelDeleteMutex(m->uid);
        m->uid = -1;
        PTHR_LOCK
        m->next_free = mutex_recycle_list;
        mutex_recycle_list = m;
        PTHR_UNLOCK
    }
    return 0;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex) {
    if (!mutex) return EINVAL;
    vita_mutex_t *m = vita_mutex_get(mutex, 1);
    if (!m) return EINVAL;
    if (sceKernelLockMutex(m->uid, 1, NULL) < 0)
        return EDEADLK;
    return 0;
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex) {
    if (!mutex) return EINVAL;
    vita_mutex_t *m = vita_mutex_get(mutex, 1);
    if (!m) return EINVAL;
    if (sceKernelTryLockMutex(m->uid, 1) < 0)
        return EBUSY;
    return 0;
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex) {
    if (!mutex) return EINVAL;
    vita_mutex_t *m = vita_mutex_get(mutex, 0);
    if (!m) return EINVAL;
    if (sceKernelUnlockMutex(m->uid, 1) < 0)
        return EPERM;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Condition variables (semaphore + waiter count; spurious wakeups are */
/* possible and allowed — libc++/ffmpeg/SDL all re-check predicates).  */
/* ------------------------------------------------------------------ */

static vita_cond_t *vita_cond_create(void) {
    vita_cond_t *c = NULL;

    PTHR_LOCK
    if (cond_recycle_list) {
        c = cond_recycle_list;
        cond_recycle_list = c->next_free;
    }
    PTHR_UNLOCK
    if (!c) {
        c = calloc(1, sizeof(vita_cond_t));
        if (!c)
            return NULL;
    }

    c->sem = sceKernelCreateSema("so_cond", 0, 0, 0x7fffffff, NULL);
    if (c->sem < 0)
        goto park;

    /* The lw-mutex is created once per struct and survives recycling, so
     * a stale waiter from a destroyed incarnation always locks valid
     * kernel state. */
    if (!c->lock_inited) {
        if (sceKernelCreateLwMutex(&c->lock, "so_cond_l", 0, 0, NULL) < 0) {
            sceKernelDeleteSema(c->sem);
            goto park;
        }
        c->lock_inited = 1;
    }
    c->waiters = 0;
    c->next_free = NULL;
    c->magic = PCND_MAGIC;
    return c;

park:
    PTHR_LOCK
    c->next_free = cond_recycle_list;
    cond_recycle_list = c;
    PTHR_UNLOCK
    return NULL;
}

static vita_cond_t *vita_cond_get(pthread_cond_t_bionic *cond, int create) {
    uint32_t word = (uint32_t) (uintptr_t) cond->real_ptr;

    if (word != BIONIC_PTHREAD_COND_INITIALIZER) {
        vita_cond_t *c = (vita_cond_t *) (uintptr_t) word;
        if (c && c->magic == PCND_MAGIC)
            return c;
        return NULL;
    }

    if (!create)
        return NULL;

    PTHR_LOCK
    word = (uint32_t) (uintptr_t) cond->real_ptr;
    if (word != BIONIC_PTHREAD_COND_INITIALIZER) {
        PTHR_UNLOCK
        vita_cond_t *c = (vita_cond_t *) (uintptr_t) word;
        if (c && c->magic == PCND_MAGIC)
            return c;
        return NULL;
    }
    vita_cond_t *c = vita_cond_create();
    if (c)
        cond->real_ptr = (pthread_cond_t *) c;
    PTHR_UNLOCK
    return c;
}

int pthread_condattr_init_soloader(pthread_condattr_t *attr) {
    if (!attr) return EINVAL;
    *(int *) attr = 0;
    return 0;
}

int pthread_condattr_destroy_soloader(pthread_condattr_t *attr) {
    if (!attr) return EINVAL;
    return 0;
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond,
                               const pthread_condattr_t *attr) {
    (void) attr; /* bionic clock attr ignored: all waits use REALTIME math */
    if (!cond) return EINVAL;
    vita_cond_t *c = vita_cond_create();
    if (!c)
        return ENOMEM;
    cond->real_ptr = (pthread_cond_t *) c;
    return 0;
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond) {
    if (!cond) return 0;
    vita_cond_t *c = vita_cond_get(cond, 0);
    cond->real_ptr = NULL;
    if (c) {
        c->magic = 0;
        /* Deleting the sema wakes any in-flight waiter with an error; the
         * lw-mutex and the struct stay valid (recycled, never freed). */
        sceKernelDeleteSema(c->sem);
        c->sem = -1;
        PTHR_LOCK
        c->next_free = cond_recycle_list;
        cond_recycle_list = c;
        PTHR_UNLOCK
    }
    return 0;
}

static int vita_cond_wait_common(pthread_cond_t_bionic *cond,
                                 pthread_mutex_t_bionic *mutex,
                                 const struct timespec *abstime) {
    if (!cond || !mutex) return EINVAL;

    vita_cond_t *c = vita_cond_get(cond, 1);
    vita_mutex_t *m = vita_mutex_get(mutex, 1);
    if (!c || !m) return EINVAL;

    SceUInt timeout = 0;
    SceUInt *timeout_ptr = NULL;
    if (abstime) {
        long long remaining = abstime_remaining_us(abstime);
        if (remaining < 1)
            remaining = 1;
        timeout = (SceUInt) remaining;
        timeout_ptr = &timeout;
    }

    sceKernelLockLwMutex(&c->lock, 1, NULL);
    c->waiters++;
    sceKernelUnlockLwMutex(&c->lock, 1);

    sceKernelUnlockMutex(m->uid, 1);

    int wr = sceKernelWaitSema(c->sem, 1, timeout_ptr);

    int result = 0;
    if (wr < 0) {
        /*
         * Timed out (or wait error). A signal may have been posted for us
         * concurrently with the timeout: consume it and report success,
         * otherwise remove ourselves from the waiter count.
         */
        sceKernelLockLwMutex(&c->lock, 1, NULL);
        if (sceKernelPollSema(c->sem, 1) >= 0) {
            result = 0;
        } else {
            if (c->waiters > 0)
                c->waiters--;
            result = abstime ? ETIMEDOUT : EINVAL;
        }
        sceKernelUnlockLwMutex(&c->lock, 1);
    }

    sceKernelLockMutex(m->uid, 1, NULL);
    return result;
}

int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond,
                               pthread_mutex_t_bionic *mutex) {
    return vita_cond_wait_common(cond, mutex, NULL);
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond,
                                    pthread_mutex_t_bionic *mutex,
                                    struct timespec *abstime) {
    if (!abstime) return EINVAL;
    return vita_cond_wait_common(cond, mutex, abstime);
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond) {
    if (!cond) return EINVAL;
    vita_cond_t *c = vita_cond_get(cond, 1);
    if (!c) return EINVAL;

    sceKernelLockLwMutex(&c->lock, 1, NULL);
    if (c->waiters > 0) {
        c->waiters--;
        sceKernelSignalSema(c->sem, 1);
    }
    sceKernelUnlockLwMutex(&c->lock, 1);
    return 0;
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond) {
    if (!cond) return EINVAL;
    vita_cond_t *c = vita_cond_get(cond, 1);
    if (!c) return EINVAL;

    sceKernelLockLwMutex(&c->lock, 1, NULL);
    int n = c->waiters;
    c->waiters = 0;
    if (n > 0)
        sceKernelSignalSema(c->sem, n);
    sceKernelUnlockLwMutex(&c->lock, 1);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Thread identity / scheduling — must never dereference pte handles, */
/* because pthread_self now returns raw kernel TIDs.                  */
/* ------------------------------------------------------------------ */

pthread_t pthread_self_soloader() {
    return (pthread_t) sceKernelGetThreadId();
}

int pthread_equal_soloader(const pthread_t t1, const pthread_t t2) {
    return t1 == t2;
}

int pthread_kill_soloader(pthread_t thread, int sig) {
    (void) thread;
    (void) sig;
    return 0;
}

int pthread_setschedparam_soloader(pthread_t thread, int policy,
                                   const struct sched_param *param) {
    (void) thread;
    (void) policy;
    (void) param;
    return 0;
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy,
                                   struct sched_param *param) {
    (void) thread;
    if (policy)
        *policy = 0; /* SCHED_NORMAL */
    if (param)
        memset(param, 0, sizeof(*param));
    return 0;
}

int sched_yield_soloader(void) {
    sceKernelDelayThread(1);
    return 0;
}

/* ------------------------------------------------------------------ */
/* TLS keys (used by the engine's emutls for thread_local). TID-keyed */
/* so they work from SDL-created (non-pte) threads. Slots are lazily  */
/* reclaimed from dead threads when the table fills up. Key           */
/* destructors are NOT run at thread exit (engine workers are         */
/* long-lived queues; the leak is bounded and harmless).              */
/* ------------------------------------------------------------------ */

#define PTHR_MAX_KEYS 64
#define PTHR_MAX_TLS_THREADS 96

static struct {
    int used;
    void (*destructor)(void *);
} tls_keys[PTHR_MAX_KEYS];

static struct tls_slot {
    volatile int tid;
    void *vals[PTHR_MAX_KEYS];
} tls_slots[PTHR_MAX_TLS_THREADS];

static void tls_reclaim_dead_slots(void) {
    for (int i = 0; i < PTHR_MAX_TLS_THREADS; i++) {
        int tid = tls_slots[i].tid;
        if (tid == 0)
            continue;
        SceKernelThreadInfo info;
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(tid, &info) < 0) {
            memset(tls_slots[i].vals, 0, sizeof(tls_slots[i].vals));
            tls_slots[i].tid = 0;
        }
    }
}

static struct tls_slot *tls_self(int create) {
    int tid = sceKernelGetThreadId();

    for (int i = 0; i < PTHR_MAX_TLS_THREADS; i++)
        if (tls_slots[i].tid == tid)
            return &tls_slots[i];

    if (!create)
        return NULL;

    for (int attempt = 0; attempt < 2; attempt++) {
        for (int i = 0; i < PTHR_MAX_TLS_THREADS; i++) {
            if (tls_slots[i].tid == 0 &&
                __sync_bool_compare_and_swap(&tls_slots[i].tid, 0, tid)) {
                memset(tls_slots[i].vals, 0, sizeof(tls_slots[i].vals));
                return &tls_slots[i];
            }
        }
        PTHR_LOCK
        tls_reclaim_dead_slots();
        PTHR_UNLOCK
    }
    l_error("pthr: TLS thread slots exhausted");
    return NULL;
}

int pthread_key_create_soloader(int *key, void (*destructor)(void *)) {
    if (!key) return EINVAL;
    PTHR_LOCK
    for (int i = 0; i < PTHR_MAX_KEYS; i++) {
        if (!tls_keys[i].used) {
            tls_keys[i].used = 1;
            tls_keys[i].destructor = destructor;
            *key = i + 1;
            PTHR_UNLOCK
            return 0;
        }
    }
    PTHR_UNLOCK
    return EAGAIN;
}

int pthread_key_delete_soloader(int key) {
    if (key < 1 || key > PTHR_MAX_KEYS) return EINVAL;
    PTHR_LOCK
    tls_keys[key - 1].used = 0;
    tls_keys[key - 1].destructor = NULL;
    for (int i = 0; i < PTHR_MAX_TLS_THREADS; i++)
        tls_slots[i].vals[key - 1] = NULL;
    PTHR_UNLOCK
    return 0;
}

void *pthread_getspecific_soloader(int key) {
    if (key < 1 || key > PTHR_MAX_KEYS)
        return NULL;
    struct tls_slot *slot = tls_self(0);
    if (!slot)
        return NULL;
    return slot->vals[key - 1];
}

int pthread_setspecific_soloader(int key, const void *value) {
    if (key < 1 || key > PTHR_MAX_KEYS) return EINVAL;
    struct tls_slot *slot = tls_self(1);
    if (!slot)
        return ENOMEM;
    slot->vals[key - 1] = (void *) value;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Thread creation — still vitasdk pthread (pte): handles returned by */
/* pthread_create are only consumed by join/detach below.             */
/* ------------------------------------------------------------------ */

// null check for `attr` must be performed before this
PTHR_INLINE int _attr_t_static_init(pthread_attr_t_bionic * attr) {
    if (attr->magic != 0x42424242) {
        attr->magic = 0x42424242;
        attr->real_ptr = malloc(sizeof(pthread_attr_t));
        return pthread_attr_init(attr->real_ptr);
    }
    return 0;
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param) {
    int ret;

    if (!attr) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 512 * 1024);
        ret = pthread_create(thread, &a, start, param);
        pthread_attr_destroy(&a);
    } else{
        _attr_t_static_init((pthread_attr_t_bionic *) attr);
        // Honor an explicitly requested stack size; only enforce a floor so
        // threads created with pte's small default don't overflow.
        size_t requested = 0;
        if (pthread_attr_getstacksize(attr->real_ptr, &requested) != 0 ||
            requested < 512 * 1024)
            pthread_attr_setstacksize(attr->real_ptr, 512 * 1024);
        ret = pthread_create(thread, attr->real_ptr, start, param);
    }

    return ret;
}

int pthread_join_soloader(pthread_t thread, void **value_ptr)
{
    if (!thread)
        return EINVAL;
    return pthread_join(thread, value_ptr);
}

int pthread_detach_soloader(pthread_t thread)
{
    if (!thread)
        return EINVAL;
    return pthread_detach(thread);
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return EINVAL;

    return _attr_t_static_init(attr);
}

int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return 0;
    if (attr->magic != 0x42424242) return 0;

    int ret = pthread_attr_destroy(attr->real_ptr);
    free(attr->real_ptr);
    attr->magic = 0x0;

    return ret;
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state)
{
    if (!attr) return -1;
    _attr_t_static_init(attr);
    state = !state; // pthread-embedded has JOINABLE/DETACHED swapped compared to BIONIC...
    return pthread_attr_setdetachstate(attr->real_ptr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
    if (!attr) return -1;
    _attr_t_static_init(attr);
    return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine)
        return EINVAL;

    /*
     * 0 = never run, 1 = running, 2 = done. Only CAS 0->1; an unconditional
     * test-and-set would knock a completed control (2) back to 1 and spin
     * forever.
     */
    int previous = __sync_val_compare_and_swap(once_control, 0, 1);
    if (previous == 0) {
        (*init_routine)();
        __sync_synchronize();
        *once_control = 2;
    } else if (previous == 1) {
        while (*once_control == 1)
            sceKernelDelayThread(100);
    }
    return 0;
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(pthread_t thread, const char* thread_name) {
    if (thread == 0 || thread_name == NULL) {
        return EINVAL;
    }
    size_t thread_name_len = strlen(thread_name);
    if (thread_name_len >= MAX_TASK_COMM_LEN) {
        return ERANGE;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Semaphores (raw SceKernel, unchanged)                              */
/* ------------------------------------------------------------------ */

int sem_destroy_soloader(int * uid) {
    if (sceKernelDeleteSema(*uid) < 0)
        return -1;
    return 0;
}

int sem_getvalue_soloader (int * uid, int * sval) {
    SceKernelSemaInfo info;
    info.size = sizeof(SceKernelSemaInfo);

    if (!sval) {
        errno = EINVAL;
        return -1;
    }
    if (sceKernelGetSemaInfo(*uid, &info) < 0) return -1;
    *sval = info.currentCount;
    return 0;
}

int sem_init_soloader (int * uid, int pshared, unsigned int value) {
    *uid = sceKernelCreateSema("sema", 0, (int) value, 0x7fffffff, NULL);
    if (*uid < 0)
        return -1;
    return 0;
}

int sem_post_soloader (int * uid) {
    if (sceKernelSignalSema(*uid, 1) < 0)
        return -1;
    return 0;
}

/*
 * abstime is an absolute CLOCK_REALTIME deadline (bionic semantics; SDL's
 * POSIX semaphores compute it from gettimeofday). Wait for the remaining
 * relative time and report ETIMEDOUT properly — the engine's media frame
 * queue and async handshakes depend on real timed-wait semantics.
 */
int sem_timedwait_soloader (int * uid, const struct timespec * abstime) {
    if (sceKernelPollSema(*uid, 1) >= 0)
        return 0;

    if (!abstime) {
        errno = EINVAL;
        return -1;
    }

    long long remaining = abstime_remaining_us(abstime);

    if (remaining <= 0) {
        errno = ETIMEDOUT;
        return -1;
    }

    SceUInt timeout = (SceUInt) remaining;
    if (sceKernelWaitSema(*uid, 1, &timeout) < 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

int sem_trywait_soloader (int * uid) {
    if (sceKernelPollSema(*uid, 1) < 0) {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

int sem_wait_soloader (int * uid) {
    if (sceKernelWaitSema(*uid, 1, NULL) < 0)
        return -1;
    return 0;
}
