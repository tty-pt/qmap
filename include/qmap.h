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

	// QM_PGET: default to obtaining primary keys
	// instead of values.
	QM_PGET = 4,
};

// built-in types
enum qmap_tbi {
	// A pointer, which gets hashed
	QM_PTR = 0,

	// Use the value as an opaque handle (no hashing)
	QM_HNDL = 1,

	// String contents hash and compare
	QM_STR = 2,
};

// iter flags
enum qmap_if {
	// Do a ranged iteration. Meaning. It will
	// continue even if the key differs from the initial.
	QM_RANGE = 1,
};

/* Open a database
 *
 * @param ktype
 * 	A built-in or registered type for keys.
 *
 * @param vtype
 * 	A built-in or registered type for values.
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
 * @returns	A pointer to the value or NULL if not found.
 */
const void *qmap_get(unsigned hd, const void * const key);

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
unsigned qmap_iter(unsigned hd, const void * const key, unsigned flags);

/* Do iteration.
 *
 * @param key
 * 	The address of a pointer to return the key to the user.
 *
 * @param value
 * 	The address of a pointer to return the value to the user.
 *
 * @param cur_id
 *	The cursor handle.
 *
 * @returns
 * 	1 if an item was produced; 0 if no more items.
 */
int qmap_next(const void **key, const void **value,
		unsigned cur_id);

/* Exit iteration early (optional).
 * Prevents cursor handle growth; otherwise no side effects.
 *
 * @param cur_id
 * 	Cursor handle.
 */
void qmap_fin(unsigned cur_id);

/* Measure callback type, to measure a key that
 * is of variable or dynamic size.
 *
 * @param data
 * 	The pointer to the key you want to measure.
 *
 * @returns
 * 	The size of the contents.
 * 	
 * Semantics: Keys that are not of fixed size need a
 * way to be measured, in case we are not using just
 * the pointers for comparison and hashing.
 */
typedef size_t qmap_measure_t(const void *data);

/* Register a type of fixed length for hashing and
 * comparing contents.
 *
 * @param len
 * 	The length of the type.
 *
 * @returns
 * 	The type's id.
 */
unsigned qmap_reg(size_t len);

typedef int qmap_cmp_t(
		const void * const a,
		const void * const b,
		size_t len);

void
qmap_cmp_set(unsigned ref, qmap_cmp_t *cmp);

/* Register a type of variable length for hashing
 * and comparing contents.
 *
 * @param measure
 * 	The callback used to determine the size.
 *
 * @returns
 * 	The type's id.
 */
unsigned qmap_mreg(qmap_measure_t *measure);

/* Return the length of a certain element in memory.
 *
 * @param type_id
 * 	The type of element.
 *
 * @param data
 * 	The element's data.
 *
 * @returns
 * 	The length of it.
 */
size_t qmap_len(unsigned type_id, const void *data);

#endif
