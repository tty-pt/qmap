#ifndef QMAP_H
#define QMAP_H

#include <stddef.h>
#include <stdlib.h>

#define QM_MISS ((unsigned) -1)

enum qmap_flags {
	// QM_AINDEX: puts with NULL key yield
	// increasing indices
	QM_AINDEX = 1,

	// QM_MIRROR: create reverse-lookup (secondary) map
	QM_MIRROR = 2,
};

// built-in types
enum qmap_tbi {
	// Hash the pointer address (NOT the content)
	QM_HASH = 0,

	// Use the value as an opaque handle (no hashing)
	QM_HNDL = 1,

	// String contents hash and compare
	QM_STR = 2,
};

/* Initialize the system */
void qmap_init(void);

/* Open a database
 *
 * @param ktype
 * 	Key type: QM_HASH (0) or QM_HNDL (1).
 *
 * @param vtype
 * 	Same for values. Only used in QM_MIRROR mode.
 *
 * @param mask
 * 	Must be 2^n - 1. (mask + 1) is the table size.
 *
 * @param flags
 * 	0, QM_AINDEX, QM_MIRROR, or bitwise OR of both.
 *
 * @returns
 * 	The map's handle for later reference.
 */
unsigned qmap_open(unsigned ktype, unsigned vtype,
		unsigned mask, unsigned flags);

/* Close a qmap
 *
 * @param hd	The handle
 */
void qmap_close(unsigned hd);

/* Get a table position from a key
 *
 * @param hd	The handle.
 * @param key	The key.
 *
 * @returns	Position n, or QM_MISS if not found.
 */
unsigned qmap_get(unsigned hd, const void * const key);

/* Put a pair into the table.
 *
 * @param hd
 * 	The handle
 *
 * @param key
 *  	The key to put. May be NULL when opened
 *  	with QM_AINDEX.
 *
 * @param value
 * 	Only needed for associations (to generate
 * 	secondary keys). May be NULL otherwise.
 *
 * @returns
 * 	The position your value should be stored at.
 */
unsigned qmap_put(unsigned hd,
		const void * const key,
		const void * const value);

/* Delete an item by key.
 *
 * @param hd	The handle.
 * @param key	The key to delete.
 */
void qmap_del(unsigned hd, const void * const key);

/* Drop all of them contents.
 *
 * @param hd
 * 	The handle.
 */
void qmap_drop(unsigned hd);

/* Association callback type
 *
 * @param skey
 * 	You want to set this to some pointer
 * 	that will then represent the secondary key.
 * 	
 * @param pkey
 * 	Key of the primary table.
 *
 * @param value
 * 	Value of the primary table.
 *
 * Semantics: after association, future puts/dels on
 * the primary will update the secondary accordingly.
 */
typedef void qmap_assoc_t(
		const void **skey,
		const void * const pkey,
		const void * const value);

/* Make an association between two tables
 *
 * @param hd
 * 	Handle of the secondary map (index).
 *
 * @param link
 * 	Handle of the primary map (source).
 *
 * @param cb
 * 	Callback to generate secondary keys. If NULL,
 * 	*skey will default to the primary value pointer.
 */
void qmap_assoc(unsigned hd,
		unsigned link, qmap_assoc_t cb);

/* Start iteration.
 *
 * @param key
 *  	NULL to start at the beginning; or a key to seek-start.
 *
 * @returns
 * 	A cursor handle.
 */
unsigned qmap_iter(unsigned hd, const void * const key);

/* Do iteration.
 *
 * @param n
 * 	The position to the item on the table will
 * 	be stored here.
 *
 * @param cur_id
 * 	The cursor handle.
 *
 * @returns
 * 	1 if an item was produced; 0 if no more items.
 */
int qmap_next(unsigned *n, unsigned cur_id);

/* Exit iteration early (optional).
 * Prevents cursor handle growth; otherwise no side effects.
 *
 * @param cur_id
 * 	Cursor handle.
 */
void qmap_fin(unsigned cur_id);

/* Return the key that corresponds to a certain id
 *
 * @param hd
 * 	Map handle.
 *
 * @param id
 * 	The item's id.
 *
 * @returns The item's registered key pointer.
 */
const void *qmap_key(unsigned hd, unsigned id);

/* Hash callback type
 *
 * @param key
 * 	The key to hash.
 * 	
 * @returns
 * 	An unsigned number hash of the key.
 */
typedef unsigned qmap_hash_t(const void * const key);

/* Compare callback type
 *
 * @param a
 * 	The first argument of the comparison.
 *
 * @param b
 * 	The second argument of the comparison.
 * 	
 * @returns
 * 	Zero if there is no difference.
 * 	Not zero if there is one.
 */
typedef int qmap_cmp_t(const void * const a,
		const void * const b);

/* Customize a map's hash and compare function.
 *	This can be useful if you're interested in
 *	the contents instead of the pointers.
 *
 * @param hd
 * 	The handle of the map to customize.
 *
 * @param hash
 * 	A hash function to use for keys.
 *
 * @param cmp
 * 	A compare function to check key equality.
 */
void qmap_custom(unsigned hd, qmap_hash_t *hash,
		qmap_cmp_t *cmp);

#endif
