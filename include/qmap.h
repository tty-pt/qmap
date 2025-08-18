#ifndef QMAP_H
#define QMAP_H

#include <stddef.h>
#include <stdlib.h>

#define QMAP_MAX 1024
#define QMAP_MAX_COMBINED_LEN (BUFSIZ * 2)
#define QMAP_MISS ((unsigned) -1)

enum qmap_flags {
	// AINDEX means you'll get increasing
	// numbers when you put with NULL key
	QMAP_AINDEX = 1,

	// This means you'll get a mirror map
	// which you can use to do reverse lookup
	QMAP_MIRROR = 2,
};

// built-in types
enum qmap_tbi {
	// Use value as if it were a handle.
	QMAP_HNDL = 1,

	// Hash the pointer!
	QMAP_HASH = 0,
};

/* Initialize the system */
void qmap_init(void);

/* Open a database
 *
 * @param ktype
 * 	This is the key type. Either:
 * 	QMAP_NOHASH (0) or QMAP_HASH (1).
 *
 * @param vtype
 * 	Likewise, but for value. However it's only
 * 	used in	MIRROR mode,
 *
 * @param mask
 * 	Must be 2^n - 1. Mask + 1 should be
 * 	your table size.
 *
 * @param flags
 * 	Might be 0, QMAP_AINDEX or QMAP_MIRROR,
 * 	or a bitwise or of those.
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
 * @returns	The position.
 */
unsigned qmap_get(unsigned hd, const void * const key);

/* Put a pair into the table.
 *
 * @param hd
 * 	The handle
 *
 * @param key
 * 	The key you want to put. Might be NULL
 * 	if the qmap was open with the AINDEX flag.
 *
 * @param value
 * 	The value is only needed for associations,
 * 	in case you want to use it to generate
 * 	secondary keys. You can also use NULL.
 *
 * @returns
 * 	The position your value should be stored at.
 */
unsigned qmap_put(unsigned hd,
		const void * const key,
		const void * const value);

/* Delete an item key key.
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
 * 	Is the key of the primary table.
 *
 * @param value
 * 	Is the value of the primary table.
 */
typedef void qmap_assoc_t(
		const void **skey,
		const void * const pkey,
		const void * const value);

/* Make an association between two tables
 *
 * @param hd
 * 	Is the handle of the secondary one.
 *
 * @param link
 * 	Is the handle of the primary one.
 *
 * @param cb
 * 	Is used to know how the secondary keys should
 * 	be generated. If you provide NULL, we'll set
 * 	skey to the value pointer.
 */
void qmap_assoc(unsigned hd,
		unsigned link, qmap_assoc_t cb);

/* Start iteration.
 *
 * @param key
 * 	Might be NULL or a key.
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
 * 	Was there an item there?
 */
int qmap_next(unsigned *n, unsigned cur_id);

/* Exist iteration early.
 *	This only makes sure that cursor handles.
 *	Don't keep increasing. Nothing special.
 */
void qmap_fin(unsigned cur_id);

#endif
