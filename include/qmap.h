#ifndef QMAP_H
#define QMAP_H

#include <stddef.h>
#include <stdlib.h>

#define QMAP_MAX 1024
#define QMAP_MAX_COMBINED_LEN (BUFSIZ * 2)
#define QMAP_MISS ((unsigned) -1)

enum qmap_flags {
	QMAP_AINDEX = 1,
	QMAP_MIRROR = 2,
};

// this is used for associations. In case you want
// to know what maps it refers to inside the callback
// and what is the operation type being performed
enum qmap_op {
	QMAP_PUT = 0,
	QMAP_DEL = 1,
};

enum qmap_tbi {
	QMAP_DYNA = 0,
	QMAP_WORD = 1,
};

typedef size_t qmap_measure_t(const void * const value);

/* Association callbacks follow this format */
typedef int qmap_assoc_t(
		const void **skey,
		const void * const pkey,
		const void * const value);

/* Initialize the system */
void qmap_init(void);

unsigned qmap_sopen(unsigned type,
		unsigned mask, unsigned flags);

/* Associate a qmap (hd) with a primary qmap (link), using
 * "cb" to generate secondary keys. Effectively, it makes
 * hd a secondary database.
 */
void qmap_assoc(unsigned hd,
		unsigned link, qmap_assoc_t cb);

static inline unsigned
qmap_open(unsigned key_type, unsigned value_type,
		unsigned mask, unsigned flags)
{
	unsigned hd = qmap_sopen(key_type, mask, flags);

	if (!(flags & QMAP_MIRROR))
		return hd;

       	/* qmap_sopen(value_type, mask, flags & ~QMAP_AINDEX); */
       	qmap_sopen(value_type, mask, flags);
	qmap_assoc(hd + 1, hd, NULL);

	return hd;
}

/* Close a qmap */
void qmap_close(unsigned hd);

/* Get a value from a key */
unsigned qmap_get(unsigned hd, const void * const key);

/* Put a value and a key, or maybe just a value, if
 * you have AINDEX on. (use NULL as the key for that)
 */
unsigned qmap_put(unsigned hd,
		const void * const key,
		const void * const value // for assoc
		);


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
int qmap_next(unsigned *sid, unsigned cur_id);

/* When finishing iteration early, use this to clean up. */
void qmap_fin(unsigned cur_id);

/* Delete the Nth element. */
void qmap_ndel(unsigned cur_id, unsigned n);

/* Delete an item. */
static inline void
qmap_del(unsigned hd, const void * const key)
{
	unsigned cur = qmap_iter(hd, key), sn;

	while (qmap_next(&sn, cur))
		qmap_ndel(cur, sn);
}

/* Register a type (might have fixed-len or not) */
unsigned qmap_reg(size_t len);

/* Get the flags of a qmap */
unsigned qmap_flags(unsigned hd);

#endif
