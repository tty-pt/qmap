#ifndef QMAP_H
#define QMAP_H

#include <stddef.h>
#include <stdlib.h>

#define QMAP_MAX 1024
#define QMAP_MAX_COMBINED_LEN (BUFSIZ * 2)
#define QMAP_MISS ((unsigned) -1)

enum qmap_mbr {
	QMAP_KEY = 0,
	QMAP_VALUE = 1,
};

enum qmap_flags {
	QMAP_DUP = 1,
	QMAP_AINDEX = 2,
	QMAP_PGET = 4,
	QMAP_TWO_WAY = 8,
};

typedef size_t qmap_measure_t(const void * const value);

typedef int qmap_compare_t(const void * const a,
		const void * const b, size_t len);

typedef int qmap_print_t(char * const target,
		const void * const value);

typedef struct qmap_type {
	size_t len; // length if fixed
	qmap_measure_t *measure;   // for variable length
	qmap_compare_t *compare;   // only used in test
	qmap_print_t   *print;     // same
	struct qmap_type *part[2]; // for composed types
} qmap_type_t;

/* Association callbacks follow this format */
typedef int qmap_assoc_t(
		const void ** const data,
		const void * const key,
		const void * const value);

/* Initialize the system */
void qmap_init(void);

unsigned qmap_open(const char * const key_tid,
		const char * const value_tid,
		unsigned mask, unsigned flags);

/* Close a qmap */
void qmap_close(unsigned hd);

/* Get a value from a key */
int qmap_get(unsigned hd, void * const destiny,
		const void * const key);

/* Put a value and a key, or maybe just a value, if
 * you have AINDEX on. (use NULL as the key for that)
 */
unsigned qmap_put(unsigned hd, const void * const key,
		const void * const value);

/* Delete a key-value pair. Or perhaps all values for a key
 * (use NULL as the value for that)
 */
void qmap_del(unsigned hd, const void * const key,
		const void * const value);

/* drop an entire qmap's contents */
void qmap_drop(unsigned hd);

/* Start iterating over a qmap. Key might be NULL or a
 * pointer to some key you want to search.
 * Returns a cursor handle.
 */
unsigned qmap_iter(unsigned hd, const void * const key);

/* Move the cursor to the next item. It returns one if there
 * is something there, or zero if there isn't. Use it in a
 * while loop, for example.
 */
int qmap_lnext(unsigned cur_id);

/* When finishing iteration early, use this to clean up. */
void qmap_fin(unsigned cur_id);

/* Get the item under the cursor. */
void qmap_cget(void * const target,
		unsigned cur_id, enum qmap_mbr t);

/* Delete the item under the cursor. */
void qmap_cdel(unsigned cur_id);

/* Like qmap_lnext except that it takes care of copying the
 * values to wherever you like.
 */
static inline int
qmap_next(void * const key, void * const value,
		unsigned cur_id)
{
	if (!qmap_lnext(cur_id))
		return 0;

	qmap_cget(key, cur_id, QMAP_KEY);
	qmap_cget(value, cur_id, QMAP_VALUE);
	return 1;
}

/* Associate a qmap (hd) with a primary qmap (link), using
 * "cb" to generate secondary keys. Effectively, it makes
 * hd a secondary database.
 */
void qmap_assoc(unsigned hd,
		unsigned link, qmap_assoc_t cb);

/* Get a primary key from a secondary qmap */
int qmap_pget(unsigned hd, void *target, void *key);

/* Measure the length of something based on its type
 * association.
 */
size_t qmap_len(unsigned hd,
		const void * const value, enum qmap_mbr t);

/* Print a thing based on its type association. */
void qmap_print(char * const target, unsigned hd,
		unsigned type, const void * const thing);

/* Register a type by type pointer */
void qmap_regc(char *key, qmap_type_t *type);

/* Register a type by string (fixed length) */
static inline void
qmap_reg(char *key, size_t len)
{
	qmap_type_t *type = (qmap_type_t *)
		malloc(sizeof(qmap_type_t));
	type->measure = NULL;
	type->len = len;
	type->print = NULL;
	qmap_regc(key, type);
}

/* Compare two values */
int qmap_cmp(unsigned hd, enum qmap_mbr t,
		const void * const a,
		const void * const b);

/* Get the type string of a member */
const char *qmap_type(unsigned hd, enum qmap_mbr t);

/* Get the flags of a qmap */
unsigned qmap_flags(unsigned hd);

#endif
