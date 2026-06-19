#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <linux/perf_event.h>

#ifndef CACHE_LINE_BYTES
#define CACHE_LINE_BYTES 64
#endif

typedef struct {
    uint64_t words[CACHE_LINE_BYTES / sizeof(uint64_t)];
} cache_line_t;

typedef enum {
    MODE_READ = 0,
    MODE_WRITE = 1,
    MODE_BASELINE = 2,
} access_mode_t;

typedef enum {
    KERNEL_CHAIN = 0,
    KERNEL_STREAM = 1,
} kernel_t;

typedef enum {
    LEVEL_L1 = 0,
    LEVEL_L2 = 1,
    LEVEL_L3 = 2,
    LEVEL_DRAM = 3,
    LEVEL_COUNT = 4,
} target_level_t;

typedef struct {
    access_mode_t mode;
    kernel_t kernel;
    uint64_t operations;
    uint64_t warmup_operations;
    uint64_t lines_per_level[LEVEL_COUNT];
    uint64_t steps_per_level[LEVEL_COUNT];
    uint64_t shares_per_level[LEVEL_COUNT];
    uint64_t evict_lines_l1;
    uint64_t evict_lines_l2;
    uint64_t evict_lines_l3;
    uint32_t evict_passes_l1;
    uint32_t evict_passes_l2;
    uint32_t evict_passes_l3;
    uint32_t cpu;
    uint32_t pattern_block;
    uint64_t seed;
} config_t;

typedef struct {
    cache_line_t *levels[LEVEL_COUNT];
    cache_line_t *evict_l1;
    cache_line_t *evict_l2;
    cache_line_t *evict_l3;
    uint64_t index[LEVEL_COUNT];
    uint64_t sink;
    uint64_t measured_cycles;
} state_t;

typedef struct {
    int leader_fd;
    int ll_ref_fd;
    int ll_miss_fd;
    bool enabled;
    uint64_t l1_miss;
    uint64_t ll_ref;
    uint64_t ll_miss;
} pmu_state_t;

typedef struct {
    uint64_t nr;
    uint64_t values[3];
} pmu_group_read_t;

static inline uint64_t rdtscp_serialized(void) {
    uint32_t lo = 0;
    uint32_t hi = 0;
    uint32_t aux = 0;
    __asm__ volatile("lfence\n\t"
                     "rdtscp\n\t"
                     : "=a"(lo), "=d"(hi), "=c"(aux)
                     :
                     : "memory");
    (void)aux;
    __asm__ volatile("lfence" ::: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "clock_gettime failed: %s\n", strerror(errno));
        exit(2);
    }
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static long perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static uint64_t hw_cache_config(uint64_t cache_id, uint64_t op_id, uint64_t result_id) {
    return cache_id | (op_id << 8) | (result_id << 16);
}

static void pmu_open_counter(int *fd_out, struct perf_event_attr *attr, int group_fd, const char *name) {
    int fd = (int)perf_event_open(attr, 0, -1, group_fd, 0);
    if (fd < 0) {
        fprintf(stderr, "perf_event_open failed for %s: %s\n", name, strerror(errno));
        exit(2);
    }
    *fd_out = fd;
}

static void pmu_init(pmu_state_t *pmu, access_mode_t mode) {
    memset(pmu, 0, sizeof(*pmu));
    pmu->leader_fd = -1;
    pmu->ll_ref_fd = -1;
    pmu->ll_miss_fd = -1;

    if (mode == MODE_WRITE) {
        return;
    }

    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_HW_CACHE;
    attr.size = sizeof(attr);
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.exclude_idle = 1;
    attr.read_format = PERF_FORMAT_GROUP;

    attr.config = hw_cache_config(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS);
    pmu_open_counter(&pmu->leader_fd, &attr, -1, "l1d-read-miss");

    attr.config = hw_cache_config(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_ACCESS);
    pmu_open_counter(&pmu->ll_ref_fd, &attr, pmu->leader_fd, "ll-read-access");

    attr.config = hw_cache_config(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS);
    pmu_open_counter(&pmu->ll_miss_fd, &attr, pmu->leader_fd, "ll-read-miss");

    pmu->enabled = true;
}

static void pmu_start(pmu_state_t *pmu) {
    if (!pmu->enabled) {
        return;
    }
    if (ioctl(pmu->leader_fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) != 0) {
        fprintf(stderr, "PERF_EVENT_IOC_RESET failed: %s\n", strerror(errno));
        exit(2);
    }
    if (ioctl(pmu->leader_fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) != 0) {
        fprintf(stderr, "PERF_EVENT_IOC_ENABLE failed: %s\n", strerror(errno));
        exit(2);
    }
}

static void pmu_stop(pmu_state_t *pmu) {
    if (!pmu->enabled) {
        return;
    }
    if (ioctl(pmu->leader_fd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) != 0) {
        fprintf(stderr, "PERF_EVENT_IOC_DISABLE failed: %s\n", strerror(errno));
        exit(2);
    }

    pmu_group_read_t group = {0};
    ssize_t got = read(pmu->leader_fd, &group, sizeof(group));
    if (got < 0) {
        fprintf(stderr, "reading PMU group failed: %s\n", strerror(errno));
        exit(2);
    }
    if (group.nr < 3) {
        fprintf(stderr, "unexpected PMU group size: %" PRIu64 "\n", group.nr);
        exit(2);
    }
    pmu->l1_miss = group.values[0];
    pmu->ll_ref = group.values[1];
    pmu->ll_miss = group.values[2];
}

static void pmu_close(pmu_state_t *pmu) {
    if (pmu->leader_fd >= 0) {
        close(pmu->leader_fd);
    }
    if (pmu->ll_ref_fd >= 0) {
        close(pmu->ll_ref_fd);
    }
    if (pmu->ll_miss_fd >= 0) {
        close(pmu->ll_miss_fd);
    }
}

static void pin_to_cpu(uint32_t cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        fprintf(stderr, "failed to pin to cpu %u: %s\n", cpu, strerror(errno));
        exit(2);
    }
}

static void *alloc_lines(uint64_t lines) {
    if (lines == 0) {
        return NULL;
    }

    size_t bytes = (size_t)(lines * sizeof(cache_line_t));
    void *ptr = NULL;
    if (posix_memalign(&ptr, CACHE_LINE_BYTES, bytes) != 0) {
        fprintf(stderr, "allocation failed for %zu bytes\n", bytes);
        exit(2);
    }

    memset(ptr, 0, bytes);
    if (mlock(ptr, bytes) != 0) {
        // Best effort: avoid failing on low mlock limits.
    }
    return ptr;
}

static uint64_t lcg_next(uint64_t *seed) {
    *seed = (*seed * 6364136223846793005ULL) + 1442695040888963407ULL;
    return *seed;
}

static void init_lines(cache_line_t *lines, uint64_t count, uint64_t *seed) {
    for (uint64_t i = 0; i < count; ++i) {
        for (size_t j = 0; j < (CACHE_LINE_BYTES / sizeof(uint64_t)); ++j) {
            lines[i].words[j] = lcg_next(seed) ^ (i * 0x9e3779b97f4a7c15ULL) ^ j;
        }
    }
}

static uint64_t gcd_u64(uint64_t a, uint64_t b) {
    while (b != 0) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static uint64_t choose_step(uint64_t lines, uint64_t seed) {
    if (lines <= 1) {
        return 1;
    }
    uint64_t step = (seed % (lines - 1)) + 1;
    if ((step & 1ULL) == 0) {
        step += 1;
    }
    while (gcd_u64(step, lines) != 1) {
        step += 2;
        if (step >= lines) {
            step = 1;
        }
    }
    return step;
}

static void build_pointer_cycle(cache_line_t *lines, uint64_t count, uint64_t step) {
    if (lines == NULL || count == 0) {
        return;
    }

    uint64_t *order = malloc(count * sizeof(uint64_t));
    if (order == NULL) {
        fprintf(stderr, "failed to allocate pointer cycle for %" PRIu64 " lines\n", count);
        exit(2);
    }

    for (uint64_t i = 0; i < count; ++i) {
        order[i] = i;
    }

    uint64_t seed = step ? step : 1;
    for (uint64_t i = count - 1; i > 0; --i) {
        seed = lcg_next(&seed);
        uint64_t j = seed % (i + 1);
        uint64_t tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }

    for (uint64_t i = 0; i + 1 < count; ++i) {
        lines[order[i]].words[0] = order[i + 1];
    }
    lines[order[count - 1]].words[0] = order[0];
    free(order);
}

static void churn_lines(cache_line_t *lines, uint64_t count, uint32_t passes, uint64_t *sink) {
    if (lines == NULL || count == 0 || passes == 0) {
        return;
    }

    volatile uint64_t sum = *sink;
    for (uint32_t pass = 0; pass < passes; ++pass) {
        for (uint64_t i = 0; i < count; ++i) {
            sum += lines[i].words[0];
        }
    }
    *sink = sum;
}

static inline __attribute__((always_inline)) uint64_t do_read(cache_line_t *line, uint64_t *sink) {
    uint64_t next = line->words[0];
    *sink += next;
    return next;
}

static inline __attribute__((always_inline)) uint64_t do_write(cache_line_t *line, uint64_t *sink) {
    uint64_t next = line->words[0];
    line->words[1] += 1;
    *sink += line->words[1] ^ next;
    return next;
}

static void run_chain_workload(state_t *state, const config_t *cfg, uint64_t operations) {
    cache_line_t *l1 = state->levels[LEVEL_L1];
    cache_line_t *l2 = state->levels[LEVEL_L2];
    cache_line_t *l3 = state->levels[LEVEL_L3];
    cache_line_t *dram = state->levels[LEVEL_DRAM];
    uint64_t idx1 = state->index[LEVEL_L1];
    uint64_t idx2 = state->index[LEVEL_L2];
    uint64_t idx3 = state->index[LEVEL_L3];
    uint64_t idx4 = state->index[LEVEL_DRAM];
    const uint64_t share1 = cfg->shares_per_level[LEVEL_L1];
    const uint64_t share2 = cfg->shares_per_level[LEVEL_L2];
    const uint64_t share3 = cfg->shares_per_level[LEVEL_L3];
    const uint64_t share4 = cfg->shares_per_level[LEVEL_DRAM];
    const bool do_evict = cfg->evict_passes_l1 || cfg->evict_passes_l2 || cfg->evict_passes_l3;
    uint64_t sink = state->sink;

    for (uint64_t block = 0; block < operations; block += cfg->pattern_block) {
        for (uint64_t n = 0; n < share1; ++n) {
            if (cfg->mode == MODE_BASELINE) {
                idx1 = (idx1 + 1U) % 8191U;
                sink += idx1;
            } else if (cfg->mode == MODE_READ) {
                idx1 = do_read(&l1[idx1], &sink);
            } else {
                idx1 = do_write(&l1[idx1], &sink);
            }
        }
        for (uint64_t n = 0; n < share2; ++n) {
            if (do_evict) {
                churn_lines(state->evict_l1, cfg->evict_lines_l1, cfg->evict_passes_l1, &sink);
            }
            if (cfg->mode == MODE_BASELINE) {
                idx2 = (idx2 + 3U) % 65521U;
                sink += idx2;
            } else if (cfg->mode == MODE_READ) {
                idx2 = do_read(&l2[idx2], &sink);
            } else {
                idx2 = do_write(&l2[idx2], &sink);
            }
        }
        for (uint64_t n = 0; n < share3; ++n) {
            if (do_evict) {
                churn_lines(state->evict_l1, cfg->evict_lines_l1, cfg->evict_passes_l1, &sink);
                churn_lines(state->evict_l2, cfg->evict_lines_l2, cfg->evict_passes_l2, &sink);
            }
            if (cfg->mode == MODE_BASELINE) {
                idx3 = (idx3 + 5U) % 131071U;
                sink += idx3;
            } else if (cfg->mode == MODE_READ) {
                idx3 = do_read(&l3[idx3], &sink);
            } else {
                idx3 = do_write(&l3[idx3], &sink);
            }
        }
        for (uint64_t n = 0; n < share4; ++n) {
            if (do_evict) {
                churn_lines(state->evict_l1, cfg->evict_lines_l1, cfg->evict_passes_l1, &sink);
                churn_lines(state->evict_l2, cfg->evict_lines_l2, cfg->evict_passes_l2, &sink);
                churn_lines(state->evict_l3, cfg->evict_lines_l3, cfg->evict_passes_l3, &sink);
            }
            if (cfg->mode == MODE_BASELINE) {
                idx4 = (idx4 + 7U) % 524287U;
                sink += idx4;
            } else if (cfg->mode == MODE_READ) {
                idx4 = do_read(&dram[idx4], &sink);
            } else {
                idx4 = do_write(&dram[idx4], &sink);
            }
        }
    }
    state->sink = sink;
    state->index[LEVEL_L1] = idx1;
    state->index[LEVEL_L2] = idx2;
    state->index[LEVEL_L3] = idx3;
    state->index[LEVEL_DRAM] = idx4;
}

static inline __attribute__((always_inline)) void stream_read(cache_line_t *base, uint64_t idx, uint64_t *sink) {
    *sink += base[idx].words[1];
}

static inline __attribute__((always_inline)) void stream_write(cache_line_t *base, uint64_t idx, uint64_t *sink) {
    base[idx].words[1] += 1;
    *sink += base[idx].words[1];
}

static inline __attribute__((always_inline)) uint64_t step_index(uint64_t idx, uint64_t step, uint64_t lines) {
    idx += step;
    if (idx >= lines) {
        idx -= lines;
        if (idx >= lines) {
            idx %= lines;
        }
    }
    return idx;
}

static inline __attribute__((always_inline)) void stream_read_once(
        cache_line_t *base, uint64_t *idx, uint64_t step, uint64_t lines, uint64_t *sink) {
    *sink += base[*idx].words[1];
    *idx = step_index(*idx, step, lines);
}

static inline __attribute__((always_inline)) void stream_write_once(
        cache_line_t *base, uint64_t *idx, uint64_t step, uint64_t lines, uint64_t *sink) {
    base[*idx].words[1] += 1;
    *sink += base[*idx].words[1];
    *idx = step_index(*idx, step, lines);
}

static inline __attribute__((always_inline)) void run_stream_level_read(
        cache_line_t *base,
        uint64_t *idx,
        uint64_t step,
        uint64_t lines,
        uint64_t count,
        uint64_t *sink) {
    uint64_t n = 0;
    for (; n + 16 <= count; n += 16) {
        stream_read_once(base, idx, step, lines, sink);
        stream_read_once(base, idx, step, lines, sink);
        stream_read_once(base, idx, step, lines, sink);
        stream_read_once(base, idx, step, lines, sink);
        stream_read_once(base, idx, step, lines, sink);
        stream_read_once(base, idx, step, lines, sink);
        stream_read_once(base, idx, step, lines, sink);
        stream_read_once(base, idx, step, lines, sink);
        stream_read_once(base, idx, step, lines, sink);
        stream_read_once(base, idx, step, lines, sink);
        stream_read_once(base, idx, step, lines, sink);
        stream_read_once(base, idx, step, lines, sink);
        stream_read_once(base, idx, step, lines, sink);
        stream_read_once(base, idx, step, lines, sink);
        stream_read_once(base, idx, step, lines, sink);
        stream_read_once(base, idx, step, lines, sink);
    }
    for (; n < count; ++n) {
        stream_read_once(base, idx, step, lines, sink);
    }
}

static inline __attribute__((always_inline)) void run_stream_level_write(
        cache_line_t *base,
        uint64_t *idx,
        uint64_t step,
        uint64_t lines,
        uint64_t count,
        uint64_t *sink) {
    uint64_t n = 0;
    for (; n + 16 <= count; n += 16) {
        stream_write_once(base, idx, step, lines, sink);
        stream_write_once(base, idx, step, lines, sink);
        stream_write_once(base, idx, step, lines, sink);
        stream_write_once(base, idx, step, lines, sink);
        stream_write_once(base, idx, step, lines, sink);
        stream_write_once(base, idx, step, lines, sink);
        stream_write_once(base, idx, step, lines, sink);
        stream_write_once(base, idx, step, lines, sink);
        stream_write_once(base, idx, step, lines, sink);
        stream_write_once(base, idx, step, lines, sink);
        stream_write_once(base, idx, step, lines, sink);
        stream_write_once(base, idx, step, lines, sink);
        stream_write_once(base, idx, step, lines, sink);
        stream_write_once(base, idx, step, lines, sink);
        stream_write_once(base, idx, step, lines, sink);
        stream_write_once(base, idx, step, lines, sink);
    }
    for (; n < count; ++n) {
        stream_write_once(base, idx, step, lines, sink);
    }
}

static void run_stream_l1_read_hotset(state_t *state, uint64_t operations) {
    cache_line_t *l1 = state->levels[LEVEL_L1];
    const uint64_t *p0 = &l1[0].words[1];
    const uint64_t *p1 = &l1[1].words[1];
    const uint64_t *p2 = &l1[2].words[1];
    const uint64_t *p3 = &l1[3].words[1];
    const uint64_t *p4 = &l1[4].words[1];
    const uint64_t *p5 = &l1[5].words[1];
    const uint64_t *p6 = &l1[6].words[1];
    const uint64_t *p7 = &l1[7].words[1];
    const uint64_t *q0 = &l1[0].words[2];
    const uint64_t *q1 = &l1[1].words[2];
    const uint64_t *q2 = &l1[2].words[2];
    const uint64_t *q3 = &l1[3].words[2];
    const uint64_t *q4 = &l1[4].words[2];
    const uint64_t *q5 = &l1[5].words[2];
    const uint64_t *q6 = &l1[6].words[2];
    const uint64_t *q7 = &l1[7].words[2];
    uint64_t a0 = 0;
    uint64_t a1 = 0;
    uint64_t a2 = 0;
    uint64_t a3 = 0;
    uint64_t a4 = 0;
    uint64_t a5 = 0;
    uint64_t a6 = 0;
    uint64_t a7 = 0;
    uint64_t a8 = 0;
    uint64_t a9 = 0;
    uint64_t a10 = 0;
    uint64_t a11 = 0;
    uint64_t a12 = 0;
    uint64_t a13 = 0;
    uint64_t a14 = 0;
    uint64_t a15 = 0;
    uint64_t start_cycles = rdtscp_serialized();
    uint64_t n = 0;

    for (; n + 16 <= operations; n += 16) {
        __asm__ volatile("" ::: "memory");
        a0 += *p0;
        a1 += *p1;
        a2 += *p2;
        a3 += *p3;
        a4 += *p4;
        a5 += *p5;
        a6 += *p6;
        a7 += *p7;
        a8 += *q0;
        a9 += *q1;
        a10 += *q2;
        a11 += *q3;
        a12 += *q4;
        a13 += *q5;
        a14 += *q6;
        a15 += *q7;
    }
    for (; n < operations; ++n) {
        __asm__ volatile("" ::: "memory");
        a0 += l1[n & 7U].words[1];
    }

    uint64_t end_cycles = rdtscp_serialized();
    state->sink += a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15;
    state->measured_cycles = end_cycles - start_cycles;
}

static void run_stream_workload(state_t *state, const config_t *cfg, uint64_t operations) {
    cache_line_t *l1 = state->levels[LEVEL_L1];
    cache_line_t *l2 = state->levels[LEVEL_L2];
    cache_line_t *l3 = state->levels[LEVEL_L3];
    cache_line_t *dram = state->levels[LEVEL_DRAM];
    uint64_t idx1 = state->index[LEVEL_L1];
    uint64_t idx2 = state->index[LEVEL_L2];
    uint64_t idx3 = state->index[LEVEL_L3];
    uint64_t idx4 = state->index[LEVEL_DRAM];
    const uint64_t lines1 = cfg->lines_per_level[LEVEL_L1];
    const uint64_t lines2 = cfg->lines_per_level[LEVEL_L2];
    const uint64_t lines3 = cfg->lines_per_level[LEVEL_L3];
    const uint64_t lines4 = cfg->lines_per_level[LEVEL_DRAM];
    const uint64_t step1 = cfg->steps_per_level[LEVEL_L1];
    const uint64_t step2 = cfg->steps_per_level[LEVEL_L2];
    const uint64_t step3 = cfg->steps_per_level[LEVEL_L3];
    const uint64_t step4 = cfg->steps_per_level[LEVEL_DRAM];
    const uint64_t share1 = cfg->shares_per_level[LEVEL_L1];
    const uint64_t share2 = cfg->shares_per_level[LEVEL_L2];
    const uint64_t share3 = cfg->shares_per_level[LEVEL_L3];
    const uint64_t share4 = cfg->shares_per_level[LEVEL_DRAM];
    const bool do_evict = cfg->evict_passes_l1 || cfg->evict_passes_l2 || cfg->evict_passes_l3;
    const bool l1_only = share1 == cfg->pattern_block && share2 == 0 && share3 == 0 && share4 == 0 && !do_evict;
    const bool l1_hot_read = l1_only && cfg->mode == MODE_READ && lines1 >= 8;
    uint64_t sink = state->sink;

    if (l1_hot_read) {
        run_stream_l1_read_hotset(state, operations);
        return;
    }

    uint64_t start_cycles = rdtscp_serialized();

    if (cfg->mode == MODE_BASELINE) {
        for (uint64_t block = 0; block < operations; block += cfg->pattern_block) {
            for (uint64_t n = 0; n < share1; ++n) {
                idx1 = step_index(idx1, step1, lines1);
                sink += idx1;
            }
            for (uint64_t n = 0; n < share2; ++n) {
                if (do_evict) {
                    churn_lines(state->evict_l1, cfg->evict_lines_l1, cfg->evict_passes_l1, &sink);
                }
                idx2 = step_index(idx2, step2, lines2);
                sink += idx2;
            }
            for (uint64_t n = 0; n < share3; ++n) {
                if (do_evict) {
                    churn_lines(state->evict_l1, cfg->evict_lines_l1, cfg->evict_passes_l1, &sink);
                    churn_lines(state->evict_l2, cfg->evict_lines_l2, cfg->evict_passes_l2, &sink);
                }
                idx3 = step_index(idx3, step3, lines3);
                sink += idx3;
            }
            for (uint64_t n = 0; n < share4; ++n) {
                if (do_evict) {
                    churn_lines(state->evict_l1, cfg->evict_lines_l1, cfg->evict_passes_l1, &sink);
                    churn_lines(state->evict_l2, cfg->evict_lines_l2, cfg->evict_passes_l2, &sink);
                    churn_lines(state->evict_l3, cfg->evict_lines_l3, cfg->evict_passes_l3, &sink);
                }
                idx4 = step_index(idx4, step4, lines4);
                sink += idx4;
            }
        }
    } else if (cfg->mode == MODE_READ) {
        if (l1_only) {
            run_stream_level_read(l1, &idx1, step1, lines1, operations, &sink);
        } else {
            for (uint64_t block = 0; block < operations; block += cfg->pattern_block) {
                run_stream_level_read(l1, &idx1, step1, lines1, share1, &sink);
                if (share2 > 0) {
                    if (do_evict) {
                        churn_lines(state->evict_l1, cfg->evict_lines_l1, cfg->evict_passes_l1, &sink);
                    }
                    run_stream_level_read(l2, &idx2, step2, lines2, share2, &sink);
                }
                if (share3 > 0) {
                    if (do_evict) {
                        churn_lines(state->evict_l1, cfg->evict_lines_l1, cfg->evict_passes_l1, &sink);
                        churn_lines(state->evict_l2, cfg->evict_lines_l2, cfg->evict_passes_l2, &sink);
                    }
                    run_stream_level_read(l3, &idx3, step3, lines3, share3, &sink);
                }
                if (share4 > 0) {
                    if (do_evict) {
                        churn_lines(state->evict_l1, cfg->evict_lines_l1, cfg->evict_passes_l1, &sink);
                        churn_lines(state->evict_l2, cfg->evict_lines_l2, cfg->evict_passes_l2, &sink);
                        churn_lines(state->evict_l3, cfg->evict_lines_l3, cfg->evict_passes_l3, &sink);
                    }
                    run_stream_level_read(dram, &idx4, step4, lines4, share4, &sink);
                }
            }
        }
    } else {
        if (l1_only) {
            run_stream_level_write(l1, &idx1, step1, lines1, operations, &sink);
        } else {
            for (uint64_t block = 0; block < operations; block += cfg->pattern_block) {
                run_stream_level_write(l1, &idx1, step1, lines1, share1, &sink);
                if (share2 > 0) {
                    if (do_evict) {
                        churn_lines(state->evict_l1, cfg->evict_lines_l1, cfg->evict_passes_l1, &sink);
                    }
                    run_stream_level_write(l2, &idx2, step2, lines2, share2, &sink);
                }
                if (share3 > 0) {
                    if (do_evict) {
                        churn_lines(state->evict_l1, cfg->evict_lines_l1, cfg->evict_passes_l1, &sink);
                        churn_lines(state->evict_l2, cfg->evict_lines_l2, cfg->evict_passes_l2, &sink);
                    }
                    run_stream_level_write(l3, &idx3, step3, lines3, share3, &sink);
                }
                if (share4 > 0) {
                    if (do_evict) {
                        churn_lines(state->evict_l1, cfg->evict_lines_l1, cfg->evict_passes_l1, &sink);
                        churn_lines(state->evict_l2, cfg->evict_lines_l2, cfg->evict_passes_l2, &sink);
                        churn_lines(state->evict_l3, cfg->evict_lines_l3, cfg->evict_passes_l3, &sink);
                    }
                    run_stream_level_write(dram, &idx4, step4, lines4, share4, &sink);
                }
            }
        }
    }
    uint64_t end_cycles = rdtscp_serialized();

    state->sink = sink;
    state->index[LEVEL_L1] = idx1;
    state->index[LEVEL_L2] = idx2;
    state->index[LEVEL_L3] = idx3;
    state->index[LEVEL_DRAM] = idx4;
    state->measured_cycles = end_cycles - start_cycles;
}

static void run_workload(state_t *state, const config_t *cfg, uint64_t operations) {
    if (cfg->kernel == KERNEL_STREAM) {
        run_stream_workload(state, cfg, operations);
        return;
    }
    run_chain_workload(state, cfg, operations);
}

static void usage(FILE *stream, const char *prog) {
    fprintf(stream,
            "usage: %s [options]\n"
            "  --mode read|write|baseline\n"
            "  --kernel chain|stream\n"
            "  --operations N\n"
            "  --warmup N\n"
            "  --lines-l1 N --lines-l2 N --lines-l3 N --lines-dram N\n"
            "  --share-l1 N --share-l2 N --share-l3 N --share-dram N\n"
            "  --evict-lines-l1 N --evict-lines-l2 N --evict-lines-l3 N\n"
            "  --evict-passes-l1 N --evict-passes-l2 N --evict-passes-l3 N\n"
            "  --pattern-block N --cpu N --seed N\n",
            prog);
}

static uint64_t parse_u64(const char *value, const char *name) {
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        exit(2);
    }
    return (uint64_t)parsed;
}

static void parse_args(int argc, char **argv, config_t *cfg) {
    *cfg = (config_t){
        .mode = MODE_READ,
        .kernel = KERNEL_CHAIN,
        .operations = 200000,
        .warmup_operations = 50000,
        .lines_per_level = {64, 256, 2048, 8192},
        .steps_per_level = {1, 3, 5, 7},
        .shares_per_level = {8, 4, 2, 2},
        .evict_lines_l1 = 1024,
        .evict_lines_l2 = 32768,
        .evict_lines_l3 = 524288,
        .evict_passes_l1 = 1,
        .evict_passes_l2 = 1,
        .evict_passes_l3 = 1,
        .cpu = 0,
        .pattern_block = 16,
        .seed = 1,
    };

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--mode") == 0 && i + 1 < argc) {
            const char *value = argv[++i];
            if (strcmp(value, "read") == 0) {
                cfg->mode = MODE_READ;
            } else if (strcmp(value, "write") == 0) {
                cfg->mode = MODE_WRITE;
            } else if (strcmp(value, "baseline") == 0) {
                cfg->mode = MODE_BASELINE;
            } else {
                fprintf(stderr, "invalid mode: %s\n", value);
                exit(2);
            }
        } else if (strcmp(arg, "--kernel") == 0 && i + 1 < argc) {
            const char *value = argv[++i];
            if (strcmp(value, "chain") == 0) {
                cfg->kernel = KERNEL_CHAIN;
            } else if (strcmp(value, "stream") == 0) {
                cfg->kernel = KERNEL_STREAM;
            } else {
                fprintf(stderr, "invalid kernel: %s\n", value);
                exit(2);
            }
        } else if (strcmp(arg, "--operations") == 0 && i + 1 < argc) {
            cfg->operations = parse_u64(argv[++i], "operations");
        } else if (strcmp(arg, "--warmup") == 0 && i + 1 < argc) {
            cfg->warmup_operations = parse_u64(argv[++i], "warmup");
        } else if (strcmp(arg, "--lines-l1") == 0 && i + 1 < argc) {
            cfg->lines_per_level[LEVEL_L1] = parse_u64(argv[++i], "lines-l1");
        } else if (strcmp(arg, "--lines-l2") == 0 && i + 1 < argc) {
            cfg->lines_per_level[LEVEL_L2] = parse_u64(argv[++i], "lines-l2");
        } else if (strcmp(arg, "--lines-l3") == 0 && i + 1 < argc) {
            cfg->lines_per_level[LEVEL_L3] = parse_u64(argv[++i], "lines-l3");
        } else if (strcmp(arg, "--lines-dram") == 0 && i + 1 < argc) {
            cfg->lines_per_level[LEVEL_DRAM] = parse_u64(argv[++i], "lines-dram");
        } else if (strcmp(arg, "--step-l1") == 0 && i + 1 < argc) {
            cfg->steps_per_level[LEVEL_L1] = parse_u64(argv[++i], "step-l1");
        } else if (strcmp(arg, "--step-l2") == 0 && i + 1 < argc) {
            cfg->steps_per_level[LEVEL_L2] = parse_u64(argv[++i], "step-l2");
        } else if (strcmp(arg, "--step-l3") == 0 && i + 1 < argc) {
            cfg->steps_per_level[LEVEL_L3] = parse_u64(argv[++i], "step-l3");
        } else if (strcmp(arg, "--step-dram") == 0 && i + 1 < argc) {
            cfg->steps_per_level[LEVEL_DRAM] = parse_u64(argv[++i], "step-dram");
        } else if (strcmp(arg, "--share-l1") == 0 && i + 1 < argc) {
            cfg->shares_per_level[LEVEL_L1] = parse_u64(argv[++i], "share-l1");
        } else if (strcmp(arg, "--share-l2") == 0 && i + 1 < argc) {
            cfg->shares_per_level[LEVEL_L2] = parse_u64(argv[++i], "share-l2");
        } else if (strcmp(arg, "--share-l3") == 0 && i + 1 < argc) {
            cfg->shares_per_level[LEVEL_L3] = parse_u64(argv[++i], "share-l3");
        } else if (strcmp(arg, "--share-dram") == 0 && i + 1 < argc) {
            cfg->shares_per_level[LEVEL_DRAM] = parse_u64(argv[++i], "share-dram");
        } else if (strcmp(arg, "--evict-lines-l1") == 0 && i + 1 < argc) {
            cfg->evict_lines_l1 = parse_u64(argv[++i], "evict-lines-l1");
        } else if (strcmp(arg, "--evict-lines-l2") == 0 && i + 1 < argc) {
            cfg->evict_lines_l2 = parse_u64(argv[++i], "evict-lines-l2");
        } else if (strcmp(arg, "--evict-lines-l3") == 0 && i + 1 < argc) {
            cfg->evict_lines_l3 = parse_u64(argv[++i], "evict-lines-l3");
        } else if (strcmp(arg, "--evict-passes-l1") == 0 && i + 1 < argc) {
            cfg->evict_passes_l1 = (uint32_t)parse_u64(argv[++i], "evict-passes-l1");
        } else if (strcmp(arg, "--evict-passes-l2") == 0 && i + 1 < argc) {
            cfg->evict_passes_l2 = (uint32_t)parse_u64(argv[++i], "evict-passes-l2");
        } else if (strcmp(arg, "--evict-passes-l3") == 0 && i + 1 < argc) {
            cfg->evict_passes_l3 = (uint32_t)parse_u64(argv[++i], "evict-passes-l3");
        } else if (strcmp(arg, "--pattern-block") == 0 && i + 1 < argc) {
            cfg->pattern_block = (uint32_t)parse_u64(argv[++i], "pattern-block");
        } else if (strcmp(arg, "--cpu") == 0 && i + 1 < argc) {
            cfg->cpu = (uint32_t)parse_u64(argv[++i], "cpu");
        } else if (strcmp(arg, "--seed") == 0 && i + 1 < argc) {
            cfg->seed = parse_u64(argv[++i], "seed");
        } else if (strcmp(arg, "--help") == 0) {
            usage(stdout, argv[0]);
            exit(0);
        } else {
            usage(stderr, argv[0]);
            fprintf(stderr, "unknown or incomplete arg: %s\n", arg);
            exit(2);
        }
    }

    uint64_t total_shares = 0;
    for (int i = 0; i < LEVEL_COUNT; ++i) {
        if (cfg->lines_per_level[i] == 0) {
            fprintf(stderr, "lines must be > 0 for all levels\n");
            exit(2);
        }
        if (cfg->steps_per_level[i] == 0) {
            cfg->steps_per_level[i] = choose_step(cfg->lines_per_level[i], cfg->seed + (uint64_t)i * 17ULL);
        } else if (gcd_u64(cfg->steps_per_level[i], cfg->lines_per_level[i]) != 1) {
            cfg->steps_per_level[i] = choose_step(cfg->lines_per_level[i], cfg->steps_per_level[i] + cfg->seed);
        }
        total_shares += cfg->shares_per_level[i];
    }
    if (total_shares == 0) {
        fprintf(stderr, "at least one share must be > 0\n");
        exit(2);
    }
    cfg->pattern_block = (uint32_t)total_shares;
    cfg->operations = (cfg->operations / total_shares) * total_shares;
    cfg->warmup_operations = (cfg->warmup_operations / total_shares) * total_shares;
    if (cfg->operations == 0) {
        fprintf(stderr, "operations too small for share sum %" PRIu64 "\n", total_shares);
        exit(2);
    }
}

int main(int argc, char **argv) {
    config_t cfg;
    parse_args(argc, argv, &cfg);
    pin_to_cpu(cfg.cpu);

    state_t state = {0};
    pmu_state_t pmu;
    uint64_t seed = cfg.seed;
    for (int level = 0; level < LEVEL_COUNT; ++level) {
        state.levels[level] = alloc_lines(cfg.lines_per_level[level]);
        init_lines(state.levels[level], cfg.lines_per_level[level], &seed);
        build_pointer_cycle(state.levels[level], cfg.lines_per_level[level], cfg.steps_per_level[level]);
    }
    state.evict_l1 = alloc_lines(cfg.evict_lines_l1);
    state.evict_l2 = alloc_lines(cfg.evict_lines_l2);
    state.evict_l3 = alloc_lines(cfg.evict_lines_l3);
    init_lines(state.evict_l1, cfg.evict_lines_l1, &seed);
    init_lines(state.evict_l2, cfg.evict_lines_l2, &seed);
    init_lines(state.evict_l3, cfg.evict_lines_l3, &seed);

    pmu_init(&pmu, cfg.mode);
    run_workload(&state, &cfg, cfg.warmup_operations);

    state.measured_cycles = 0;
    uint64_t start_ns = monotonic_ns();
    pmu_start(&pmu);
    uint64_t start_cycles = rdtscp_serialized();
    run_workload(&state, &cfg, cfg.operations);
    uint64_t end_cycles = rdtscp_serialized();
    pmu_stop(&pmu);
    uint64_t end_ns = monotonic_ns();

    uint64_t cycles = end_cycles - start_cycles;
    if (cfg.kernel == KERNEL_STREAM && cfg.mode != MODE_BASELINE && state.measured_cycles != 0) {
        cycles = state.measured_cycles;
    }
    uint64_t ns = end_ns - start_ns;
    double ops_per_cycle = (double)cfg.operations / (double)cycles;
    double gbps = ((double)cfg.operations * (double)CACHE_LINE_BYTES) / (double)ns;

    const char *mode_name = cfg.mode == MODE_READ ? "read" : cfg.mode == MODE_WRITE ? "write" : "baseline";
    printf("mode=%s\n", mode_name);
    printf("operations=%" PRIu64 "\n", cfg.operations);
    printf("cycles=%" PRIu64 "\n", cycles);
    printf("ns=%" PRIu64 "\n", ns);
    printf("ops_per_cycle=%.9f\n", ops_per_cycle);
    printf("gbps=%.9f\n", gbps);
    printf("pmu_l1_miss=%" PRIu64 "\n", pmu.l1_miss);
    printf("pmu_ll_ref=%" PRIu64 "\n", pmu.ll_ref);
    printf("pmu_ll_miss=%" PRIu64 "\n", pmu.ll_miss);
    printf("sink=%" PRIu64 "\n", state.sink);
    pmu_close(&pmu);
    return 0;
}
