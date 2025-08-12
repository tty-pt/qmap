#include "./include/qmap.h"
#include "./include/qidm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <xxhash.h>

/* MACROS, STRUCTS, ENUMS AND GLOBALS {{{ */

#define new(a, n, t)    (t *)alloc(a, n, sizeof(t), _Alignof(t))
#define QMAP_SEED 13
#define QMAP_DEFAULT_MASK 0x7FFF

#define DEBUG_LVL 0
/* #define FEAT_DUP_PRIMARY */
/* #define FEAT_REALLOC */

#define DEBUG(lvl, fmt, ...) \
	if (DEBUG_LVL > lvl) \
	fprintf(stderr, fmt, __VA_ARGS__)

#define UNUSED __attribute__((unused))

typedef unsigned qmap_hash_t(void *key, size_t key_len);

typedef struct {
	// these have to do with keys
	unsigned *map,  // id -> n
		 *omap; // n -> id

	char *vmaps[2];
	qmap_type_t *type[2];
	unsigned m, mask, flags;
	size_t lens[2];
	qmap_hash_t *hash;
	idm_t idm;

	unsigned phd;
	unsigned tophd;
	qmap_assoc_t *assoc;
} qmap_t;

typedef struct {
	unsigned hd, position, sub_cur;
	void *key;
} qmap_cur_t;

qmap_t qmaps[QMAP_MAX];
qmap_cur_t qmap_cursors[QMAP_MAX];
idm_t idm, cursor_idm;
unsigned assoc_hd;

static qmap_type_t type_unsigned = {
	.len = sizeof(unsigned),
};

/* }}} */

/* BUILT-INS {{{ */

/* HASH FUNCTIONS */

static unsigned qmap_hash(void *key, size_t key_len) {
	return XXH32(key, key_len, QMAP_SEED);
}

static unsigned qmap_nohash(void *key, size_t key_len UNUSED) {
	return * (unsigned *) key;
}

/* ASSOCIATION CALLBACKS */

int
qmap_assoc_rhd(void **skey, void *key UNUSED, void *value) {
	*skey = value;
	return 0;
}

int
qmap_twin_assoc(void **skey, void *key, void *value UNUSED) {
	*skey = key;
	return 0;
}

/* }}} */

/* HELPER FUNCTIONS {{{ */

/* this gets the lowest cursor counting from cur_id,
 * meaning, iteration starts on a high-level cursor on
 * some qmap, but then it trickles down to whatever might
 * be on sub-qmaps. This returns the lowest level one.
 * It's useful for methods that operate on the cursor itself,
 * like cget and cdel.
 * */
static inline unsigned
qmap_low_cur(unsigned cur_id) {
	register qmap_cur_t *cursor = &qmap_cursors[cur_id];

	if (!cursor->sub_cur)
		return cur_id;
	else
		return qmap_low_cur(cursor->sub_cur);
}

/* In some cases we want to calculate the id based on
 * the qmap's hash function and the key, and the mask.
 * Other times it's not useful to do that.
 * This is for when it is.
 * */
static inline unsigned qmap_id(unsigned hd, void *key) {
	qmap_t *qmap = &qmaps[hd];
	return qmap->hash(key, qmap_len(hd, key, QMAP_KEY)) & qmap->mask;
}

/* This obtains something which might be a direct value.
 * But it might also be a pointer, in case of unknown length
 * values, or a qmap handle (unsigned), if the qmap is has
 * the DUP flag, for example. 
 */
static inline
void *qmap_val(qmap_t *qmap, enum qmap_member t, unsigned id) {
	return qmap->vmaps[t] + id * qmap->lens[t];
}

/* This, on the other hand, will return the pointer to the
 * value itself, even if it has variable length. However
 * for DUP qmaps it will still return the pointer to the
 * handle.
 */
static inline
void *qmap_rval(unsigned hd, enum qmap_member t, unsigned id) {
	qmap_t *qmap = &qmaps[hd];
	void *value = qmap_val(qmap, t, id);

	if (((qmap->flags & QMAP_DUP) && t == QMAP_VALUE)
			|| qmap->type[t]->len)
		return value;

	return * (void **) value;
}

/* This is the exact same as qmap_rval except that it's
 * meant to be used with pointers.
 */
static inline void *
qmap_cval(qmap_cur_t *cursor, enum qmap_member t) {
	register qmap_t *qmap = &qmaps[cursor->hd];
	unsigned n = cursor->position - 1,
		 id = qmap->omap[n];

	return qmap_rval(cursor->hd, t, id);
}

/* This compares the thing being pointed to by a cursor
 * to the pointer the user provides.
 */
static inline int
qmap_ccmp(unsigned cur_id, enum qmap_member t, void *cmp)
{
	register qmap_cur_t *cursor
		= &qmap_cursors[qmap_low_cur(cur_id)];
	void *value = qmap_cval(cursor, t);
	size_t len = qmap_len(cursor->hd, value, t);
	return memcmp(value, cmp, len);
}

/* This will get you the true length of a value */
size_t qmap_len(unsigned hd, void *value, enum qmap_member member) {
	qmap_t *qmap = &qmaps[hd];
	qmap_type_t *type = qmap->type[member];

	if (type->len)
		return type->len;
#ifndef FEAT_DUP_PRIMARY
	return type->measure(value);
#else
	else if (type->measure)
		return type->measure(value);
	else {
		size_t key_len = _qmap_len(type->part[0], value);
		size_t value_len = _qmap_len(type->part[1], (char *) value + key_len);
		return key_len + value_len;
	}
#endif
}

/* }}} */

/* OPEN / INITIALIZATION {{{ */

/* You must call this to initialize the system */
void qmap_init(void) {
	idm = idm_init();
	cursor_idm = idm_init();
	assoc_hd = qmap_open(
			&type_unsigned,
			&type_unsigned,
			0, QMAP_DUP);
}

/* This is used by qmap_open to open databases. */
static unsigned
_qmap_open(qmap_type_t *key_type,
		qmap_type_t *value_type,
		unsigned mask,
		unsigned flags)
{
	unsigned hd = idm_new(&idm);
	qmap_t *qmap = &qmaps[hd];
	unsigned len;
	size_t ids_len, keys_len, values_len;

	DEBUG(3, "_open %u %p %p %u %u\n",
			hd, (void *) key_type,
			(void *) value_type, mask, flags);

	qmap->lens[QMAP_KEY] = key_type->len ? key_type->len : sizeof(char *);
	qmap->lens[QMAP_VALUE] = (flags & QMAP_DUP)
		?  sizeof(unsigned)
		: (value_type->len
				?  value_type->len
				: sizeof(char *));
	
	qmap->hash = qmap_hash;
	if (key_type->len == sizeof(unsigned))
		qmap->hash = qmap_nohash;

	mask = mask ? mask : QMAP_DEFAULT_MASK;
	len = ((unsigned) -1) & mask;

	ids_len = len * sizeof(unsigned);
	keys_len = len * qmap->lens[QMAP_KEY];
	values_len = len * qmap->lens[QMAP_VALUE];

	qmap->map = malloc(ids_len);
	qmap->omap = malloc(ids_len);
	qmap->vmaps[QMAP_KEY] = malloc(keys_len);
	qmap->vmaps[QMAP_VALUE] = malloc(values_len);
	qmap->m = len;
	qmap->type[QMAP_KEY] = key_type;
	qmap->type[QMAP_VALUE] = value_type;
	qmap->mask = mask;
	qmap->flags = flags;
	qmap->idm = idm_init();

	memset(qmap->map, 0, ids_len);
	memset(qmap->omap, 0, ids_len);
	memset(qmap->vmaps[QMAP_KEY], 0, keys_len);
	memset(qmap->vmaps[QMAP_VALUE], 0, values_len);

	return hd;
}

/* This opens databases */
unsigned
qmap_open(qmap_type_t *key_type, qmap_type_t *value_type, unsigned mask, unsigned flags)
{
	qmap_type_t *backup_key_type = key_type;
	int prim_dup = 0;

	unsigned phd = _qmap_open(key_type, value_type, mask, flags);

	if (!(flags & QMAP_TWO_WAY))
		return phd;

#ifdef FEAT_DUP_PRIMARY
	// we need a special type to account
	// for both key and value in this case
	// in case we want n <-> n
	if (flags & QMAP_DUP) {
		// TODO make sure we free this
		key_type = malloc(sizeof(qmap_type_t));
		key_type->part[0] = backup_key_type;
		key_type->part[1] = value_type;
		key_type->len = 0;
		key_type->measure = NULL;
		prim_dup = 1;
	}
#endif

	flags &= ~(QMAP_TWO_WAY | QMAP_AINDEX);
	flags |= QMAP_DUP;

	key_type = backup_key_type;

	_qmap_open(value_type, key_type, mask, flags | QMAP_PGET);
	qmap_assoc(phd + 1, phd, qmap_assoc_rhd);

	if (prim_dup) {
		_qmap_open(key_type, value_type, mask, flags);
		qmap_assoc(phd + 2, phd, NULL);
	}

	return phd;
}

/* }}} */

/* PUT {{{ */

/* This is a very low-level construct to put values in their place */
static inline void
qmap_mPUT(unsigned hd, enum qmap_member t, void *value, unsigned id)
{
	qmap_t *qmap = &qmaps[hd];
	void *real_value = value, *tmp = value;
	size_t len = qmap->lens[t];

	if (!((qmap->flags & QMAP_DUP) && t == QMAP_VALUE)
			&& !qmap->type[t]->len) {
		size_t real_len = qmap_len(hd, value, t);
		tmp = malloc(real_len);
		memcpy(tmp, value, real_len);
		real_value = &tmp;
	}

	memcpy(qmap_val(qmap, t, id), real_value, len);
}

/* This is similar except that it already puts value and key,
 * and gets an 'n' and an 'id' to insert
 */
static inline
unsigned qmap_PUT(unsigned hd, void *key, void *value) {
	qmap_t *qmap = &qmaps[hd];
	unsigned n = idm_new(&qmap->idm);
	unsigned id;

	if (key)
		id = qmap_id(hd, key);
	else {
		id = n;
		key = &n;
	}

#ifdef FEAT_REALLOC
	if (qmap->m <= id) {
		qmap->m = id;
		qmap->m *= 2; // TODO careful here - id can be big
		qmap->map = realloc(qmap->map, qmap->m * sizeof(unsigned));
		qmap->omap = realloc(qmap->omap, qmap->m * sizeof(unsigned));
		qmap->vmaps[QMAP_KEY] = realloc(qmap->vmaps[QMAP_KEY], qmap->m * qmap->lens[QMAP_KEY]);
		qmap->vmaps[QMAP_VALUE] = realloc(qmap->vmaps[QMAP_VALUE], qmap->m * qmap->lens[QMAP_VALUE]);
	}
#endif

	qmap_mPUT(hd, QMAP_KEY, key, id);
	qmap_mPUT(hd, QMAP_VALUE, value, id);

	qmap->omap[n] = id;
	qmap->map[id] = n;
	DEBUG(4, "%u's _put %u %u\n", hd, id, n);

	return id;
}

/* this is a highter-level put but for internal use only.
 * It's designed to know how to put in DUP databases.
 */
static unsigned
_qmap_put(unsigned hd, void *key, void *value) {
	qmap_t *qmap = &qmaps[hd], *iqmap;
	unsigned in_hd, id, n;

	if (!(qmap->flags & QMAP_DUP))
		return qmap_PUT(hd, key, value);

	id = qmap_id(hd, key);
	n = qmap->map[id];

	if (n >= qmap->idm.last || qmap->omap[n] != id)
	{
		in_hd = qmap_open(
				qmap->type[QMAP_KEY],
				qmap->type[QMAP_VALUE],
				0, qmap->flags & QMAP_PGET);

		iqmap = &qmaps[in_hd];
		iqmap->tophd = hd;

		if (qmap->assoc) {
			iqmap->assoc = qmap->assoc;
			iqmap->phd = qmap->phd;
		}

		qmap_PUT(hd, key, &in_hd);
	} else
		in_hd = * (unsigned *)
			qmap_rval(hd, QMAP_VALUE, id);

	return qmap_PUT(in_hd, key, value);
}

/* this is the user's API. It tries to have everything
 * into consideration.
 */
unsigned
qmap_put(unsigned hd, void *key, void *value)
{
#ifdef FEAT_DUP_PRIMARY
	qmap_t *qmap = &qmaps[hd];
	size_t key_len, value_len;
	unsigned flags = qmap->flags;
#endif
	unsigned ret, cur, linked_hd;

#ifdef FEAT_DUP_PRIMARY
	if (key != NULL) {
		key_len = qmap_len(hd, key, QMAP_KEY);

		if ((flags & QMAP_TWO_WAY) && (flags & QMAP_DUP)) {
			key_len = qmap_len(hd + 2, key, QMAP_KEY);
			value_len = qmap_len(hd + 2, value, QMAP_VALUE);
			char buf[key_len + value_len];
			memcpy(buf, key, key_len);
			memcpy(buf + key_len, value, value_len);
			ret = _qmap_put(hd, buf, value);
			goto proceed;
		}

	}
#endif

	ret = _qmap_put(hd, key, value);
	if (!key)
		key = &ret;

#ifdef FEAT_DUP_PRIMARY
proceed:
#endif

	cur = qmap_iter(assoc_hd, &hd);
	while (qmap_lnext(cur)) {
		qmap_t *aqmap;
		void *skey;

		qmap_cget(&linked_hd, cur, QMAP_VALUE);
		aqmap = &qmaps[linked_hd];
		aqmap->assoc(&skey, key, value);

		_qmap_put(linked_hd, skey, value);
	}

	return ret;
}

/* }}} */

/* GET {{{ */

/* Use this to get a primary key from a secondary qmap */
static inline
int qmap_pget(unsigned hd, void *target, void *key) {
	qmap_t *qmap = &qmaps[hd], *pqmap;
	unsigned id = qmap_id(hd, key), n, pid;
	void *value;
	size_t len;

	if (id >= qmap->m)
		return 1;

	n = qmap->map[id];
	DEBUG(4, "%u's pget %u %u\n", hd, id, n);

	if (n > qmap->idm.last)
		return 1;

	pqmap = &qmaps[qmap->phd];
	pid = pqmap->omap[n];

	if (pqmap->map[pid] != n)
		return 1;

	value = qmap_rval(qmap->phd, QMAP_KEY, pid);
	len = qmap_len(qmap->phd, value, QMAP_KEY);
	memcpy(target, value, len);
	return 0;
}

int qmap_get(unsigned hd, void *value, void *key)
{
	unsigned cur_id = qmap_iter(hd, key);;

	if (!qmap_lnext(cur_id))
		return 1;

	qmap_cget(value, cur_id, QMAP_VALUE);
	qmap_fin(cur_id);
	return 0;
}

static inline
void *qmap_csget(qmap_cur_t *cursor, enum qmap_member t)
{
	qmap_t *qmap = &qmaps[cursor->hd];

	if ((qmap->flags & QMAP_PGET) && t == QMAP_VALUE)
	{
		void *key = qmap_csget(cursor, QMAP_KEY);
		unsigned id = qmap_id(cursor->hd, key), n, pid;
		qmap_t *pqmap, *tqmap;

		tqmap = &qmaps[cursor->hd];
		n = qmaps[tqmap->tophd].map[id];

		pqmap = &qmaps[qmap->phd];
		pid = pqmap->omap[n];

		return qmap_rval(qmap->phd, QMAP_KEY, pid);
	} else 
		return qmap_cval(cursor, t);
}

void qmap_cget(void *target, unsigned cur_id, enum qmap_member t)
{
	register qmap_cur_t *cursor
		= &qmap_cursors[qmap_low_cur(cur_id)];
	void *value;
	size_t len;

	value = qmap_csget(cursor, t);
	len = qmap_len(cursor->hd, value, t);
	memcpy(target, value, len);
}

/* }}} */

/* DELETE {{{ */

void qmap_cdel(unsigned cur_id)
{
	cur_id = qmap_low_cur(cur_id);
	register qmap_cur_t *cursor = &qmap_cursors[cur_id];
	register qmap_t *qmap = &qmaps[cursor->hd];
	unsigned n, id;
	n = cursor->position - 1;
	id = qmap->omap[n];

	if (!(qmap->type[QMAP_KEY]->len)) {
		void *key = qmap_val(qmap, QMAP_KEY, id);
		free(* (void **) key);
	}

	void *value = qmap_val(qmap, QMAP_VALUE, id);

	if (qmap->flags & QMAP_DUP)
		qmap_drop(* (unsigned *) value);
	else if (!(qmap->type[QMAP_VALUE]->len))
		free(* (void **) value);

	idm_del(&qmap->idm, n);
	qmap->map[id] = QMAP_MISS;
	qmap->omap[n] = QMAP_MISS;
}

static inline // should always happen for secondaries
void qmap_pdel(unsigned hd, void *key)
{
	qmap_t *qmap = &qmaps[hd];
	unsigned n, id = qmap_id(hd, key);
	qmap_t *pqmap = &qmaps[qmap->phd];
	void *real_key, *real_value;

	if (qmap->m < id)
		return; // not present

	n = qmap->map[id];
	id = pqmap->omap[n];

	real_key = qmap_rval(qmap->phd, QMAP_KEY, id);
	real_value = qmap_rval(qmap->phd, QMAP_VALUE, id);

	qmap_del(qmap->phd, real_key, real_value);
	idm_del(&qmap->idm, n);
}

void qmap_del(unsigned hd, void *key, void *value)
{
	qmap_t *qmap = &qmaps[hd];
	unsigned cur;

	if (qmap->assoc) {
		qmap_pdel(hd, key);
		return;
	}

	cur = qmap_iter(hd, key);
	while (qmap_lnext(cur)) {
		if (value && qmap_ccmp(cur, QMAP_VALUE, value))
			continue;
		qmap_cdel(cur);
		qmap_fin(cur);
		return;
	}
}

/* }}} */

/* ITERATION {{{ */
void qmap_fin(unsigned cur_id) {
	qmap_cur_t *cursor = &qmap_cursors[cur_id];
	if (cursor->sub_cur)
		qmap_fin(cursor->sub_cur);
	idm_del(&cursor_idm, cur_id);
}

unsigned qmap_iter(unsigned hd, void *key) {
	qmap_t *qmap = &qmaps[hd];
	unsigned cur_id = idm_new(&cursor_idm);
	qmap_cur_t *cursor = &qmap_cursors[cur_id];
	unsigned id;

	if (key) {
		id = qmap_id(hd, key);
		cursor->position = id > qmap->m
			? qmap->m
			: qmap->map[id];
	} else
		cursor->position = 0;

	cursor->sub_cur = 0;
	cursor->hd = hd;
	cursor->key = key;
	return cur_id;
}

int qmap_lnext(unsigned cur_id)
{
	register qmap_cur_t *cursor = &qmap_cursors[cur_id];
	register qmap_t *qmap = &qmaps[cursor->hd];
	unsigned n, id;
	char *key;
	size_t key_len;
cagain:
	n = cursor->position;

	if (n >= qmap->idm.last)
		goto end;

	id = qmap->omap[n];
	if (id == QMAP_MISS) {
		cursor->position++;
		goto cagain;
	}

	if (qmap->flags & QMAP_DUP) {
		unsigned in_hd = * (unsigned *)
			qmap_rval(cursor->hd, QMAP_VALUE, id);

		DEBUG(4, "next_dup %u\n", in_hd);

		if (!cursor->sub_cur)
			cursor->sub_cur = qmap_iter(in_hd, cursor->key);

		if (qmap_lnext(cursor->sub_cur))
			return 1;

		cursor->position++;
		cursor->sub_cur = 0;
		goto cagain;
	}

	key = qmap_rval(cursor->hd, QMAP_KEY, id);
	key_len = qmap_len(cursor->hd, key, QMAP_KEY);

	if (cursor->key && memcmp(key, cursor->key, key_len))
		goto end;

	cursor->position++;
	return 1;
end:
	idm_del(&cursor_idm, cur_id);
	return 0;
}

/* }}} */

/* DROP + CLOSE + OTHERS {{{ */
void qmap_drop(unsigned hd) {
	unsigned cur_id = qmap_iter(hd, NULL);

	while (qmap_lnext(cur_id))
		qmap_cdel(cur_id);
}

void
qmap_close(unsigned hd) {
	qmap_t *qmap = &qmaps[hd];

	if (qmap->flags & QMAP_TWO_WAY) {
		qmap_close(hd + 1);
#ifdef FEAT_DUP_PRIMARY
		if (qmap->flags & QMAP_DUP) {
			qmap_close(hd + 2);
			free(qmap->type[QMAP_KEY]);
		}
#endif
	}

	qmap_drop(hd);
	qmap_del(assoc_hd, &hd, NULL);
	ids_drop(&qmap->idm.free);
	qmap->idm.last = 0;
	free(qmap->map);
	free(qmap->omap);
	free(qmap->vmaps[QMAP_KEY]);
	free(qmap->vmaps[QMAP_VALUE]);
	idm_del(&idm, hd);
}

void
qmap_assoc(unsigned hd, unsigned link, qmap_assoc_t cb)
{
	qmap_t *qmap = &qmaps[hd];

	if (!cb)
		cb = qmap_twin_assoc;

	_qmap_put(assoc_hd, &link, &hd);

	qmap->assoc = cb;
	qmap->phd = link;
}

void qmap_print(char *target, unsigned hd,
		unsigned type, void *thing)
{
	qmap_t *qmap = &qmaps[hd];
	if (qmap->flags & QMAP_TWO_WAY)
		hd += 1;
	qmap->type[type]->print(target, thing);
}

/* }}} */
