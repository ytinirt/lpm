/*
 * lpm.c
 *
 * Longest prefix matching implementation file.
 *
 * ATTENTION:
 *      1. b-trie equals to 1-trie.
 *      2. m-trie block has 256 m-trie entry (aka. node) while stride is 8.
 *      3. While compare two valid prefix, the "more specific" prefix refers to the one who 
 *         has longer mask length, and the "less specific" prefix has shorter mask length.
 *
 * History
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>

#include "lpm.h"
#include "lpm_internal.h"

/*******************************
 * B-trie rel. codes
 */
static btrie_node_t *btrie_mem_alloc(struct lpm_lkup_table_stat *stat)
{
    btrie_node_t *ret;

    assert(stat != NULL);
    
    ret = malloc(sizeof(btrie_node_t));
    
#if LPM_DEBUG_ALLOC_FAIL
    if (!lpm_mem_success(64)) {
        free(ret);
        ret = NULL;
    }
#endif

    if (ret != NULL) {
        /* zero out needed */
        memset(ret, 0, sizeof(btrie_node_t));
        stat->btrie_node_alloc_stat++;
    } else {
        stat->btrie_node_alloc_fail_stat++;
    }
    
    return ret;
}

static void btrie_mem_free(struct lpm_lkup_table_stat *stat, btrie_node_t *p)
{
    assert(stat != NULL);

    if (p != NULL) {
        assert(stat->btrie_node_alloc_stat > 0);
        free(p);
        stat->btrie_node_alloc_stat--;
    }
}

static btrie_node_t *btrie_alloc_node(lpm_lkup_table_t *table)
{
    btrie_node_t *ret = NULL;

    assert(table != NULL);

    ret = btrie_mem_alloc(&table->stat);

    return ret;
}

static void btrie_free_node(lpm_lkup_table_t *table, btrie_node_t *p)
{
    assert(table != NULL);

    btrie_mem_free(&table->stat, p);
}

/* find addr/masklen corresponding 1-trie node, will not add new node when don't find */
static btrie_node_t *btrie_find_node(btrie_node_t *root, u8 *addr, u32 masklen)
{
    int pos;
    u8 bit;
    btrie_node_t *node;

    if (masklen > 0) {
        assert(addr != NULL);
    }

    for (node = root, pos = 0; pos < masklen && node != NULL; pos++) {
        bit = bit_at_position(addr, pos);
        node = node->child[bit];
    }

    return node;
}

/* accurately find */
static void *btrie_find_data(btrie_node_t *root, u8 *addr, u32 masklen)
{
    btrie_node_t *node;

    node = btrie_find_node(root, addr, masklen);
    
    if (node != NULL) {
        return node->data;
    }

    return NULL;
}

/*
 * Return LPM_ERR_EXISTS while find corresponding node success, ATTENTION it's not an failure.
 * Add new node while not find corresponding node, and return LPM_SUCCESS.
 * Any failure will lead to return LPM_ERR_XXX.
 * It will not operate data.
 */
static lpm_result_t btrie_add_node(lpm_lkup_table_t *table,
                                   u8 *addr,
                                   u32 masklen,
                                   btrie_node_t **newnode,
                                   btrie_node_t **append_point,
                                   u8 *append_bit)
{
    lpm_result_t ret = LPM_ERR_EXISTS;              /* node is existing by default */
    int pos;
    u8 bit;
    btrie_node_t *place;

    assert(newnode != NULL);
    assert(append_point != NULL);
    assert(table != NULL);
    assert(table->hi256_table_base != NULL);
    
    if (masklen > 0) {
        assert(addr != NULL);
        *append_point = table->btrie_root;
        *append_bit = bit_at_position(addr, 0);
    }

    place = table->btrie_root;                      /* from the root of 1-trie */
    pos = 0;

    while (pos < masklen) {
        bit = bit_at_position(addr, pos);
        if (place->child[bit] == NULL) {
            place->child[bit] = btrie_alloc_node(table);
            if (place->child[bit] == NULL) {        /* Resources failed */
                lpm_debug_mem(table, "btrie node [%d Bytes] alloc failed\n", sizeof(btrie_node_t));
                return LPM_ERR_RESOURCES;
            }
            ret = LPM_SUCCESS;                      /* node new allocated */
        } else {
            *append_point = place->child[bit];
            *append_bit = bit_at_position(addr, (pos + 1));
        }
        place = place->child[bit];
        pos++;
    }

    *newnode = place;

    return ret;
}

/*
 * Releasing the allocated 1-trie nodes when failure takes place 
 * within add entry in 1-trie or M-trie.
 */
static void btrie_del_appended(lpm_lkup_table_t *table, btrie_node_t *node)
{
    btrie_node_t *curr, *next;

    for (curr = node, next = NULL; curr != NULL; curr = next) {
        if (curr->child[0] != NULL && curr->child[1] != NULL) {
            /* XXX BUG */
            lpm_debug_alg(table, "*BUG* Appended btrie nodes can not have two children\n");
            lpm_con_print("*BUG* Appended btrie nodes can not have two children\n");
            assert(0);  /* XXX suicide */
        } else if (curr->child[0] != NULL) {
            next = curr->child[0];
        } else {
            next = curr->child[1];
        }

        lpm_debug_norm(table, "\t\tfree one temporary btrie node...\n");
        btrie_free_node(table, curr);
    }
}

static void __btrie_destroy_subtree(lpm_lkup_table_t *table, btrie_node_t *node, u32 *recur_times)
{
#if LPM_DEBUG_RECURSION
    if (*recur_times > LPM_RECUR_DEPTH_WARN) {
        lpm_con_print("%s *BUG WARNING* recursion times = %u, too deep\n", __func__, *recur_times);
    }
    *recur_times = *recur_times + 1;
#endif

    if (node == NULL) {
        return;
    }

    __btrie_destroy_subtree(table, node->child[0], recur_times);
    
#if LPM_DEBUG_RECURSION
    *recur_times = *recur_times - 1;
#endif

    __btrie_destroy_subtree(table, node->child[1], recur_times);

#if LPM_DEBUG_RECURSION
    *recur_times = *recur_times - 1;
#endif

    btrie_free_node(table, node);
}

static void btrie_destroy_subtree(lpm_lkup_table_t *table, btrie_node_t *node)
{
    u32 recur_times = 0;
    
    __btrie_destroy_subtree(table, node, &recur_times);
}

static lpm_result_t __btrie_dfs_walk(btrie_node_t *node,
                                     u8 *addr,
                                     u32 bitpos,
                                     lpm_data_walker_func_t walker,
                                     u32 *recur_times)
{
    lpm_result_t ret;

#if LPM_DEBUG_RECURSION
    if (*recur_times > LPM_RECUR_DEPTH_WARN) {
        lpm_con_print("%s *BUG WARNING* recursion times = %u, too deep\n", __func__, *recur_times);
    }
    *recur_times = *recur_times + 1;
#endif
    
    if (node->data != NULL) {
        if ((*walker)(addr, bitpos, node->data) != 0) {
            /* Error is from walker, not from LPM */
            return LPM_ERR_EXOTIC;
        }
    }

    /* Traverse left sub-tree */
    if (node->child[0] != NULL) {
        CLEAR_BIT_AT_POSITION(addr, bitpos);

        ret = __btrie_dfs_walk(node->child[0], addr, bitpos + 1, walker, recur_times);

#if LPM_DEBUG_RECURSION
        *recur_times = *recur_times - 1;
#endif

        if (ret != LPM_SUCCESS) {
            return ret;
        }
    }

    /* Traverse right sub-tree */
    if (node->child[1] != NULL) {
        SET_BIT_AT_POSITION(addr, bitpos);

        ret = __btrie_dfs_walk(node->child[1], addr, bitpos + 1, walker, recur_times);

        /*
         * XXX: addr is changed due to SET_BIT_AT_POSITION, recover its value and prepare for
         *      next recursion. eg. 128.0.0.0/2 and 80.0.0.0/4
         */
        CLEAR_BIT_AT_POSITION(addr, bitpos);

#if LPM_DEBUG_RECURSION
        *recur_times = *recur_times - 1;
#endif

        if (ret != LPM_SUCCESS) {
            return ret;
        }
    }

    return LPM_SUCCESS;
}

static lpm_result_t btrie_dfs_walk(btrie_node_t *root, lpm_data_walker_func_t walker)
{
    int ret = LPM_SUCCESS;
    u8 addr[LPM_LEVEL_MAX];
    u32 prefix_len;
    u32 recur_times = 0;    /* Recursion depth check */

    memset(&addr, 0, sizeof(addr));
    prefix_len = 0;
    
    ret = __btrie_dfs_walk(root, addr, prefix_len, walker, &recur_times);

    return ret;
}

static lpm_result_t btrie_init(lpm_lkup_table_t *table)
{
    if (table == NULL) {
        return LPM_ERR_INVALID;
    }
    
    if (table->btrie_root != NULL) {
        lpm_debug_norm(table, "B-trie already exists\n");
        return LPM_ERR_EXISTS;
    }

    table->btrie_root = btrie_alloc_node(table);
    if (table->btrie_root == NULL) {
        lpm_debug_mem(table, "B-trie root node [%d Bytes] alloc failed\n", sizeof(btrie_node_t));
        return LPM_ERR_RESOURCES;
    }
    
    lpm_debug_norm(table, "B-trie initialized\n");

    return LPM_SUCCESS;
}

static void btrie_destroy(lpm_lkup_table_t *table)
{
    if (table == NULL) {
        return;
    }

    btrie_destroy_subtree(table, table->btrie_root);
    
    table->btrie_root = NULL;

    lpm_debug_norm(table, "B-trie is destroyed\n");
}

/*******************************
 * M-trie rel. codes
 */
#define MTRIE_BLOCK_ENTRY   (0x1 << LPM_STRIDE)         /* for 8-stride */
#define MTRIE_BLOCK_ALLOC_SIZE ((sizeof(mtrie_node_t)) * MTRIE_BLOCK_ENTRY)

static mtrie_node_t *mtrie_mem_alloc(struct lpm_lkup_table_stat *stat)
{
    mtrie_node_t *ret;

    assert(stat != NULL);

    ret = malloc(MTRIE_BLOCK_ALLOC_SIZE);
    
#if LPM_DEBUG_ALLOC_FAIL
    if (!lpm_mem_success(8)) {
        free(ret);
        ret = NULL;
    }
#endif

    if (ret != NULL) {
        memset(ret, 0, MTRIE_BLOCK_ALLOC_SIZE);
        stat->mtrie_block_alloc_stat++;
    } else {
        stat->mtrie_block_alloc_fail_stat++;
    }
    
    return ret;
}

static void mtrie_mem_free(struct lpm_lkup_table_stat *stat, mtrie_node_t *p)
{
    assert(stat != NULL);

    if (p != NULL) {
        assert(stat->mtrie_block_alloc_stat > 0);
        free(p);
        stat->mtrie_block_alloc_stat--;
    }
}

static mtrie_node_t *mtrie_alloc_block(lpm_lkup_table_t *table)
{
    mtrie_node_t *base = NULL;

    assert(table != NULL);

    base = mtrie_mem_alloc(&table->stat);
    if (base == NULL) {
        lpm_debug_mem(table, "Mtrie block [%d Bytes] allocate failed\n", MTRIE_BLOCK_ALLOC_SIZE);
    }
    
    return base;
}

static void __mtrie_free_block(lpm_lkup_table_t *table, mtrie_node_t *base, u32 *recur_times)
{
    int i;
    mtrie_node_t *entry;

    assert(table != NULL);
    
#if LPM_DEBUG_RECURSION
    if (*recur_times > LPM_RECUR_DEPTH_WARN) {
        lpm_con_print("%s *BUG WARNING* recursion times = %u, too deep\n", __func__, *recur_times);
    }
    *recur_times = *recur_times + 1;
#endif

    if (base == NULL) {
        return;
    }

    for (i = 0; i < MTRIE_BLOCK_ENTRY; i++) {
        entry = (mtrie_node_t *)(base + i);
        if (entry->base != NULL) {
            __mtrie_free_block(table, entry->base, recur_times);

#if LPM_DEBUG_RECURSION
            *recur_times = *recur_times - 1;
#endif 
        }
    }

    mtrie_mem_free(&table->stat, base);
}

static void mtrie_free_block(lpm_lkup_table_t *table, mtrie_node_t *base)
{
    u32 recur_times = 0;

    __mtrie_free_block(table, base, &recur_times);
}

static lpm_result_t mtrie_init(lpm_lkup_table_t *table)
{
    if (table == NULL) {
        return LPM_ERR_INVALID;
    }

    if (table->hi256_table_base != NULL) {
        lpm_debug_norm(table, "M-trie table already exists\n");
        return LPM_ERR_EXISTS;
    }

    table->hi256_table_base = mtrie_alloc_block(table);
    if (table->hi256_table_base == NULL) {
        lpm_debug_mem(table, "M-trie base table [%d Bytes] of LPM alloc failed\n",
                                    MTRIE_BLOCK_ALLOC_SIZE);
        return LPM_ERR_RESOURCES;
    }
    
    lpm_debug_norm(table, "M-trie is initialized\n");

    return LPM_SUCCESS;
}

static void mtrie_destroy(lpm_lkup_table_t *table)
{
    if (table == NULL) {
        return;
    }

    mtrie_free_block(table, table->hi256_table_base);

    table->hi256_table_base = NULL;
    
    lpm_debug_norm(table, "M-trie is destroyed\n");
}

/*******************************
 * LPM rel. codes
 */
static lpm_lkup_table_t *lpm_mem_alloc()
{
    lpm_lkup_table_t *ret;
    
    ret = malloc(sizeof(lpm_lkup_table_t));
    
#if LPM_DEBUG_ALLOC_FAIL
    if (!lpm_mem_success(101)) {
        free(ret);
        ret = NULL;
    }
#endif

    if (ret != NULL) {
        memset(ret, 0, sizeof(lpm_lkup_table_t));
    }
    
    return ret;
}

static void lpm_mem_free(lpm_lkup_table_t *p)
{
    if (p != NULL) {
        free(p);
    }
}

lpm_result_t lpm_debug_support(lpm_lkup_table_t *table, lpm_debug_t debug, int on)
{
    if (table == NULL) {
        lpm_con_print("%s table not found..\n", __func__);
        return LPM_ERR_INVALID;
    }

    if (on == 1) {          /* open */
        switch (debug) {
        case DEBUG_NORMAL:
            table->debug_flag |= LPM_DEBUG_NORM;
            break;
        case DEBUG_MEMORY:
            table->debug_flag |= LPM_DEBUG_MEM;
            break;
        case DEBUG_ALGORITHM:
            table->debug_flag |= LPM_DEBUG_ALG;
            break;
        case DEBUG_ALL:
            table->debug_flag |= LPM_DEBUG_MASK;
            break;
        case LOGGING:
            table->debug_flag |= LPM_LOGGING;
            break;
        default:
            lpm_debug_norm(table, "Unknown debug <%d>\n", debug);
            return LPM_ERR_INVALID;
        }
    } else if (on == 0){    /* close */
        switch (debug) {
        case DEBUG_NORMAL:
            table->debug_flag &= (~LPM_DEBUG_NORM);
            break;
        case DEBUG_MEMORY:
            table->debug_flag &= (~LPM_DEBUG_MEM);
            break;
        case DEBUG_ALGORITHM:
            table->debug_flag &= (~LPM_DEBUG_ALG);
            break;
        case DEBUG_ALL:
            table->debug_flag &= (~LPM_DEBUG_MASK);
            break;
        case LOGGING:
            table->debug_flag &= (~LPM_LOGGING);
            break;
        default:
            lpm_debug_norm(table, "Unknown debug <%d>\n", debug);
            return LPM_ERR_INVALID;
        }
    } else {
        lpm_debug_norm(table, "Unknown on <%d>\n", on);
        return LPM_ERR_INVALID;
    }

    lpm_log_print(table, "debug<%d>, on<%d>\n", debug, on);

    return LPM_SUCCESS;
}

void lpm_table_statistic(lpm_lkup_table_t *table)
{
    struct lpm_lkup_table_stat *stat;
    float btrie_mem, mtrie_mem;
    float cnt;
    u32 i, j;
    char buf[128], *p;

    if (table == NULL) {
        lpm_con_print("%s table not found...\n", __func__);
        return;
    }
    
    lpm_log_print(table, "print LPM statistic\n");

    stat = &table->stat;
    btrie_mem = ((float)((stat->btrie_node_alloc_stat) * sizeof(btrie_node_t))) / 1000000.0;
    mtrie_mem = ((float)((stat->mtrie_block_alloc_stat) * MTRIE_BLOCK_ALLOC_SIZE)) / 1000000.0;

    lpm_con_print("LPM Table [%s] statistic:\n", table->name);
    lpm_con_print("\tB-trie allocated nodes: %d nodes, [%.3f MB]\n",
                        stat->btrie_node_alloc_stat, btrie_mem);
    lpm_con_print("\tB-trie allocated failure: %u times\n", stat->btrie_node_alloc_fail_stat);
    lpm_con_print("\tM-trie allocated blocks: %d blocks, [%.3f MB]\n",
                        stat->mtrie_block_alloc_stat, mtrie_mem);
    lpm_con_print("\tM-trie allocated failure: %u times\n", stat->mtrie_block_alloc_fail_stat);
    lpm_con_print("\tLPM Table valid data total count: [%d]\n", stat->data_total);

    if (LPM_DEBUGGING_NORM(table)) {
        for (i = 0; i <= LPM_MASKLEN_MAX; i++) {
            if (stat->data_per_masklen[i] == 0) {
                continue;
            }
#define TOTAL_ASTERISK_PRINT 100
            assert(stat->data_total > 0);
            cnt = ((float)(stat->data_per_masklen[i] * TOTAL_ASTERISK_PRINT)) / ((float)(stat->data_total));
            j = (u32)cnt;
            if (j == 0) {
                j = 1;
            }
            if (j > TOTAL_ASTERISK_PRINT) {
                lpm_debug_alg(table, "data of masklen /%u is larger than total count\n", i);
                j = TOTAL_ASTERISK_PRINT;
            }
            for (p = buf; j > 0; j--, p++) {
                *p = '*';
            }
            *p = '\0';
            lpm_debug_norm(table, "\t  /%-3u [%4u]: %s\n", i, stat->data_per_masklen[i], buf);
        }
    }

    lpm_con_print("\tTotal memory size: %.3f MB\n", (btrie_mem + mtrie_mem));
}

/*
 * lpm_create_table never fail to allocate 1-trie root node and m-trie root trie block.
 * After LPM table is created, 1-trie root node and m-trie root trie block will never be NULL,
 * otherwise it is LPM algorithm internal error.
 */
lpm_lkup_table_t *lpm_create_table(char *name)
{
    lpm_lkup_table_t *table;

    lpm_con_print("%s with name <%s>\n", __func__, name);

    table = lpm_mem_alloc();
    if (table == NULL) {
        lpm_con_print("%s allocate LPM table failed\n", __func__);
        return NULL;
    }
    if (name == NULL) {
        sprintf(table->name, LPM_TABLE_DEFAULT_NAME);
    } else {
        strncpy(table->name, name, (LPM_TABLE_NAME_LEN - 1));
    }
    
    if (btrie_init(table) != LPM_SUCCESS) {
        lpm_debug_norm(table, "B-trie initial failed\n");
        goto error_btrie;
    }

    if (mtrie_init(table) != LPM_SUCCESS) {
        lpm_debug_norm(table, "M-trie initial failed\n");
        goto error_mtrie;
    }

    lpm_log_print(table, "name <%s>, success\n", table->name);

    return table;

error_mtrie:
    btrie_destroy(table);

error_btrie:
    lpm_mem_free(table);

    return NULL;
}

lpm_result_t lpm_destroy_table(lpm_lkup_table_t *table)
{
    if (table == NULL) {
        lpm_con_print("%s table not found...\n", __func__);
        return LPM_ERR_INVALID;
    }

    lpm_log_print(table, "I am done...\n");

    mtrie_destroy(table);
    btrie_destroy(table);
    lpm_mem_free(table);
    
    return LPM_SUCCESS;
}

static lpm_result_t lpm_check_arg(lpm_lkup_table_t *table, u8 *addr, u32 masklen)
{
    if (table == NULL) {                /* table should be valid */
        lpm_con_print("%s table not found...\n", __func__);
        return LPM_ERR_INVALID;
    }

    if (masklen > LPM_MASKLEN_MAX) {
        lpm_con_print("%s masklen [%u] is too large\n", __func__, masklen);
        return LPM_ERR_INVALID;
    }

    if (masklen > 0 && addr == NULL) {  /* addr should be valid while masklen is not 0 */
        lpm_con_print("%s address CAN NOT be NULL while masklen > 0\n", __func__);
        return LPM_ERR_INVALID;
    }

    return LPM_SUCCESS;
}

void *lpm_find_entry(lpm_lkup_table_t *table, u8 *addr, u32 masklen)
{
    void *data = NULL;

    if (lpm_check_arg(table, addr, masklen) != LPM_SUCCESS) {
        lpm_con_print("%s invalid argument\n", __func__);
        return NULL;
    }

    if (table->btrie_root == NULL) {
        lpm_debug_alg(table, "B-trie of LPM not exists\n");
        return NULL;
    }

    data = btrie_find_data(table->btrie_root, addr, masklen);

    lpm_log_print(table, "accurately find result %p\n", data);

    return data;
}

/*
 * Longest prefix matching search, performance is the KEY.
 * Return default data when do not find valid data in m-trie, and set the value to 1 pointed by
 * using_default.
 */
void *lpm_search_table(lpm_lkup_table_t *table, u8 *addr, u8 *using_default)
{
    u8 *idx;
    mtrie_node_t *entry, *base;
    void *data = NULL;

    if (table == NULL || addr == NULL || using_default == NULL) {
        lpm_con_print("%s invalid argument\n", __func__);
        return NULL;
    }

    base = table->hi256_table_base;
    idx = addr;
    *using_default = 0;
    while (base != NULL) {
        entry = (mtrie_node_t *)(base + (*idx));
        if (entry->data != NULL) {
            data = entry->data;
        }
        idx++;
        base = entry->base;
    }

    if (data == NULL) {
        data = table->default_data;
        *using_default = 1;
    }
    
    return data;
}

/* Write data in m-trie block */
static void lpm_pattern_generate(mtrie_node_t *base, u8 idx, u32 bitpos, void *data)
{
    u8 mask;
    u32 tmp_idx, end_idx;
    u32 masklen = (bitpos + 1) % 8;
    mtrie_node_t *tmp_trie;

    if (BOUNDARY_BIT_POSITION(bitpos)) {
        mask = 0xFF;
    } else {
        mask = ~((0x1 << (8 - masklen)) - 1);
    }

    tmp_idx = idx & mask;
    idx |= (~mask);
    end_idx = idx;

    for ( ; tmp_idx <= end_idx; tmp_idx++) {
        tmp_trie = (mtrie_node_t *)(base + tmp_idx);
        tmp_trie->data = data;
    }
}

/* Operation is only confined to a certain m-trie block */
static lpm_result_t lpm_gen_combinations(lpm_lkup_table_t *table,
                                         u8 *addr,
                                         u32 temp_bitpos,
                                         void *data,
                                         char nextbit)
{
    lpm_result_t ret = LPM_SUCCESS;
    mtrie_node_t *mtrie_table_base;
    mtrie_node_t *frontier_trie, *pre_entry;
    mtrie_node_t *trie_chain[LPM_LEVEL_MAX] = {NULL}; /* trie base */
    u8 trie_idx[LPM_LEVEL_MAX] = {0};
    
#define TRIE_CHAIN_ALLOC 0x10
    u8 trie_chain_alloc[LPM_LEVEL_MAX] = {0};   /* 0x10 stands for newly allocating */

    int trie_count;                             /* the count of tries which will be operated */
    int level, i;
    u8 idx = 0;

    assert(temp_bitpos < LPM_MASKLEN_MAX);
    assert(addr != NULL);
    assert(table != NULL);
    mtrie_table_base = table->hi256_table_base;
    assert(mtrie_table_base != NULL);
    assert((nextbit == -1) || (nextbit == 0) || (nextbit == 1));

    if (temp_bitpos < 8) {
        idx = addr[0];
        switch (nextbit) {
        case 0: /* next bit should set to 0 */
            assert(temp_bitpos != 7);
            idx &= (0xFF ^ (0x1 << (7 - (temp_bitpos + 1))));
            lpm_pattern_generate(mtrie_table_base, idx, (temp_bitpos + 1), data);
            break;
        case 1: /* next bit should set to 1 */
            assert(temp_bitpos != 7);
            idx |= (0x1 << (7 - (temp_bitpos + 1)));
            lpm_pattern_generate(mtrie_table_base, idx, (temp_bitpos + 1), data);
            break;
        case -1:/* next bit need no touch */
            lpm_pattern_generate(mtrie_table_base, idx, temp_bitpos, data);
            break;
        }

        return ret;
    }

    /* temp_bitpos >= 8 */
    trie_count = (temp_bitpos >> 3) + 1;

    /* Build trie chain, allocate new trie block when necessary */
    for (level = 0, frontier_trie = mtrie_table_base; level < trie_count; level++) {
        if (frontier_trie == NULL) {
            frontier_trie = mtrie_alloc_block(table);
            if (frontier_trie == NULL) {
                lpm_debug_mem(table, "mtrie block [%d Btyes] allocate failed, releasing allocated memory\n",
                                            MTRIE_BLOCK_ALLOC_SIZE);
                for (i = 0; i < level; i++) {
                    if (trie_chain_alloc[i] == TRIE_CHAIN_ALLOC) {
                        /* release mtrie blocks which were former allocated */
                        mtrie_free_block(table, trie_chain[i]);
                        lpm_debug_mem(table, "\t\tfree one mtrie block...\n");
                    }
                }
                return LPM_ERR_RESOURCES;
            }
            trie_chain_alloc[level] = TRIE_CHAIN_ALLOC;
        }
        trie_chain[level] = frontier_trie;
        trie_idx[level] = addr[level];

        frontier_trie = ((mtrie_node_t *)(trie_chain[level] + trie_idx[level]))->base;
    }

    frontier_trie = trie_chain[trie_count - 1]; /* lowest level trie block */
    idx = trie_idx[trie_count - 1]; /* index in lowest level trie block */

    /*
     * XXX: backward hook from low to high level trie block, for the sake of data plane reading.
     *      Since control plane bind to core 0, we only need disable preemption when multiple
     *      control threads are co-exist.
     */
    for (level = trie_count - 1; level > 0; level--) {
        pre_entry = (mtrie_node_t *)(trie_chain[level - 1] + trie_idx[level - 1]);

        if (trie_chain_alloc[level] == TRIE_CHAIN_ALLOC) {
            /* Hook newly allocating trie block to upper level trie block entry's base */
            pre_entry->base = trie_chain[level];
        } else {
            /* Inconsistence check */
            if (pre_entry->base != trie_chain[level]) {
                /* XXX BUG */
                lpm_debug_alg(table, "*BUG* *FATAL ERROR* trie_chain[%d]=%p, pre_node->base=%p, *INCONSISTENT*\n",
                                      level, trie_chain[level], pre_entry->base);
                lpm_log_print(table, "*FATAL ERROR* : *inconsistent*\n");
                lpm_con_print("*FATAL ERROR* : *inconsistent*\n");
                assert(0);  /* XXX suicide */
                return LPM_ERR_INTERNAL;
            }
        }
    }
    
    /* trie_chain: mtrie_table_base -> ... -> frontier_trie */
    switch (nextbit) {
    case 0: /* next bit should set to 0 */
        assert(!BOUNDARY_BIT_POSITION(temp_bitpos));
        idx &= (0xFF ^ (0x1 << (7 - ((temp_bitpos + 1) & 7))));
        lpm_pattern_generate(frontier_trie, idx, (temp_bitpos + 1), data);
        break;
    case 1: /* next bit should set to 1 */
        assert(!BOUNDARY_BIT_POSITION(temp_bitpos));
        idx |= (0x1 << (7 - ((temp_bitpos + 1) & 7)));
        lpm_pattern_generate(frontier_trie, idx, (temp_bitpos + 1), data);
        break;
    case -1:/* next bit need no touch */
        lpm_pattern_generate(frontier_trie, idx, temp_bitpos, data);
        break;
    }

    return ret;
}

static lpm_result_t __lpm_prefix_expansion(lpm_lkup_table_t *table,
                                         u8 *addr,
                                         u32 masklen,
                                         u32 temp_bitpos,
                                         btrie_node_t *temp_root,
                                         void *data,
                                         u32 *recur_times)
{
    lpm_result_t ret;
    
#if LPM_DEBUG_RECURSION
    if (*recur_times > LPM_RECUR_DEPTH_WARN) {
        lpm_con_print("%s *BUG WARNING* recursion times = %u, too deep\n", __func__, *recur_times);
    }
    *recur_times = *recur_times + 1;
#endif

    /* Boundary bit specify only one entry in m-trie block. Combinations directly. */
    if (BOUNDARY_BIT_POSITION(temp_bitpos)) {
        return lpm_gen_combinations(table, addr, temp_bitpos, data, -1);
    }

    if ((temp_root->child[0] == NULL) && (temp_root->child[1] == NULL)) {
        /* No children in 1-trie, which means I AM the most specific data. Combinations directly. */
        return lpm_gen_combinations(table, addr, temp_bitpos, data, -1);
    }

    /* Take care left sub-tree. */
    if (temp_root->child[0] != NULL) {  /* More specific data maybe exist in left sub-tree */
        
        if (temp_root->child[0]->data == NULL) {
            /* Take care left sub-tree recursively */
            CLEAR_BIT_AT_POSITION(addr, (temp_bitpos + 1));

            ret = __lpm_prefix_expansion(table,
                                         addr,
                                         masklen,
                                         (temp_bitpos + 1),
                                         temp_root->child[0],
                                         data,
                                         recur_times);
            
#if LPM_DEBUG_RECURSION
            *recur_times = *recur_times - 1;
#endif

            if (ret != LPM_SUCCESS) {
                return ret;
            }
        } else {
            /*
             * The temp_root->child[0]->data is not NULL, which means it is the more specific data,
             * and it will take over all below nodes. We have done our job now.
             */
            (void)0; 
        }
        
    } else {
        /* Left sub-tree not exist, combination directly in left sub-tree. */
        ret = lpm_gen_combinations(table, addr, temp_bitpos, data, 0);
        if (ret != LPM_SUCCESS) {
            return ret;
        }
    }

    /* Take care right sub-tree. */
    if (temp_root->child[1] != NULL) {  /* More specific data maybe exist in right sub-tree */
        
        if (temp_root->child[1]->data == NULL) {
            /* Take care right sub-tree recursively */
            SET_BIT_AT_POSITION(addr, (temp_bitpos + 1));

            ret = __lpm_prefix_expansion(table,
                                         addr,
                                         masklen,
                                         (temp_bitpos + 1),
                                         temp_root->child[1],
                                         data,
                                         recur_times);

#if LPM_DEBUG_RECURSION
            *recur_times = *recur_times - 1;
#endif

            if (ret != LPM_SUCCESS) {
                return ret;
            }
        } else {
            /*
             * The temp_root->child[1]->data is not NULL, which means it is the more specific data,
             * and it will take over all below nodes. We have done our job now.
             */
            (void)0; 
        }
        
    } else {
        /* Right sub-tree not exist, combination directly in right sub-tree. */
        ret = lpm_gen_combinations(table, addr, temp_bitpos, data, 1);
        if (ret != LPM_SUCCESS) {
            return ret;
        }
    }

    return LPM_SUCCESS;
}

static lpm_result_t lpm_prefix_expansion(lpm_lkup_table_t *table,
                                         u8 *addr,
                                         u32 masklen,
                                         u32 temp_bitpos,
                                         btrie_node_t *temp_root,
                                         void *data)
{
    u32 recur_times = 0;

    return __lpm_prefix_expansion(table, addr, masklen, temp_bitpos, temp_root, data, &recur_times);
}

/* Update LPM default data, accroding to addr/masklen prefix. */
lpm_result_t lpm_update_default_data(lpm_lkup_table_t *table, u8 *addr, u32 masklen)
{
    void *data = NULL;
    int cnt;
    u8 mask;

    if (lpm_check_arg(table, addr, masklen) != LPM_SUCCESS) {
        lpm_con_print("%s invalid argument\n", __func__);
        return LPM_ERR_INVALID;
    }

    if (table->btrie_root == NULL) {
        lpm_debug_alg(table, "B-trie of LPM not exists\n");
        return LPM_ERR_INTERNAL;
    }

    data = btrie_find_data(table->btrie_root, addr, masklen);
    if (data == NULL) {
        lpm_debug_norm(table, "not find valid data to update default\n");
        return LPM_ERR_NOTFOUND;
    }

    table->default_data = data;
    table->default_masklen = masklen;
    memset(&(table->default_addr), 0, sizeof(table->default_addr));
    if (masklen > 0) {
        cnt = ((masklen - 1) >> 0x3) + 1;
        memcpy(&(table->default_addr), addr, cnt);
        mask = 0xFF ^ ((1 << (0x7 - ((masklen - 1) & 0x7))) - 1);
        table->default_addr[cnt - 1] &= mask;
    }
    
    lpm_log_print(table, "update default data with <%p> success\n", data);

    return LPM_SUCCESS;
}

/*
 * Take care of "more specifc" data, and do not overwriting it.
 */
lpm_result_t lpm_add_entry(lpm_lkup_table_t *table, u8 *addr, u32 masklen, void *data)
{
    btrie_node_t *newnode = NULL, *append_point = NULL;
    lpm_result_t ret;
    u32 bitpos;
    u8 temp_addr[LPM_LEVEL_MAX], cnt, append_bit = 0;
    int already_exist = 0;

    ret = lpm_check_arg(table, addr, masklen);
    if (ret != LPM_SUCCESS) {
        return ret;
    }
    if (data == NULL) {
        lpm_con_print("%s can not add NULL data\n", __func__);
        return LPM_ERR_INVALID;
    }
    if (table->btrie_root == NULL || table->hi256_table_base == NULL) {
        lpm_debug_alg(table, "B-trie or M-trie of LPM not exists\n");
        return LPM_ERR_INTERNAL;
    }

    /*
     * btrie_add_entry will never fail to return a 1-trie node.
     * If node is newly added, append_point will be used in rollback.
     */
    ret = btrie_add_node(table, addr, masklen, &newnode, &append_point, &append_bit);
    if (ret != LPM_SUCCESS) {
        if (ret == LPM_ERR_RESOURCES) {
            lpm_debug_mem(table, "get b-trie node [%d Bytes] failed, due to memory allocate\n",
                                    sizeof(btrie_node_t));
            /*
             * Rollback operation due to b-trie node adding failure.
             * Since variable newnode is not used, it can be used as temporary variable.
             */
            newnode = append_point->child[append_bit];
            append_point->child[append_bit] = NULL;
            btrie_del_appended(table, newnode);
            return LPM_ERR_RESOURCES;
        } else if (ret == LPM_ERR_EXISTS) { /* 1-trie node exists already */
            already_exist = 1;
        } else {                            /* Other error must return directly */
            return ret;
        }
    }

    assert(newnode != NULL);
    
    if (newnode->data != NULL) {
        /* data has already exists */
        if (newnode->data == data) {
            lpm_debug_norm(table, "data <%p> alreadly exists\n", data);
            return LPM_ERR_EXISTS;
        } else {
            lpm_debug_norm(table, "data <%p> conflict with new data <%p>\n", newnode->data, data);
            return LPM_ERR_CONFLICT;
        }
    }

    /* add data in 1-trie */
    newnode->data = data;
    table->stat.data_total++;
    table->stat.data_per_masklen[masklen]++;

    /*
     * Zero route can be done now.
     */
    if (masklen == 0) {
        lpm_log_print(table, "add data<%p> success\n", data);
        return LPM_SUCCESS;
    }

    memset(&temp_addr, 0, sizeof(temp_addr));
    cnt = ((masklen - 1) >> 0x3) + 1;
    memcpy(&temp_addr, addr, cnt);  /* Copy addr information to local variable */

    bitpos = masklen - 1;
    ret = lpm_prefix_expansion(table, temp_addr, masklen, bitpos, newnode, data);
    if (ret != LPM_SUCCESS) {
        if (ret == LPM_ERR_RESOURCES) {
            newnode->data = NULL;
            table->stat.data_total--;
            table->stat.data_per_masklen[masklen]--;
            if (already_exist) {
                /* The newnode is not NEWLY added, this is easy situation */
                lpm_debug_alg(table, "btrie node already exists, but mtrie block alloc failed\n");
            } else {
                /*
                 * XXX: rollback 1-trie, newnode can be used as temporary variable.
                 */
                newnode = append_point->child[append_bit];
                append_point->child[append_bit] = NULL;
                btrie_del_appended(table, newnode);
                lpm_debug_alg(table, "btrie node alloc success but mtrie block failed\n");
            }
        } else {
            /* XXX BUG */
            lpm_debug_alg(table, "*BUG* *ERROR* mtrie block failed, ret %d\n", ret);
            lpm_con_print("*BUG* *ERROR* mtrie block failed, ret %d\n", ret);
            assert(0);  /* XXX: suicide */
            return LPM_ERR_INTERNAL;
        }
    }

    lpm_log_print(table, "add data<%p> success\n", data);

    return ret;
}

lpm_result_t lpm_update_entry(lpm_lkup_table_t *table, u8 *addr, u32 masklen, void *data)
{
    lpm_result_t ret;
    btrie_node_t *stored_node;
    u32 bitpos;
    u8 temp_addr[LPM_LEVEL_MAX], cnt;

    ret = lpm_check_arg(table, addr, masklen);
    if (ret != LPM_SUCCESS) {
        return ret;
    }
    if (data == NULL) {
        lpm_con_print("%s can not using NULL data to update\n", __func__);
        return LPM_ERR_INVALID;
    }
    if (table->btrie_root == NULL || table->hi256_table_base == NULL) {
        lpm_debug_alg(table, "B-trie or M-trie of LPM not exists\n");
        return LPM_ERR_INTERNAL;
    }

    stored_node = btrie_find_node(table->btrie_root, addr, masklen);
    if ((stored_node == NULL) || (stored_node->data == NULL)) {
        lpm_con_print("%s not find valid data; please use lpm_add_entry() first\n", __func__);
        return LPM_ERR_NOTFOUND;
    }
    
    if (stored_node->data == data) {
        lpm_debug_norm(table, "data <%p> are the same\n", data);
    } else {
        /* update data in 1-btrie */
        stored_node->data = data;
    }

    if (masklen == 0) {
        lpm_log_print(table, "update success\n");
        return LPM_SUCCESS;
    }
    
    memset(&temp_addr, 0, sizeof(temp_addr));
    cnt = ((masklen - 1) >> 0x3) + 1;
    memcpy(&temp_addr, addr, cnt);

    bitpos = masklen - 1;
    /* Update data in m-trie */
    ret = lpm_prefix_expansion(table, temp_addr, masklen, bitpos, stored_node, data);

    lpm_log_print(table, "update success\n");

    return ret;
}

/* Take care only default route and data in LPM table */
lpm_result_t lpm_del_default_data(lpm_lkup_table_t *table)
{
    lpm_result_t ret = LPM_SUCCESS;

    if (table == NULL) {
        lpm_con_print("%s failed, table not found...\n", __func__);
        return LPM_ERR_INVALID;
    }

    if (table->default_data == NULL) {
        lpm_debug_norm(table, "default data do not exist\n");
        return LPM_ERR_NOTFOUND;
    }

    table->default_data = NULL;
    table->default_masklen = 0;
    memset(&(table->default_addr), 0, sizeof(table->default_addr));

    lpm_log_print(table, "success\n");

    return ret;
}

/* Zero out corresponding entrys' data in m-trie block */
static lpm_result_t zero_out_data(lpm_lkup_table_t *table, u8 *addr, u32 masklen)
{
    lpm_result_t ret = LPM_SUCCESS;
    u8 idx;
    int level;
    mtrie_node_t *entry, *trie;
    mtrie_node_t *table_base = table->hi256_table_base;

    idx = addr[0];

    if (masklen <= 8) { /* hi256_table_base operating directly */
        lpm_pattern_generate(table_base, idx, (masklen - 1), NULL);
        return LPM_SUCCESS;
    }

    entry = (mtrie_node_t *)(table_base + idx);
    entry->data = NULL;
    trie = entry->base;
    if (trie == NULL) {
        lpm_debug_alg(table, "mtrie block do not exist\n");
        return LPM_ERR_INTERNAL;
    }
    for (level = 1; (trie != NULL) && (level < 16); level++) {
        idx = addr[level];
        if ((masklen - (level << 3)) <= 8) {
            /* we are in this trie block */
            lpm_debug_norm(table, "idx<%u>, bitpos<%u>\n", idx, masklen - 1);
            lpm_pattern_generate(trie, idx, (masklen - 1), NULL);
            break;
        }
        entry = (mtrie_node_t *)(trie + idx);
        entry->data = NULL;
        trie = entry->base;
    }

    return ret;
}

static void delete_trie_block(lpm_lkup_table_t *table, u8 *addr, u32 bitpos)
{
    u8 idx;
    int level, trie_count, i;
    mtrie_node_t *entry, *trie;

    assert(BOUNDARY_BIT_POSITION(bitpos));  /* must be boundary */
    lpm_debug_norm(table, "bitpos %u must be boundary\n", bitpos);

    trie_count = (bitpos >> 3) + 1;
    trie = table->hi256_table_base;

    /* Find the trie block to delete */
    for (level = 0; (trie != NULL) && (level < trie_count); level++) {
        idx = addr[level];
        
        entry = (mtrie_node_t *)(trie + idx);
        trie = entry->base;
    }

    entry->base = NULL;                     /* delete trie block from LPM m-trie */
    if (trie != NULL) {
        for (i = 0; i < MTRIE_BLOCK_ENTRY; i++) {
            /* Inconsistence check */
            entry = (mtrie_node_t *)(trie + i);
            if (entry->base != NULL) {
                /* XXX BUG */
                lpm_debug_alg(table, "*BUG*, bitpos %u, sub-block entry[%d]'s base not null\n",
                                        bitpos, i);
                lpm_log_print(table, "*BUG*, bitpos %u, sub-block entry[%d]'s base not null\n",
                                        bitpos, i);
                lpm_con_print("*BUG*, bitpos %u, sub-block entry[%d]'s base not null\n", bitpos, i);
                exit(-1);
            }
        }
        mtrie_free_block(table, trie);
    }
}

/*
 * Recursively check and delete sub-tree in 1-trie and m-trie.
 */
static int __delete_subtree(lpm_lkup_table_t *table,
                            u8 *addr,
                            btrie_node_t *temp_root,
                            u32 bitpos,
                            u32 *recur_times)
{
    int delete_left = 1;
    int delete_right = 1;
    
#if LPM_DEBUG_RECURSION
    if (*recur_times > LPM_RECUR_DEPTH_WARN) {
        lpm_con_print("%s *BUG WARNING* recursion times = %u, too deep\n", __func__, *recur_times);
    }
    *recur_times = *recur_times + 1;
#endif

    if ((temp_root->child[0] == NULL) && (temp_root->child[1] == NULL)) {
        if (temp_root->data == NULL) {
            /* temp_root has no data, can delete it. */
            return 1;
        } else {
            /* temp_root has data, so do not touch it. */
            return 0;
        }
    }

    /* Left sub-tree operation */
    if (temp_root->child[0] != NULL) {
        delete_left = __delete_subtree(table,
                                       addr,
                                       temp_root->child[0],
                                       bitpos + 1,
                                       recur_times);

#if LPM_DEBUG_RECURSION
        *recur_times = *recur_times - 1;
#endif

        if (delete_left) {
            /* Left sub-tree can be destroyed. */
            btrie_destroy_subtree(table, temp_root->child[0]);
            temp_root->child[0] = NULL;
        } else {
            /* Left sub-tree has more specific data, do not touch it. */
            return 0;
        }
    }

    /* Right sub-tree operation */
    if (temp_root->child[1] != NULL) {
        delete_right = __delete_subtree(table,
                                        addr,
                                        temp_root->child[1],
                                        bitpos + 1,
                                        recur_times);

#if LPM_DEBUG_RECURSION
        *recur_times = *recur_times - 1;
#endif

        if (delete_right) {
            btrie_destroy_subtree(table, temp_root->child[1]);
            temp_root->child[1] = NULL;
        } else {
            return 0;
        }
    }

    if ((delete_left && delete_right) && (temp_root != table->btrie_root)) {
        if (BOUNDARY_BIT_POSITION(bitpos)) {
            /*
             * While recusively deleting 1-trie node from lowest to highest level, if we meet
             * boundary bit, we delete mtrie block too. */
            delete_trie_block(table, addr, bitpos);
        }

        /*
         * temp_root's left and right sub-tree is clear now, we can delete temp_root now.
         */
        return 1;
    }

    return 0;
}

static int delete_subtree(lpm_lkup_table_t * table,
                            u8 * addr,
                            btrie_node_t * temp_root,
                            u32 bitpos)
{
    u32 recur_times = 0;

    return __delete_subtree(table, addr, temp_root, bitpos, &recur_times);
}

static lpm_result_t __lpm_del_entry(lpm_lkup_table_t *table, u8 *addr, u32 masklen)
{
    lpm_result_t ret = LPM_SUCCESS;
    btrie_node_t *node;
    btrie_node_t *last_known_node;
    void *last_known_data = NULL;
    u32 last_known_bitpos = 0, bitpos = 0;
    int last_known_hit_trie, node_hit_trie;
    u8 bit;

    node = table->btrie_root;
    /* XXX: last_known_node will never be assigned by zero route data */
    last_known_node = node;
    for (bitpos = 0; bitpos < masklen; bitpos++) {
        bit = bit_at_position(addr, bitpos);
        node = node->child[bit];
        if (node == NULL) {
            lpm_debug_norm(table, "do not find corresponding node in b-trie\n");
            return LPM_ERR_NOTFOUND;
        }
        if ((node->data != NULL) && (bitpos != (masklen - 1))) {
            last_known_node = node;         /* Record less specific node data */
            last_known_data = node->data;
            last_known_bitpos = bitpos;
        }
    }

    if (node->data == NULL) {
        lpm_debug_norm(table, "do not find valid data in b-trie\n");
        return LPM_ERR_NOTFOUND;
    }

    bitpos = masklen - 1;
    /* Delete data in 1-trie. */
    node->data = NULL;
    assert(table->stat.data_total > 0);
    table->stat.data_total--;
    table->stat.data_per_masklen[masklen]--;
    
    /* XXX FIXME TODO: rollback needed while deletion fails ??? */

    if (last_known_data != NULL) {
        /* Less specific data exists, using it to restore deleted data in m-trie */
        node_hit_trie = bitpos >> 3;
        last_known_hit_trie = last_known_bitpos >> 3;
        if (node_hit_trie == last_known_hit_trie) {
            /* Less specific data and deleted data are in the same trie block, restore directly. */
            ret = lpm_prefix_expansion(table,
                                       addr,
                                       masklen,
                                       last_known_bitpos,
                                       last_known_node,
                                       last_known_data);
        } else {
            /*
             * Less specific data and deleted data are NOT in the same trie block,
             * zero out directly just like adding NULL data at addr/masklen.
             */
            ret = lpm_prefix_expansion(table,
                                       addr,
                                       masklen,
                                       bitpos,
                                       node,
                                       NULL);
        }
    } else {
        /* Less specific data is not existing. */
        if ((node->child[0] != NULL) || (node->child[1] != NULL)) {
            /*
             * Since more specific data existes, we need zero out just like add NULL data at
             * the addr/masklen prefix.
             */
            ret = lpm_prefix_expansion(table,
                                       addr,
                                       masklen,
                                       bitpos,
                                       node,
                                       NULL);
        } else {
            /* More specific data is not existing too, just zero out all data top-down. */
            ret = zero_out_data(table, addr, masklen);
        }
    }

    if (ret != LPM_SUCCESS) {
        return ret;
    }

    /* When the less specific data not exists, we check and delete subtree from 1-trie root. */
    if (last_known_node == table->btrie_root) {
        last_known_bitpos = -1;
    }

    delete_subtree(table, addr, last_known_node, last_known_bitpos);

    return LPM_SUCCESS;
}

/*
 * TODO FIXME XXX: when delete the prefix which is assigned to be default route, how ???
 */
lpm_result_t lpm_del_entry(lpm_lkup_table_t *table, u8 *addr, u32 masklen)
{
    lpm_result_t ret;
    u8 temp_addr[LPM_LEVEL_MAX], cnt;

    ret = lpm_check_arg(table, addr, masklen);
    if (ret != LPM_SUCCESS) {
        return ret;
    }
    if (table->btrie_root == NULL || table->hi256_table_base == NULL) {
        lpm_debug_alg(table, "B-trie or M-trie of LPM not exists\n");
        return LPM_ERR_INTERNAL;
    }

    if (masklen == 0) {
        if (table->btrie_root->data == NULL) {
            lpm_debug_norm(table, "do not find valid data in b-trie\n");
            ret = LPM_ERR_NOTFOUND;
            goto finish;
        }
        
        table->btrie_root->data = NULL;
        assert(table->stat.data_total > 0);
        table->stat.data_total--;
        table->stat.data_per_masklen[masklen]--;

        ret = LPM_SUCCESS;
        goto finish;
    }
    
    memset(&temp_addr, 0, sizeof(temp_addr));
    cnt = ((masklen - 1) >> 0x3) + 1;
    memcpy(&temp_addr, addr, cnt);

    ret = __lpm_del_entry(table, temp_addr, masklen);

finish:
    
    lpm_log_print(table, "delete entry return %d\n", ret);

    return ret;
}

/* Depth first traversal. Traversing all 1-trie data and then print default data */
lpm_result_t lpm_walk_entry(lpm_lkup_table_t *table, lpm_data_walker_func_t walker)
{
    lpm_result_t ret = LPM_SUCCESS;

    if (table == NULL) {
        lpm_con_print("%s table not found...\n", __func__);
        return LPM_ERR_INVALID;
    }

    if (walker == NULL) {
        lpm_con_print("%s walker function not valid\n", __func__);
        return LPM_ERR_INVALID;
    }
    if (table->btrie_root == NULL) {
        lpm_debug_alg(table, "B-trie of LPM not exists\n");
        return LPM_ERR_INTERNAL;
    }

    ret = btrie_dfs_walk(table->btrie_root, walker);

    if (table->default_data != NULL) {
        /* Print default data */
        lpm_con_print("Default data: ----------------\n");
        if ((*walker)(table->default_addr, table->default_masklen, table->default_data) != 0) {
            ret = LPM_ERR_EXOTIC;
        }
    }

    lpm_log_print(table, "using walker <%p>\n", walker);

    return ret;
}

void lpm_dump_mtrie(lpm_lkup_table_t *table)
{
    if (table == NULL) {
        lpm_con_print("%s table not found...\n", __func__);
        return;
    }

    lpm_log_print(table, "...\n");

    return;
}

