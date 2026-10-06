/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/mem.h"
#include "utils/logger.h"

#include <string.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdint.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>

static unsigned int alloc_log_count = 0;
static int alloc_log_guard = 0;

/* Success-path large-alloc tracing (free-memory snapshot per hit) is a
 * diagnostic, not a runtime need: each line costs a kernel memory query +
 * a log write during asset loads. Failures (ptr == NULL) always log. Set
 * to 1 when chasing memory issues. */
#ifndef PIVAS_ALLOC_TRACE
#define PIVAS_ALLOC_TRACE 0
#endif

static void log_large_alloc(const char *op, size_t size, void *ptr) {
    if (ptr && !PIVAS_ALLOC_TRACE)
        return;
    if (ptr && size < 512 * 1024)
        return;
    if (ptr && alloc_log_count++ >= 160)
        return;
    if (alloc_log_guard)
        return;

    alloc_log_guard = 1;

    SceKernelFreeMemorySizeInfo info;
    sceClibMemset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    int ret = sceKernelGetFreeMemorySize(&info);
    struct mallinfo heap = mallinfo();
    if (ret >= 0) {
        l_info("[alloc] %s size=%u ptr=%p free_user=%d free_cdram=%d free_phycont=%d heap_arena=%d heap_used=%d heap_free=%d heap_keep=%d",
               op ? op : "(null)",
               (unsigned)size,
               ptr,
               info.size_user,
               info.size_cdram,
               info.size_phycont,
               heap.arena,
               heap.uordblks,
               heap.fordblks,
               heap.keepcost);
    } else {
        l_error("[alloc] %s size=%u ptr=%p free_mem_query_failed=0x%08x heap_arena=%d heap_used=%d heap_free=%d heap_keep=%d",
                op ? op : "(null)", (unsigned)size, ptr, ret,
                heap.arena, heap.uordblks, heap.fordblks, heap.keepcost);
    }

    alloc_log_guard = 0;
}

void *sceClibMemclr(void *dst, size_t len) {
    return sceClibMemset(dst, 0, len);
}

/*
 * Heap guards: every allocation handed to the .so gets a 16-byte header
 * (magic, size, allocating caller) and a 4-byte tail canary. free/realloc
 * verify the tail, so a buffer overrun is reported at free time with the
 * culprit's allocation site (engine .so address = value - 0x98000000)
 * instead of crashing later inside newlib's _free_r on an unrelated thread.
 * Pointers without the magic (e.g. strdup'd inside newlib) are freed raw.
 */
#define GUARD_HEAD_MAGIC  0x47554152u
#define GUARD_FREED_MAGIC 0x46524545u
#define GUARD_TAIL_MAGIC  0x4C494154u
#define GUARD_HEAD_SIZE   16

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint32_t alloc_caller;
    uint32_t base_off; /* head minus malloc base (aligned allocations) */
} guard_head_t;

#define GUARD_BASE(head) ((void *)((char *)(head) - (head)->base_off))

static unsigned int guard_report_count = 0;

static void guard_report(const char *what, void *ptr, const guard_head_t *head,
                         void *caller, uint32_t tail_seen) {
    if (guard_report_count++ >= 32)
        return;
    l_error("[heap-guard] %s ptr=%p size=%u alloc_caller=%p free_caller=%p tail=0x%08x",
            what, ptr, head ? head->size : 0,
            head ? (void *)head->alloc_caller : NULL, caller, tail_seen);
}

/*
 * alignment must be a power of two. newlib malloc only guarantees 8, so
 * larger alignments over-allocate and slide the user pointer up; the
 * header records the offset back to the malloc base for free/realloc.
 * (ffmpeg's av_malloc uses memalign(16) on ARM and writes frames with
 * NEON :128 stores — an 8-aligned result is a guaranteed data abort.)
 */
static void *guarded_alloc_aligned(size_t size, size_t alignment, void *caller) {
    if (alignment < 8)
        alignment = 8;

    void *base = malloc(size + alignment + GUARD_HEAD_SIZE + sizeof(uint32_t));
    if (!base)
        return NULL;
    uintptr_t user = ((uintptr_t)base + GUARD_HEAD_SIZE + (alignment - 1)) &
                     ~(uintptr_t)(alignment - 1);
    guard_head_t *head = (guard_head_t *)(user - GUARD_HEAD_SIZE);
    head->magic        = GUARD_HEAD_MAGIC;
    head->size         = (uint32_t)size;
    head->alloc_caller = (uint32_t)(uintptr_t)caller;
    head->base_off     = (uint32_t)((uintptr_t)head - (uintptr_t)base);
    uint32_t tail = GUARD_TAIL_MAGIC;
    memcpy((char *)user + size, &tail, sizeof(tail));
    return (void *)user;
}

static void *guarded_alloc(size_t size, void *caller) {
    return guarded_alloc_aligned(size, 8, caller);
}

static int guarded_check_tail(void *ptr, const guard_head_t *head, void *caller,
                              const char *op) {
    uint32_t tail;
    memcpy(&tail, (char *)ptr + head->size, sizeof(tail));
    if (tail != GUARD_TAIL_MAGIC) {
        guard_report(op, ptr, head, caller, tail);
        return 0;
    }
    return 1;
}

void *malloc_soloader(size_t size) {
    void *ret = guarded_alloc(size, __builtin_return_address(0));
    log_large_alloc("malloc", size, ret);
    return ret;
}

/*
 * Quarantine: freed guarded chunks are poisoned (0xDD) and parked in a
 * FIFO ring instead of being returned to newlib immediately. On eviction
 * the poison and tail are verified — a mismatch is a write-after-free,
 * reported with the chunk's allocation site. While a chunk sits in the
 * ring its FREED head magic is stable, so double-frees are caught too.
 */
#define QUAR_SLOTS       64
#define QUAR_MAX_BYTES   (8u * 1024u * 1024u)
/* Large transients (e.g. the ~780KB textbox buffers allocated on every
 * advance) bypass the quarantine: poisoning, pinning and scanning them each
 * time costs CPU and fragments the heap. */
#define QUAR_CHUNK_LIMIT (256u * 1024u) /* bigger chunks skip quarantine */
#define POISON_BYTE      0xDD

static guard_head_t *quar_ring[QUAR_SLOTS];
static unsigned quar_pos = 0;
static size_t quar_bytes = 0;
static int quar_lock = 0;

/* Word-wise poison verification (full byte loops cost ~ms on multi-MB
 * chunks). Returns bad byte count (approximate over words), sets the
 * first bad offset. */
static size_t poison_check(const uint8_t *data, size_t size, size_t *first_bad) {
    size_t bad = 0;
    *first_bad = 0;
    size_t i = 0;

    for (; i + 4 <= size && ((uintptr_t)(data + i) & 3); i++) {
        if (data[i] != POISON_BYTE) {
            if (!bad)
                *first_bad = i;
            bad++;
        }
    }
    const uint32_t poison_word = 0xDDDDDDDDu;
    for (; i + 4 <= size; i += 4) {
        if (*(const uint32_t *)(data + i) != poison_word) {
            if (!bad)
                *first_bad = i;
            bad += 4;
        }
    }
    for (; i < size; i++) {
        if (data[i] != POISON_BYTE) {
            if (!bad)
                *first_bad = i;
            bad++;
        }
    }
    return bad;
}

static void quar_report_dirty(const char *stage, guard_head_t *head) {
    uint8_t *data = (uint8_t *)head + GUARD_HEAD_SIZE;
    size_t first_bad = 0;
    size_t bad = poison_check(data, head->size, &first_bad);
    uint32_t tail;
    memcpy(&tail, data + head->size, sizeof(tail));
    if ((bad || tail != GUARD_TAIL_MAGIC) && guard_report_count++ < 32) {
        uint32_t sample = 0;
        memcpy(&sample, data + (first_bad & ~3u), sizeof(sample));
        l_error("[heap-guard] write-after-free (%s) ptr=%p size=%u alloc_caller=%p bad_bytes=%u first_off=%u val=0x%08x tail=0x%08x",
                stage, data, head->size, (void *)head->alloc_caller,
                (unsigned)bad, (unsigned)first_bad, sample, tail);
    }
}

static void quar_evict(unsigned idx) {
    guard_head_t *head = quar_ring[idx];
    if (!head)
        return;
    quar_ring[idx] = NULL;
    quar_bytes -= head->size;

    quar_report_dirty("evict", head);
    void *base = GUARD_BASE(head);
    head->magic = 0;
    free(base);
}

void free_soloader(void *ptr) {
    if (!ptr)
        return;
    guard_head_t *head = (guard_head_t *)((char *)ptr - GUARD_HEAD_SIZE);
    if (head->magic == GUARD_HEAD_MAGIC) {
        guarded_check_tail(ptr, head, __builtin_return_address(0), "overflow-on-free");

        if (head->size > QUAR_CHUNK_LIMIT) {
            void *base = GUARD_BASE(head);
            head->magic = 0;
            free(base);
            return;
        }

        memset(ptr, POISON_BYTE, head->size);
        head->magic = GUARD_FREED_MAGIC;

        while (__sync_lock_test_and_set(&quar_lock, 1)) {}
        quar_evict(quar_pos);
        quar_ring[quar_pos] = head;
        quar_bytes += head->size;
        quar_pos = (quar_pos + 1) % QUAR_SLOTS;
        for (unsigned scan = quar_pos, n = 0;
             quar_bytes > QUAR_MAX_BYTES && n < QUAR_SLOTS;
             scan = (scan + 1) % QUAR_SLOTS, n++)
            quar_evict(scan);
        __sync_lock_release(&quar_lock);
    } else if (head->magic == GUARD_FREED_MAGIC) {
        guard_report("double-free", ptr, head, __builtin_return_address(0), 0);
        /* do not free again */
    } else {
        /* Allocated outside the guarded entry points (newlib strdup etc). */
        free(ptr);
    }
}

void *calloc_soloader(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    if (size != 0 && total / size != nmemb)
        return NULL;
    void *ret = guarded_alloc(total, __builtin_return_address(0));
    if (ret)
        memset(ret, 0, total);
    log_large_alloc("calloc", total, ret);
    return ret;
}

void *realloc_soloader(void *ptr, size_t size) {
    void *caller = __builtin_return_address(0);

    if (!ptr) {
        void *ret = guarded_alloc(size, caller);
        log_large_alloc("realloc", size, ret);
        return ret;
    }
    if (size == 0) {
        free_soloader(ptr);
        return NULL;
    }

    guard_head_t *head = (guard_head_t *)((char *)ptr - GUARD_HEAD_SIZE);
    if (head->magic == GUARD_FREED_MAGIC) {
        guard_report("realloc-after-free", ptr, head, caller, 0);
        return guarded_alloc(size, caller);
    }
    if (head->magic != GUARD_HEAD_MAGIC) {
        void *ret = realloc(ptr, size);
        log_large_alloc("realloc", size, ret);
        return ret;
    }

    guarded_check_tail(ptr, head, caller, "overflow-on-realloc");

    if (head->base_off) {
        /* Aligned allocation: the malloc base isn't the header, so grow
         * via alloc+copy (av_realloc does not promise alignment). */
        void *nptr = guarded_alloc(size, caller);
        if (!nptr)
            return NULL;
        memcpy(nptr, ptr, head->size < size ? head->size : size);
        free_soloader(ptr);
        log_large_alloc("realloc", size, nptr);
        return nptr;
    }

    void *nbase = realloc(head, size + GUARD_HEAD_SIZE + sizeof(uint32_t));
    if (!nbase)
        return NULL;
    guard_head_t *nhead = (guard_head_t *)nbase;
    nhead->magic        = GUARD_HEAD_MAGIC;
    nhead->size         = (uint32_t)size;
    nhead->alloc_caller = (uint32_t)(uintptr_t)caller;
    nhead->base_off     = 0;
    uint32_t tail = GUARD_TAIL_MAGIC;
    memcpy((char *)nbase + GUARD_HEAD_SIZE + size, &tail, sizeof(tail));
    void *ret = (char *)nbase + GUARD_HEAD_SIZE;
    log_large_alloc("realloc", size, ret);
    return ret;
}

void *memalign_soloader(size_t alignment, size_t size) {
    void *ret;
    /* Power-of-two alignments are honored exactly: ffmpeg's NEON :128
     * frame stores data-abort on an 8-aligned pointer. */
    if (alignment <= 4096 && (alignment & (alignment - 1)) == 0)
        ret = guarded_alloc_aligned(size, alignment, __builtin_return_address(0));
    else
        ret = memalign(alignment, size);
    log_large_alloc("memalign", size, ret);
    return ret;
}

void *operator_new_soloader(size_t size) {
    void *ret = guarded_alloc(size, __builtin_return_address(0));
    log_large_alloc("operator new", size, ret);
    if (!ret) {
        l_error("operator new(%u) failed; aborting instead of returning NULL",
                (unsigned)size);
        abort();
    }
    return ret;
}

void *operator_new_array_soloader(size_t size) {
    void *ret = guarded_alloc(size, __builtin_return_address(0));
    log_large_alloc("operator new[]", size, ret);
    if (!ret) {
        l_error("operator new[](%u) failed; aborting instead of returning NULL",
                (unsigned)size);
        abort();
    }
    return ret;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offs) {
    l_warn("mmap(%p, %i, %i, %i, %i, %li)", addr, length, prot, flags, fd, offs);

    if (length <= 0)
        return MAP_FAILED;

    void *ret = malloc(length);
    log_large_alloc("mmap", length, ret);
    if (!ret)
        return MAP_FAILED;
    memset(ret, 0, length);
    return ret;
}

int munmap(void *addr, size_t length) {
    (void)length;
    if (addr)
        free(addr);
    return 0;
}
