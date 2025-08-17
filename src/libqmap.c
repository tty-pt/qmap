/* libqmap.c
 * Licence: BSD-2-Clause
 *
 * I'm adding some comments to make it easier to understand,
 * but whatever's user API is documented in the header file.
 */
#include "./../include/qmap.h"
#include "./../include/qidm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <xxhash.h>
#include <qsys.h>

/* MACROS, STRUCTS, ENUMS AND GLOBALS {{{ */

#define QMAP_SEED 13
#define QMAP_DEFAULT_MASK 0x7FFF

#define DEBUG_LVL 0
/* #define FEAT_DUP_TWO_WAY */
/* #define FEAT_REHASH */

#define DEBUG(lvl, ...) \
	if (DEBUG_LVL > lvl) WARN(__VA_ARGS__)

typedef unsigned qmap_hash_t(
		const void * const key, size_t key_len);

typedef struct {
	// these have to do with keys
	unsigned *map,  // id -> n
		 *omap; // n -> id

	char *vmaps[2];
	const qmap_type_t *type[2];
	const char * type_str[2];
	unsigned m, mask, flags;
	size_t lens[2];
	qmap_hash_t *hash;
	idm_t idm;

	unsigned phd;
	unsigned tophd, topn;
	qmap_assoc_t *assoc;
} qmap_t;

typedef struct {
	unsigned hd, position, sub_cur, id;
	const void * key;
} qmap_cur_t;

qmap_t qmaps[QMAP_MAX];
qmap_cur_t qmap_cursors[QMAP_MAX];
idm_t idm, cursor_idm;
unsigned assoc_hd, types_hd;

int
u_print(char * const target, const void * const value)
{
	return sprintf(target, "%u", * (unsigned *) value);
}

int
s_print(char * const target, const void * const value)
{
	return sprintf(target, "%s", (char *) value);
}

size_t
s_measure(const void * const value)
{
	return value ? strlen(value) + 1 : 0;
}

int s_compare(const void * const a,
		const void * const b,
		size_t _len UNUSED)
{
	return strcmp(a, b);
}

static qmap_type_t type_string = {
	.print = s_print,
	.measure = s_measure,
	.compare = s_compare,
}, type_unsigned = {
	.print = u_print,
	.len = sizeof(unsigned),
}, type_ptr = {
	.len = sizeof(void *),
};

/* }}} */

/* BUILT-INS {{{ */

/* HASH FUNCTIONS */

static unsigned
qmap_hash(const void * const key, size_t key_len)
{
	return XXH32(key, key_len, QMAP_SEED);
}

static unsigned
qmap_nohash(const void * const key, size_t key_len UNUSED)
{
	return * (unsigned *) key;
}

/* ASSOCIATION CALLBACKS */

int
qmap_assoc_rhd(const void ** const skey,
		const void * const key UNUSED,
		const void * const value)
{
	*skey = value;
	return 0;
}

int
qmap_twin_assoc(const void ** const skey,
		const void * const key,
		const void * const value UNUSED)
{
	*skey = key;
	return 0;
}

/* OTHERS */
int
other_compare(const void * const a,
		const void * const b, size_t len)
{
	return memcmp(a, b, len);
}


/* }}} */

/* HELPER FUNCTIONS {{{ */

/* this gets the lowest cursor counting from cur_id, meaning,
 * iteration starts on a high-level cursor on some qmap, but
 * then it trickles down to whatever might be on sub-qmaps.
 * This returns the lowest level one. It's useful for methods
 * that operate on the cursor itself, like cget and cdel.
 */
static inline unsigned
qmap_low_cur(unsigned cur_id)
{
	register qmap_cur_t *cursor
		= &qmap_cursors[cur_id];

	if (!cursor->sub_cur)
		return cur_id;
	else
		return qmap_low_cur(cursor->sub_cur);
}

/* In some cases we want to calculate the id based on the
 * qmap's hash function and the key, and the mask. Other
 * times it's not useful to do that. This is for when it is.
 */
static inline unsigned
qmap_id(unsigned hd, const void * const key)
{
	qmap_t *qmap = &qmaps[hd];
	size_t len = qmap_len(hd, key, QMAP_KEY);
	return qmap->hash(key, len) & qmap->mask;
}

/* This obtains something which might be a direct value. But
 * it might also be a pointer, in case of unknown length
 * values, or a qmap handle (unsigned), if the qmap is has
 * the DUP flag, for example.
 */
static inline void
*qmap_val(qmap_t *qmap, enum qmap_mbr t, unsigned n)
{
	return qmap->vmaps[t] + n * qmap->lens[t];
}

/* This, on the other hand, will return the pointer to the
 * value itself, even if it has variable length. However for
 * DUP qmaps it will still return the pointer to the handle.
 */
static inline void
*qmap_rval(unsigned hd, enum qmap_mbr t, unsigned n)
{
	qmap_t *qmap = &qmaps[hd];
	void *value = qmap_val(qmap, t, n);
	int dup_value = (qmap->flags & QMAP_DUP)
		&& t == QMAP_VALUE;

	if (dup_value || qmap->type[t]->len)
		return value;

	return * (void **) value;
}

/* This compares the thing in the map to the pointer the user
 * provides.
 */
static inline int
_qmap_cmp(unsigned hd, enum qmap_mbr t, unsigned n,
		const void * const cmp)
{
	void *value = qmap_rval(hd, t, n);
	size_t clen = qmap_len(hd, cmp, t);
	size_t vlen = qmap_len(hd, value, t);

	if (clen != vlen)
		return 1;

	return memcmp(value, cmp, vlen);
}

/* Same as qmap_cmp but for cursors. */
static inline int
qmap_ccmp(unsigned cur_id, enum qmap_mbr t,
		const void * const cmp)
{
	register qmap_cur_t *cursor = &qmap_cursors[cur_id];
	unsigned hd = cursor->hd;
	unsigned n = cursor->position - 1;

	if (t == QMAP_VALUE && qmaps[hd].flags & QMAP_PGET)
	{
		t = QMAP_KEY;
		hd = qmaps[hd].phd;
	}

	return _qmap_cmp(hd, t, n, cmp);
}

size_t /* API */
qmap_len(unsigned hd, const void * const value,
		enum qmap_mbr t)
{
	qmap_t *qmap = &qmaps[hd];
	const qmap_type_t *type = qmap->type[t];

	if (type->len)
		return type->len;
#ifndef FEAT_DUP_TWO_WAY
	return type->measure(value);
#else
	else if (type->measure)
		return type->measure(value);

	size_t key_len = _qmap_len(
			type->part[0], value);

	size_t value_len = _qmap_len(type->part[1],
			(char *) value + key_len);

	return key_len + value_len;
#endif
}

/* }}} */

/* OPEN / INITIALIZATION {{{ */

void /* API */
qmap_regc(char *key, qmap_type_t *type) {
	qmap_put(types_hd, key, &type);
}

/* This is used by qmap_open to open databases. */
static unsigned
_qmap_open(const qmap_type_t * const key_type,
		const qmap_type_t * const value_type,
		unsigned mask,
		unsigned flags)
{
	unsigned hd = idm_new(&idm);
	qmap_t *qmap = &qmaps[hd];
	unsigned len;
	size_t ids_len, keys_len, values_len;
	
	DEBUG(1, "_open %u %p %p %u %u %zu %zu\n",
			hd, (void *) key_type,
			(void *) value_type,
			mask, flags, key_type->len, value_type->len);

	qmap->lens[QMAP_KEY] = key_type->len
		? key_type->len
		: sizeof(char *);

	qmap->lens[QMAP_VALUE] = (flags & QMAP_DUP)
		?  sizeof(unsigned)
		: (value_type->len
				?  value_type->len
				: sizeof(char *));
	
	qmap->hash = qmap_hash;
	if (key_type->len == sizeof(unsigned))
		qmap->hash = qmap_nohash;

	mask = mask ? mask : QMAP_DEFAULT_MASK;
	len = (((unsigned) -1) & mask) + 1;

	ids_len = len * sizeof(unsigned);
	keys_len = len * qmap->lens[QMAP_KEY];
	values_len = len * qmap->lens[QMAP_VALUE];

	qmap->map = malloc(ids_len);
	qmap->omap = malloc(ids_len);
	qmap->vmaps[QMAP_KEY] = malloc(keys_len);
	qmap->vmaps[QMAP_VALUE] = malloc(values_len);
	CBUG(!(qmap->map && qmap->omap
				&& qmap->vmaps[QMAP_KEY]
				&& qmap->vmaps[QMAP_VALUE]),
			"alloc error\n");
	qmap->m = len;
	qmap->type[QMAP_KEY] = key_type;
	qmap->type[QMAP_VALUE] = value_type;
	qmap->mask = mask;
	qmap->flags = flags;
	qmap->idm = idm_init();
	qmap->tophd = qmap->topn = 0;
	qmap->phd = hd;

	memset(qmap->map, 0xFF, ids_len);
	memset(qmap->omap, 0xFF, ids_len);
	memset(qmap->vmaps[QMAP_KEY], 0, keys_len);
	memset(qmap->vmaps[QMAP_VALUE], 0, values_len);

	return hd;
}

/* Open a qmap by specifying types directly */
static unsigned
qmap_topen(const qmap_type_t *key_type,
		const qmap_type_t *value_type,
		unsigned mask, unsigned flags)
{
	const qmap_type_t *backup_key_type = key_type;
#ifdef FEAT_DUP_TWO_WAY
	int prim_dup = 0;
#endif

	unsigned phd = _qmap_open(key_type,
			value_type, mask, flags);

	if (!(flags & QMAP_TWO_WAY))
		return phd;

#ifdef FEAT_DUP_TWO_WAY
	// we need a special type to account
	// for both key and value in this case
	// in case we want n <-> n
	if (flags & QMAP_DUP) {
		// TODO make sure we free this
		key_type = malloc(sizeof(qmap_type_t));
		CBUG(!key_type, "type alloc error\n");
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

	_qmap_open(value_type, key_type,
			mask, flags | QMAP_PGET);

	qmap_assoc(phd + 1, phd, qmap_assoc_rhd);

#ifdef FEAT_DUP_TWO_WAY
	if (prim_dup) {
		_qmap_open(key_type, value_type,
				mask, flags);

		qmap_assoc(phd + 2, phd, NULL);
	}
#endif

	return phd;
}

unsigned /* API */
qmap_open(const char * const key_tid,
		const char * const value_tid,
		unsigned mask, unsigned flags) {
	qmap_type_t *key_type, *value_type;
	unsigned hd;

	CBUG(qmap_get(types_hd, &key_type, key_tid),
			"key type was not registered\n");

	CBUG(qmap_get(types_hd, &value_type, value_tid),
			"key type was not registered\n");

	hd = qmap_topen(key_type, value_type, mask, flags);

	qmaps[hd].type_str[QMAP_KEY] = key_tid;
	qmaps[hd].type_str[QMAP_VALUE] = value_tid;
	return hd;
}

void /* API */
qmap_init(void)
{
	idm = idm_init();
	cursor_idm = idm_init();

	types_hd = qmap_topen(&type_string, &type_ptr,
			0, QMAP_TWO_WAY);

	assoc_hd = qmap_topen(&type_unsigned,
			&type_unsigned, 0, QMAP_DUP);

	qmap_regc("s", &type_string);
	qmap_regc("u", &type_unsigned);
	qmap_regc("p", &type_ptr);
}

/* }}} */

/* PUT {{{ */

#ifdef FEAT_REHASH
/* This is for rehashing a qmap to make it able to fit more
 * things inside it.
 */
static inline void
qmap_rehash(unsigned hd, unsigned new_m)
{
	register qmap_t *qmap = &qmaps[hd];
	register size_t map_len
		= new_m * sizeof(unsigned);
	size_t key_len, val_len;

	qmap->m = new_m;
	qmap->map = realloc(qmap->map, map_len);
	qmap->omap = realloc(qmap->omap, map_len);

	key_len = qmap->lens[QMAP_KEY];
	qmap->vmaps[QMAP_KEY]
		= realloc(qmap->vmaps[QMAP_KEY],
				qmap->m * key_len);

	val_len = qmap->lens[QMAP_VALUE];
	qmap->vmaps[QMAP_VALUE]
		= realloc(qmap->vmaps[QMAP_VALUE],
				qmap->m * val_len);

	CBUG(!(qmap->map && qmap->omap
				&& qmap->vmaps[QMAP_KEY]
				&& qmap->vmaps[QMAP_VALUE]),
			"realloc error\n");
}
#endif

/* This is a very low-level construct to put values
 * where they belong.
 */
static inline void
qmap_mPUT(unsigned hd, enum qmap_mbr t,
		const void * const value, unsigned n)
{
	qmap_t *qmap = &qmaps[hd];
	const void * real_value = value;
	void *tmp = (void *) value;
	size_t len = qmap->lens[t];
	int dup_value = (qmap->flags & QMAP_DUP)
		&& t == QMAP_VALUE;

	if (!dup_value && !qmap->type[t]->len) {
		size_t real_len
			= qmap_len(hd, value, t);

		tmp = malloc(real_len);
		CBUG(!tmp, "var len alloc error\n");
		memcpy(tmp, value, real_len);
		real_value = &tmp;
	}

	memcpy(qmap_val(qmap, t, n), real_value, len);
}

/* calculate the n of a keyed put */
static inline unsigned
qmap_keyed_n(unsigned hd, unsigned id, unsigned pn) {
	qmap_t *qmap = &qmaps[hd];

	if (pn == QMAP_MISS) {
		if (!(qmaps[qmap->phd].flags & QMAP_AINDEX))
			return idm_new(&qmap->idm);

		pn = id;
	}

	if (qmap->omap[pn] != id)
		idm_push(&qmap->idm, pn);

	return pn;
}

/* This is similar to qmap_mPUT except that it already puts
 * both value and key, and gets an 'n' and an 'id' to insert
 */
static inline unsigned
qmap_PUT(unsigned hd, const void * const key,
		const void * const value, unsigned pn)
{
	qmap_t *qmap = &qmaps[hd];
	unsigned n;
	unsigned id;
	const void *rkey;

	rkey = key;
	if (key) {
		id = qmap_id(hd, key);
		n = qmap_keyed_n(hd, id, pn);
	} else {
		n = idm_new(&qmap->idm);
		id = n;
		rkey = &n;
	}

#ifdef FEAT_REHASH
	if (qmap->m <= id)
		qmap_rehash(id * 2);
#else
	if (n > qmap->m) {
		return QMAP_MISS;
	}
#endif

	qmap->omap[n] = id;

	qmap_mPUT(hd, QMAP_KEY, rkey, n);
	qmap_mPUT(hd, QMAP_VALUE, value, n);

	qmap->map[id] = n;
	DEBUG(4, "%u's _put %u %u\n", hd, id, n);

	return n;
}

/* this is a highter-level put but for internal use only.
 * It's designed to know how to put in DUP databases.
 */
static unsigned
_qmap_put(unsigned hd, const void * const key,
		const void * const value, unsigned pn)
{
	qmap_t *qmap = &qmaps[hd], *iqmap;
	unsigned in_hd, id, n;

	if (!(qmap->flags & QMAP_DUP)) {
		n = qmap_PUT(hd, key, value, pn);
		return n;
	}

	id = qmap_id(hd, key);
	n = qmap->map[id];

	if (n >= qmap->idm.last || qmap->omap[n] != id)
	{
		in_hd = qmap_topen(
				qmap->type[QMAP_KEY],
				qmap->type[QMAP_VALUE],
				0, QMAP_AINDEX | (qmap->flags & QMAP_PGET));

		iqmap = &qmaps[in_hd];
		iqmap->tophd = hd;

		if (qmap->assoc) {
			iqmap->assoc = qmap->assoc;
			iqmap->phd = qmap->phd;
		}

#ifndef FEAT_REHASH
		n = qmap_PUT(hd, key, &in_hd, pn);
		iqmap->topn = n;
#endif
	} else
		in_hd = * (unsigned *)
			qmap_rval(hd, QMAP_VALUE, n);

	qmap_PUT(in_hd, NULL, value, QMAP_MISS);
	return n;
}

unsigned /* API */
qmap_put(unsigned hd, const void * const key,
		const void * const value)
{
	unsigned ret, cur, linked_hd, n;

#ifdef FEAT_DUP_TWO_WAY
	qmap_t *qmap = &qmaps[hd];
	size_t key_len, value_len;
	unsigned flags = qmap->flags;

	if (key == NULL)
		goto normal;

	key_len = qmap_len(hd, key, QMAP_KEY);

	if ((flags & QMAP_TWO_WAY)
			&& (flags & QMAP_DUP))
	{
		key_len = qmap_len(hd + 2,
				key, QMAP_KEY);

		value_len = qmap_len(hd + 2,
				value, QMAP_VALUE);

		char buf[key_len + value_len];

		memcpy(buf, key, key_len);
		memcpy(buf + key_len,
				value, value_len);

		n = _qmap_put(hd, buf, value, QMAP_MISS);
		goto proceed;
	}
normal:
#endif
	n = _qmap_put(hd, key, value, QMAP_MISS);

#ifdef FEAT_DUP_TWO_WAY
proceed:
#endif

	cur = qmap_iter(assoc_hd, &hd);
	while (qmap_lnext(cur)) {
		qmap_t *aqmap;
		const void *skey;

		qmap_cget(&linked_hd, cur, QMAP_VALUE);
		aqmap = &qmaps[linked_hd];
		aqmap->assoc(&skey, key ? key : &ret, value);

		_qmap_put(linked_hd, skey, value, n);
	}

	return n;
}

/* }}} */

/* GET {{{ */

int /* API */
qmap_pget(unsigned hd, void *target, void *key)
{
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

	value = qmap_rval(qmap->phd, QMAP_KEY, n);
	len = qmap_len(qmap->phd, value, QMAP_KEY);
	memcpy(target, value, len);
	return 0;
}

int /* API */
qmap_get(unsigned hd, void * const value,
		const void * const key)
{
	unsigned cur_id = qmap_iter(hd, key);;

	if (!qmap_lnext(cur_id))
		return 1;

	qmap_cget(value, cur_id, QMAP_VALUE);
	qmap_fin(cur_id);
	return 0;
}

/* This gets the pointer to the data under the cursor */
static inline void *
*qmap_csget(qmap_cur_t * const cursor, enum qmap_mbr t)
{
	qmap_t *qmap = &qmaps[cursor->hd];
	unsigned n = cursor->position - 1;

	if (t == QMAP_KEY && qmap->tophd)
		return qmap_rval(qmap->tophd, t, qmap->topn);
	else if (t == QMAP_VALUE && (qmap->flags & QMAP_PGET))
		return qmap_rval(qmap->phd, QMAP_KEY,
				qmap->tophd ? qmap->topn : n);

	return qmap_rval(cursor->hd, t, n);
}

void /* API */
qmap_cget(void * const target,
		unsigned cur_id, enum qmap_mbr t)
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

// simplest possible
static inline void
qmap_rdel(unsigned hd, unsigned n) {
	register qmap_t *qmap = &qmaps[hd];
	unsigned id = qmap->omap[n];

	if (id == QMAP_MISS)
		return;

	if (!(qmap->type[QMAP_KEY]->len)) {
		void *key = qmap_val(qmap, QMAP_KEY, n);
		free(* (void **) key);
	}

	if (!((qmap->flags & QMAP_DUP) || (qmap->type[QMAP_VALUE]->len))) {
		void *value = qmap_val(qmap, QMAP_VALUE, n);
		free(* (void **) value);
	}

	qmap->map[id] = QMAP_MISS;
	qmap->omap[n] = QMAP_MISS;
	idm_del(&qmap->idm, n);

	if (qmap->tophd) {
		if (qmap->idm.last)
			return;

		qmap_drop(hd);
		qmap_rdel(qmap->tophd, qmap->topn);
	}

	if (qmap->phd == hd) {
		unsigned cur = qmap_iter(assoc_hd, &hd);

		while (qmap_lnext(cur)) {
			unsigned ahd;
			qmap_cget(&ahd, cur, QMAP_VALUE);
			qmap_rdel(ahd, n);
		}
	} else if (!qmap->tophd)
		qmap_rdel(qmap->phd, n);
}

void /* API */
qmap_cdel(unsigned cur_id)
{
	unsigned low_id = qmap_low_cur(cur_id);
	register qmap_cur_t *cursor = &qmap_cursors[low_id];
	unsigned n = cursor->position - 1;
	qmap_rdel(cursor->hd, n);
}

void /* API */
qmap_del(unsigned hd,
		const void * const key,
		const void * const value)
{
	unsigned cur = qmap_iter(hd, key);

	if (value) while (qmap_lnext(cur)) {
		if (qmap_ccmp(cur, QMAP_VALUE,
					value))
			continue;
		qmap_cdel(cur);
		qmap_fin(cur);
		return;
	} else while (qmap_lnext(cur))
		qmap_cdel(cur);
}

/* }}} */

/* ITERATION {{{ */

void /* API */
qmap_fin(unsigned cur_id)
{
	qmap_cur_t *cursor = &qmap_cursors[cur_id];

	if (cursor->sub_cur)
		qmap_fin(cursor->sub_cur);

	idm_del(&cursor_idm, cur_id);
}

unsigned /* API */
qmap_iter(unsigned hd, const void * const key)
{
	qmap_t *qmap = &qmaps[hd];
	unsigned cur_id = idm_new(&cursor_idm);
	qmap_cur_t *cursor = &qmap_cursors[cur_id];
	unsigned id;

	if (key) {
		cursor->id = id = qmap_id(hd, key);
		cursor->position = id > qmap->m
			? qmap->m
			: qmap->map[id];
	} else {
		cursor->position = 0;
		cursor->id = QMAP_MISS;
	}

	cursor->sub_cur = 0;
	cursor->hd = hd;
	cursor->key = key;
	return cur_id;
}

int /* API */
qmap_lnext(unsigned cur_id)
{
	unsigned low_id = qmap_low_cur(cur_id);
	register qmap_cur_t *cursor
		= &qmap_cursors[cur_id];
	register qmap_t *qmap = &qmaps[cursor->hd];
	unsigned n, id;
cagain:
	n = cursor->position;

	if (n >= qmap->idm.last)
		goto end;

	id = qmap->omap[n];
	if (id == QMAP_MISS) {
		cursor->position++;
		goto cagain;
	}

	DEBUG(3, "NEXT! high %u low %u n %u id %u\n",
			cur_id, low_id, n, id);
	cursor->id = id;

	if (qmap->flags & QMAP_DUP) {
		if (!cursor->sub_cur) {
			unsigned in_hd = * (unsigned *)
				qmap_rval(cursor->hd,
						QMAP_VALUE, n);

			cursor->sub_cur = qmap_iter(in_hd,
					NULL);

			cursor->position++;
		}

		if (qmap_lnext(cursor->sub_cur))
			return 1;

		if (cursor->key)
			goto end;

		cursor->sub_cur = 0;
		goto cagain;
	}

	if (cursor->key && _qmap_cmp(cursor->hd,
				QMAP_KEY, n,
				cursor->key))
		goto end;

	cursor->position++;
	return 1;
end:
	idm_del(&cursor_idm, cur_id);
	return 0;
}

/* }}} */

/* DROP + CLOSE + OTHERS {{{ */

void /* API */
qmap_drop(unsigned hd)
{
	unsigned cur_id = qmap_iter(hd, NULL);

	while (qmap_lnext(cur_id))
		qmap_cdel(cur_id);
}

void /* API */
qmap_close(unsigned hd)
{
	qmap_t *qmap = &qmaps[hd];

	if (qmap->flags & QMAP_TWO_WAY)
		qmap_close(hd + 1);
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

void /* API */
qmap_assoc(unsigned hd, unsigned link, qmap_assoc_t cb)
{
	qmap_t *qmap = &qmaps[hd];

	if (!cb)
		cb = qmap_twin_assoc;

	DEBUG(1, "%u Assoc %u!\n", hd, link)
	_qmap_put(assoc_hd, &link, &hd, QMAP_MISS);

	qmap->assoc = cb;
	qmap->phd = link;
}

void /* API */
qmap_print(char * const target, unsigned hd,
		unsigned type, const void * const thing)
{
	qmap_t *qmap = &qmaps[hd];
	if (qmap->flags & QMAP_TWO_WAY)
		hd += 1;
	qmap->type[type]->print(target, thing);
}

int /* API */
qmap_cmp(unsigned hd, enum qmap_mbr t,
		const void * const a,
		const void * const b)
{
	qmap_t *qmap = &qmaps[hd];
	qmap_compare_t *cmp = qmap->type[t]->compare;

	if (!cmp)
		cmp = other_compare;

	return cmp(a, b, qmap->type[t]->len);
}

const char * /* API */
qmap_type(unsigned hd, enum qmap_mbr t)
{
	return qmaps[hd].type_str[t];
}

unsigned /* API */
qmap_flags(unsigned hd)
{
	return qmaps[hd].flags;
}

/* }}} */
