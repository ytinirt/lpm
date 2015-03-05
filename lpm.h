/*
 * lpm.h
 *
 * Longest prefix matching public header file.
 *
 * ATTENTION:
 *     1. Mask length is limited to LPM_MASKLEN_MAX(128) internally.
 *     2. LPM table name's length (including '\0') is limited to LPM_TABLE_NAME_LEN(32) internally.
 *     3. Type of data which stored in LPM table is (void *) and it is implementation independent.
 *     4. Zero route (0.0.0.0/0 or ::/0)'s data stored in 1-trie's root node, NOT stored in m-trie.
 *        Default route stored in LPM table control block. Default route is the copy of one prefix
 *        stored in 1-trie. Of course, the default route could come from zero route data or any
 *        other common prefix data.
 *     5. LPM table DO NOT take care of data's reference count.
 *
 * History
 */

#ifndef _LPM_H_
#define _LPM_H_

#include <stdint.h>

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;

/**
 * LPM debug option types, more details in lpm_debug_support().
 */
typedef enum lpm_debug_e {
    DEBUG_NORMAL = 0,   /* General warning, error or auxiliary information */
    DEBUG_MEMORY,       /* Memory operation informations */
    DEBUG_ALGORITHM,    /* Internal warning or information of LPM Algorithm */
    DEBUG_ALL,          /* All debug messages and logging messages */

    LOGGING,            /* Logging messages */
} lpm_debug_t;

/**
 * LPM operation results.
 */
typedef enum lpm_result_e {
    LPM_SUCCESS = 0,    /* Success */
    LPM_ERR_RESOURCES,  /* Memory resource request failed */
    LPM_ERR_INVALID,    /* Invalid input arguments */
    LPM_ERR_INTERNAL,   /* Internal error of LPM Algorithm */
    LPM_ERR_NOTFOUND,   /* Data not found */
    LPM_ERR_EXISTS,     /* Data already exist (be the same) */
    LPM_ERR_CONFLICT,   /* Data already exist (not the same) */
    LPM_ERR_EXOTIC,     /* Exotic errors, not from LPM, eg. data_walker callback function */
} lpm_result_t;

/**
 * LPM table control block structure.
 */
struct lpm_lkup_table_s;
typedef struct lpm_lkup_table_s lpm_lkup_table_t;

/**
 * lpm_create_table - create LPM table
 * @name: name string of LPM table, eg. "IPv4" or "IPv6"
 *
 * Return pointer of LPM table for success,
 *      or NULL for failure.
 */
lpm_lkup_table_t *lpm_create_table(char *name);

/**
 * lpm_destroy_table - destroy and release LPM table
 * @table: LPM table pointer
 *
 * Return LPM operation results.
 */
lpm_result_t lpm_destroy_table(lpm_lkup_table_t *table);

/**
 * lpm_table_statistic - print LPM table statistic
 * @table: LPM table pointer
 *
 * No return value.
 */
void lpm_table_statistic(lpm_lkup_table_t *table);

/**
 * lpm_dump_mtrie - print all data in M-trie, for the sake of debugging
 * @table: LPM table pointer
 *
 * No return value.
 */
void lpm_dump_mtrie(lpm_lkup_table_t *table);

/**
 * lpm_debug_support - LPM debug switch
 * @table: LPM table pointer
 * @debug: debug option type
 * @on: 0 for closing, 1 for opening, other value leads to failure
 *
 * Return LPM operation results.
 */
lpm_result_t lpm_debug_support(lpm_lkup_table_t *table, lpm_debug_t debug, int on);

/**
 * lpm_search_table - loggest prefix matching search in M-trie
 * @table: LPM table pointer
 * @addr: pointer of address, ATTENTION address should be network byte order (big endianness)
 * @using_default: default data is used, *using_default should set to non-zero, otherwise set to 0
 *
 * Return the valid data for success,
 *      or NULL for failure.
 */
void *lpm_search_table(lpm_lkup_table_t *table, u8 *addr, u8 *using_default);

/**
 * lpm_find_entry - accurately search in 1-trie
 * @table: LPM table pointer
 * @addr: pointer of address, ATTENTION address should be network byte order (big endianness)
 * @masklen: mask length value
 *
 * Return the valid data for success,
 *      or NULL for failure.
 */
void *lpm_find_entry(lpm_lkup_table_t *table, u8 *addr, u32 masklen);

/**
 * lpm_add_entry - adding the prefix and data
 * @table: LPM table pointer
 * @addr: pointer of address, ATTENTION address should be network byte order (big endianness)
 * @masklen: mask length value
 * @data: data to be add
 *
 * Return LPM operation results.
 */
lpm_result_t lpm_add_entry(lpm_lkup_table_t *table, u8 *addr, u32 masklen, void *data);

/**
 * lpm_update_entry - updating the prefix and data
 * @table: LPM table pointer
 * @addr: pointer of address, ATTENTION address should be network byte order (big endianness)
 * @masklen: mask length value
 * @data: new data used for updating
 *
 * Return LPM operation results.
 */
lpm_result_t lpm_update_entry(lpm_lkup_table_t *table, u8 *addr, u32 masklen, void *data);

/**
 * lpm_update_default_data - update default data using the appointed prefix's data
 * @table: LPM table pointer
 * @addr: pointer of address, ATTENTION address should be network byte order (big endianness)
 * @masklen: mask length value
 *
 * Return LPM operation results.
 */
lpm_result_t lpm_update_default_data(lpm_lkup_table_t *table, u8 *addr, u32 masklen);

/**
 * lpm_del_entry - deleting the prefix and data
 * @table: LPM table pointer
 * @addr: pointer of address, ATTENTION address should be network byte order (big endianness)
 * @masklen: mask length value
 *
 * Return LPM operation results.
 */
lpm_result_t lpm_del_entry(lpm_lkup_table_t *table, u8 *addr, u32 masklen);

/**
 * lpm_del_default_data - delete default data, and will not touch data in 1-trie of course
 * @table: LPM table pointer
 *
 * Return LPM operation results.
 */
lpm_result_t lpm_del_default_data(lpm_lkup_table_t *table);

/**
 * Traversal function's type defination, used in LPM prefix and data.
 * @addr: pointer of address, ATTENTION address should be network byte order (big endianness)
 * @masklen: mask length value
 * @data: data of corresponding prefix (addr/masklen)
 *
 * Return 0 for success, non-0 for failure.
 * 
 */
typedef int (*lpm_data_walker_func_t)(u8 *addr, u32 masklen, void *data);

/**
 * lpm_walk_entry - traverse all prefix and data stored in 1-trie
 * @table: LPM table pointer
 * @walker: callback function used for each data of LPM when traversing
 *
 * Return LPM operation results.
 */
lpm_result_t lpm_walk_entry(lpm_lkup_table_t *table, lpm_data_walker_func_t walker);

#endif /* !_LPM_H_ */

