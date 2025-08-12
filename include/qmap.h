#ifndef QMAP_H
#define QMAP_H

#include <stddef.h>

#define QMAP_MAX 1024
#define QMAP_MAX_COMBINED_LEN (BUFSIZ * 2)
#define QMAP_MISS ((unsigned) -1)

enum qmap_member {
	QMAP_KEY = 0,
	QMAP_VALUE = 1,
};

enum qmap_flags {
	QMAP_DUP = 1,
	QMAP_AINDEX = 2,
	QMAP_PGET = 4,
	QMAP_TWO_WAY = 8,
};

typedef size_t qmap_measure_t(void *value);
typedef int qmap_compare_t(void *a, void *b, size_t len);
typedef int qmap_print_t(char *target, void *value);

typedef struct qmap_type {
	size_t len; // length if fixed
	qmap_measure_t *measure;   // for variable length
	qmap_compare_t *compare;   // only used in test
	qmap_print_t   *print;     // same
	struct qmap_type *part[2]; // for composed types
} qmap_type_t;

/* association callbacks should have this format */
typedef int qmap_assoc_t(
		void **data, void *key, void *value);

/* initialize the system */
void qmap_init(void);

/* initialize a new qmap */
unsigned qmap_open(qmap_type_t *key_type,
		qmap_type_t *value_type,
		unsigned mask, unsigned flags);

/* get rid of a qmap */
void qmap_close(unsigned hd);

/* get a value from a key */
int qmap_get(unsigned hd, void *destiny, void *key);

/* put a value and a key, or maybe just a value, if
 * you have AINDEX on. (use NULL as the key for that)
 */
unsigned qmap_put(unsigned hd, void *key, void *value);

/* delete a key-value pair. Or perhaps all values for
 * a key (use NULL as the value for that)
 */
void qmap_del(unsigned hd, void *key, void *value);

/* drop an entire qmap's contents */
void qmap_drop(unsigned hd);

/* start iteration over a qmap. Key might be NULL
 * or a pointer to some key you want to search.
 * Returns a cursor handle.
 */
unsigned qmap_iter(unsigned hd, void *key);

/* This moves the cursor, it returns one if there
 * is something there, or zero if there isn't. It's
 * good for while loops and the like.
 */
int qmap_lnext(unsigned cur_id);

/* This notes that you've finished iteration early.
 * In case you want to break out of an iteration loop.
 * It's not require but it accounts for better clean-up.
 */
void qmap_fin(unsigned cur_id);

/* This will return the item under the cursor to you. */
void qmap_cget(void *target,
		unsigned cur_id, enum qmap_member t);

/* This will delete the item under the cursor. */
void qmap_cdel(unsigned cur_id);

/* This is like qmap_lnext except that it takes care
 * of copying the values to wherever you like. A bit
 * overkill in some situations. That's why I broke
 * lnext out of it.
 */
static inline int
qmap_next(void *key, void *value, unsigned cur_id) {
	if (!qmap_lnext(cur_id))
		return 0;

	qmap_cget(key, cur_id, QMAP_KEY);
	qmap_cget(value, cur_id, QMAP_VALUE);
	return 1;
}

/* This associates a qmap (hd) with a primary
 * qmap (link), using "cb" to generate secondary keys.
 * Effectively, it makes hd a secondary database.
 */
void qmap_assoc(unsigned hd,
		unsigned link, qmap_assoc_t cb);

/* This is just for measuring the length of a
 * certain element. It's useful for qdb, maybe
 * not for much else.
 */
size_t qmap_len(unsigned hd, void *value, enum qmap_member member);

/* Likewise, you shouldn't often need this. I think.
 * It prints a certain thing.
 */
void qmap_print(char *target, unsigned hd,
		unsigned type, void *thing);

#endif
