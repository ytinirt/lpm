/*
 * lpm_internal.h
 *
 * Longest prefix matching internal header file.
 *
 * History
 */

#ifndef _LPM_INTERNAL_H_
#define _LPM_INTERNAL_H_

#include "lpm.h"

#define LPM_STRIDE      8           /* 8-8-8-8-8...cannot modify for now */
#define LPM_LEVEL_MAX   16
#define LPM_MASKLEN_MAX (LPM_LEVEL_MAX * LPM_STRIDE)

typedef struct btrie_node_s {
    void *data;
    /* child[0] is left child, and child[1] is right child */
    struct btrie_node_s *child[2];
} btrie_node_t;

/*
 * On 32-bit CPU, mtrie node is 8 bytes, mtrie block is 8 x 256 = 2KB
 */
typedef struct mtrie_node_s {
    void *data;
    struct mtrie_node_s *base;  /* sub-level mtrie table (block) base */
} mtrie_node_t;

/*
 * LPM table statistic structure
 */
struct lpm_lkup_table_stat {
    volatile int btrie_node_alloc_stat;                 /* B-trie nodes total allocating quantity */
    volatile u32 btrie_node_alloc_fail_stat;            /* B-trie nodes alloc failure quantity */

    volatile int mtrie_block_alloc_stat;                /* M-trie block total allocating quantity */
    volatile u32 mtrie_block_alloc_fail_stat;           /* M-trie block alloc failure quantity */

    volatile int data_total;                            /* quantity of valid data stored in LPM */
    volatile u32 data_per_masklen[LPM_MASKLEN_MAX + 1]; /* data's quantity of each masklen */
};

#define LPM_TABLE_NAME_LEN  32              /* table name string maximum length, include '\0' */
#define LPM_TABLE_DEFAULT_NAME "Unknown"    /* table name by default */

struct lpm_lkup_table_s {
    char name[LPM_TABLE_NAME_LEN];          /* LPM table name */

    btrie_node_t *btrie_root;               /* b-trie root node */
    mtrie_node_t *hi256_table_base;         /* m-trie base block */
    
    void *default_data;                     /* LPM default data */
    u8 default_addr[LPM_LEVEL_MAX];         /* LPM default prefix (network) */
    u32 default_masklen;                    /* LPM default prefix's mask length */

    unsigned long debug_flag;               /* LPM debug flag */
    struct lpm_lkup_table_stat stat;        /* LPM table statistic */
};

/* memory allocation failure simulating switch, 0 for close, and 1 for open */
#define LPM_DEBUG_ALLOC_FAIL    0                       /* close by default */
/* check for recursion depth */
#define LPM_DEBUG_RECURSION     1                       /* open by default */
#define LPM_RECUR_DEPTH_WARN    (LPM_MASKLEN_MAX + 1)   /* maximum recursion depth */

/*
 * debug options
 */
#define LPM_DEBUG_MASK  (~(0UL))
#define LPM_DEBUG_NORM  ((0x1UL) << 0)
#define LPM_DEBUG_MEM   ((0x1UL) << 1)
#define LPM_DEBUG_ALG   ((0x1UL) << 2)
#define LPM_LOGGING     ((0x1UL) << 3)

#define unlikely(x) __builtin_expect(!!(x), 0)
#define likely(x)   __builtin_expect(!!(x), 1)

/* General errors or any other type of debugging message */
#define LPM_DEBUGGING_NORM(table) (unlikely(((table)->debug_flag) & LPM_DEBUG_NORM))
#define lpm_debug_norm(table, fmt, arg...) \
    do { \
        if (LPM_DEBUGGING_NORM(table)) { \
            fprintf(stderr, "LPM-debug-normal @ %s <%d> [%s]: ", __func__, __LINE__, (table)->name); \
            fprintf(stderr, fmt, ##arg); \
        } \
    } while (0)

/* Memory allocating and releasing debugging */
#define LPM_DEBUGGING_MEM(table) (unlikely(((table)->debug_flag) & LPM_DEBUG_MEM))
#define lpm_debug_mem(table, fmt, arg...) \
    do { \
        if (LPM_DEBUGGING_MEM(table)) { \
            fprintf(stderr, "LPM-debug-memory @ %s <%d> [%s]: ", __func__, __LINE__, (table)->name); \
            fprintf(stderr, fmt, ##arg); \
        } \
    } while (0)

/* LPM algorithm internal error, eg. data inconsistent, contradict results... */
#define LPM_DEBUGGING_ALG(table) (unlikely(((table)->debug_flag) & LPM_DEBUG_ALG))
#define lpm_debug_alg(table, fmt, arg...) \
    do { \
        if (LPM_DEBUGGING_ALG(table)) { \
            fprintf(stderr, "LPM-debug-algorithm @ %s <%d> [%s]: ", __func__, __LINE__, (table)->name); \
            fprintf(stderr, fmt, ##arg); \
        } \
    } while (0)

/* Used only for LPM external interface logging message */
#define LPM_LOGGING_ON(table) (unlikely(((table)->debug_flag) & LPM_LOGGING))
#define lpm_log_print(table, fmt, arg...) \
    do { \
        if (LPM_LOGGING_ON(table)) { \
            fprintf(stdout, "LPM-log [%s]: ", (table)->name); \
            fprintf(stdout, fmt, ##arg); \
        } \
    } while (0)

/* Used while table is invalid or for console print */
#define lpm_con_print(fmt, arg...) \
    do { \
        fprintf(stderr, "LPM-con: " fmt, ##arg); \
    } while (0)

#if LPM_DEBUG_ALLOC_FAIL
#include <time.h>

static inline u8 lpm_mem_success(int range)
{
    u32 r;
    static u32 seed = 0;

    if (range <= 1) {
        return 1;
    }

    seed += rand();
    seed = seed % 97;
    srand(((unsigned int)(time(NULL))) + seed);
    r = rand();
    if ((r % range) == 0) {
        return 0;
    }

    return 1;
}
#endif

/*
 * only for 8-8-8-8-...
 */
static inline u8 BOUNDARY_BIT_POSITION(u32 pos)
{
    /* If pos is 7, 15, 23, 31, 39 ..., 127 */
    if (((pos) & 7) == 7) {
        return 1;
    }

    return 0;
}

/*
 * ATTENTION addr should be network byte order (big endianness)
 */
static inline void CLEAR_BIT_AT_POSITION(u8 *addr, u32 pos)
{
    u8 mask;

    assert(pos < LPM_MASKLEN_MAX);
    assert(addr != NULL);
    
    mask = 0xFF ^ (0x1 << (7 - (pos & 0x7)));
    *(addr + (pos >> 3)) &= mask;
}

/*
 * ATTENTION addr should be network byte order (big endianness)
 */
static inline void SET_BIT_AT_POSITION(u8 *addr, u32 pos)
{
    assert(pos < LPM_MASKLEN_MAX);
    assert(addr != NULL);
    
    *(addr + (pos >> 3)) |= (0x1 << (7 - (pos & 0x7)));
}

/*
 * ATTENTION addr should be network byte order (big endianness)
 * eg. 128.0.0.2
 * pos=0, bit=1,
 * pos=30, bit=1,
 * pos=31, bit=0.
 */
static inline u8 bit_at_position(u8 *addr, u32 pos)
{
    assert(pos < LPM_MASKLEN_MAX);
    assert(addr != NULL);

    return (!!((*(addr + (pos >> 3))) & (0x1 << (7 - (pos & 0x7)))));
}

#endif /* !_LPM_INTERNAL_H_ */

