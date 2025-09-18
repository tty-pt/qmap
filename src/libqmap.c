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
#include <limits.h>

/* MACROS, STRUCTS, ENUMS AND GLOBALS {{{ */

#define QM_SEED 13
#define QM_DEFAULT_MASK 0xFF
#define QM_MAX 1024

#define TYPES_MASK 0xFF

#define DEBUG_LVL 1

#define DEBUG(lvl, ...) \
	if (DEBUG_LVL > lvl) WARN(__VA_ARGS__)

#define VAL_ADDR(qmap, n) \
	(void **)(((char *) qmap->table) \
			+ sizeof(void *) * n)

static_assert(QM_MISS == UINT_MAX, "assume UINT_MAX");

enum QM_MBR {
	QM_KEY,
	QM_VALUE,
};

typedef struct {
	// these have to do with keys
	unsigned *map;  	// id -> n
	const void **omap;	// n -> key

	void **table;
	unsigned types[2];
	unsigned m, mask, flags;
	idm_t idm;
	ids_t linked;

	unsigned phd;
	qmap_assoc_t *assoc;
} qmap_t;

typedef struct {
	unsigned hd, pos, sub_cur, ipos, flags;
	const void * key;
} qmap_cur_t;

typedef unsigned qmap_hash_t(
		const void * const key,
		size_t len);

typedef struct {
	size_t len;
	qmap_measure_t *measure;
	qmap_hash_t *hash;
	qmap_cmp_t *cmp;
} qmap_type_t;

static qmap_t qmaps[QM_MAX];
static qmap_cur_t qmap_cursors[QM_MAX];
static idm_t idm, cursor_idm;

static qmap_type_t qmap_types[TYPES_MASK + 1];
static unsigned types_n = 0;

/* }}} */

/* BUILT-INS {{{ */

static unsigned
qmap_nohash(const void * const key, size_t len UNUSED)
{
	unsigned u;
	memcpy(&u, key, sizeof(u));
	return u;
}

static unsigned
qmap_chash(const void *data, size_t len) {
	return XXH32(data, len, QM_SEED);
}

static int
qmap_ccmp(const void * const a,
		const void * const b,
		size_t len)
{
	return memcmp((char *) b, (char *) a, len);
}

static void
qmap_rassoc(const void **skey,
		const void * const pkey UNUSED,
		const void * const value)
{
	*skey = value;
}

/* }}} */

/* HELPER FUNCTIONS {{{ */

/* Easily obtain the pointer to the key */
static inline void *
qmap_key(unsigned hd, unsigned n)
{
	qmap_t *qmap = &qmaps[hd];
	return (void *) qmap->omap[n];
}

/* Easily obtain the pointer to the value */
static inline void *
qmap_val(unsigned hd, unsigned n) {
	qmap_t *qmap = &qmaps[hd], *pqmap;

	if (qmap->flags & QM_PGET)
		return qmap_key(qmap->phd, n);

	pqmap = &qmaps[qmap->phd];
	return * VAL_ADDR(pqmap, n);
}

/* In some cases we want to calculate the id based on the
 * qmap's hash function and the key, and the mask. Other
 * times it's not useful to do that. This is for when it is.
 */
static inline unsigned
qmap_id(unsigned hd, const void * const key)
{
	qmap_t *qmap = &qmaps[hd];
	qmap_type_t *type = &qmap_types[qmap->types[QM_KEY]];
	size_t len = type->measure
		? type->measure(key)
		: type->len;
	unsigned id = type->hash(key, len)
		& qmap->mask;
	unsigned n;
	const void *okey;

	if (qmap->types[QM_KEY] == QM_HNDL)
		return id;

	while (1) {
		n = qmap->map[id];
		if (n == QM_MISS)
			break;
		okey = qmap_key(hd, n);
		// FIXME NO CLUE WHY THIS IS NEEDED
		if (okey == NULL)
			break;
		if (!type->cmp(okey, key, len))
			break;
		id ++;
		id &= qmap->mask;
	}

	return id;
}

/* }}} */

/* OPEN / INITIALIZATION {{{ */

/* Low level way of opening databases. */
static unsigned
_qmap_open(unsigned ktype, unsigned vtype,
		unsigned mask, unsigned flags)
{
	unsigned hd = idm_new(&idm);
	qmap_t *qmap = &qmaps[hd];
	unsigned len;
	size_t ids_len;

	mask = mask ? mask : QM_DEFAULT_MASK;

	DEBUG(1, "%u %u 0x%x %u\n",
			hd, ktype,
			mask, flags);

	len = mask + 1u;

	CBUG((len & mask) != 0, "mask must be 2^k - 1\n");
	ids_len = len * sizeof(unsigned);

	qmap->map = malloc(ids_len);
	qmap->omap = malloc(len * sizeof(void *));
	CBUG(!(qmap->map && qmap->omap), "malloc error\n");
	qmap->m = len;
	qmap->types[QM_KEY] = ktype;
	qmap->types[QM_VALUE] = vtype;
	qmap->mask = mask;
	qmap->flags = flags;
	qmap->idm = idm_init();
	qmap->phd = hd;
	qmap->linked = ids_init();

	// STORE {{{
	qmap->table = malloc(sizeof(void *) * len);
	memset(qmap->table, 0, sizeof(void *) * len);
	// }}}

	memset(qmap->map, 0xFF, ids_len);
	memset(qmap->omap, 0, sizeof(void *) * len);

	return hd;
}

unsigned /* API */
qmap_open(unsigned ktype, unsigned vtype,
		unsigned mask, unsigned flags)
{
	unsigned hd = _qmap_open(ktype, vtype, mask, flags);

	if (!(flags & QM_MIRROR))
		return hd;

	flags &= ~QM_AINDEX;
	_qmap_open(vtype, ktype, mask, flags | QM_PGET);
	qmap_assoc(hd + 1, hd, NULL);

	return hd;
}

static size_t
s_measure(const void *key)
{
	return strlen(key) + 1;
}

__attribute__((destructor))
static void qmap_destruct(void) {
	for (unsigned i = 0; i < idm.last; i++)
		qmap_close(i);

	idm_drop(&cursor_idm);
	idm_drop(&idm);
}

__attribute__((constructor))
static void
qmap_init(void)
{
	qmap_type_t *type;
	idm = idm_init();
	cursor_idm = idm_init();

	// QM_PTR
	type = &qmap_types[qmap_reg(sizeof(void *))];

	// QM_HNDL
	type = &qmap_types[qmap_reg(sizeof(unsigned))];
	type->hash = qmap_nohash;

	// QM_STR
	qmap_mreg(s_measure);
}

/* }}} */

/* PUT {{{ */

/* This is the low-level put. It doesn't aim to provide
 * MIRROR functionality in itself, just putting in whatever
 * kind of map.
 */
static inline unsigned
_qmap_put(unsigned hd, const void * key,
		const void *value, unsigned pn)
{
	qmap_t *qmap = &qmaps[hd];
	unsigned n;
	unsigned id;
	const void *aval = value;
	void *rval, *rkey;
	size_t klen;

	if (key) {
		unsigned old_n;

		id = qmap_id(hd, key);
		old_n = qmap->map[id];

		if (old_n == QM_MISS) {
			n = idm_new(&qmap->idm);
			if (pn != QM_MISS)
				n = pn;
		} else
			n = old_n;
	} else {
		id = n = idm_new(&qmap->idm);
		key = &id;
	}

	CBUG(n >= qmap->m, "Capacity reached\n");
	DEBUG(2, "%u %u %u %p\n", hd, n, id, key);
	rkey = key ? (void *) key : &qmap->map[id];

	if (qmap->phd == hd) {
		if (qmap->types[QM_VALUE] == QM_PTR)
			value = &value;

		if (qmap->map[id] == n) {
			const void *ekey = qmap_key(hd, n);
			const void *eval = qmap_val(hd, n);

			free((void *) ekey);
			free((void *) eval);
		}

		klen = qmap_len(qmap->types[QM_VALUE], aval);
		rval = malloc(klen);
		* VAL_ADDR(qmap, n) = rval;
		memcpy(rval, value, klen);

		// this could be avoided
		// if the key is the same
		klen = qmap_len(qmap->types[QM_KEY], key);
		rkey = malloc(klen);
		memcpy(rkey, key, klen);
	}

	qmap->map[id] = n;
	qmap->omap[n] = rkey;

	return id;
}

unsigned /* API */
qmap_put(unsigned hd, const void * const key,
		const void * const value)
{
	unsigned ahd, n, id;
	idsi_t *cur;
	const void *rkey, *rval;

	id = _qmap_put(hd, key, value, QM_MISS);
	n = qmaps[hd].map[id];

	cur = ids_iter(&qmaps[hd].linked);
	rkey = qmap_key(hd, n);
	rval = qmap_val(hd, n);

	while (ids_next(&ahd, &cur)) {
		qmap_t *aqmap;
		const void *skey;

		aqmap = &qmaps[ahd];
		aqmap->assoc(&skey, rkey, rval);

		_qmap_put(ahd, skey, rval, n);
	}

	return id;
}

/* }}} */

/* GET {{{ */

static int qmap_lnext(unsigned *sn, unsigned cur_id);

const void * /* API */
qmap_get(unsigned hd, const void * const key)
{
	unsigned cur_id = qmap_iter(hd, key, 0), sn;

	if (!qmap_lnext(&sn, cur_id))
		return NULL;

	qmap_fin(cur_id);

	return qmap_val(hd, sn);
}

/* }}} */

/* DELETE {{{ */

static inline unsigned
qmap_root(unsigned hd)
{
	while(qmaps[hd].phd != hd)
		hd = qmaps[hd].phd;

	return hd;
}

static void qmap_ndel_topdown(unsigned hd, unsigned n){
	qmap_t *qmap = &qmaps[hd];
	const void *key, *value;
	unsigned id, ahd;
	idsi_t *cur;

	if (n >= qmap->m)
		return;

	cur = ids_iter(&qmap->linked);

	while (ids_next(&ahd, &cur))
		qmap_ndel_topdown(ahd, n);

	key = qmap_key(hd, n);

	id = qmap_id(hd, key);

	if (qmap->phd == hd) {
		value = qmap_val(hd, n);
		free((void *) key);
		free((void *) value);
	}

	qmap->map[id] = QM_MISS;
	qmap->omap[n] = NULL;
	idm_del(&qmap->idm, n);

}

/* Delete based on position */
static inline void
qmap_ndel(unsigned hd, unsigned n) {
	qmap_ndel_topdown(qmap_root(hd), n);
}

void /* API */
qmap_del(unsigned hd, const void * const key)
{
	unsigned cur = qmap_iter(hd, key, 0), sn;

	while (qmap_lnext(&sn, cur))
		qmap_ndel(hd, sn);
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
qmap_iter(unsigned hd, const void * const key, unsigned flags)
{
	qmap_t *qmap = &qmaps[hd];
	unsigned cur_id = idm_new(&cursor_idm);
	qmap_cur_t *cursor = &qmap_cursors[cur_id];
	unsigned id;

	if (key && !(flags & QM_RANGE)) {
		unsigned n;

		id = qmap_id(hd, key);

		CBUG(id >= qmap->m, "Strange. Id hash "
				"does not fit\n");

		n = qmap->map[id];
		DEBUG(2, "%u %u %u %p\n", hd, n, id, key);
		cursor->pos = n;
	} else {
		cursor->pos = 0;
	}

	cursor->ipos = cursor->pos;
	cursor->sub_cur = 0;
	cursor->hd = hd;
	cursor->key = key;
	cursor->flags = flags;
	return cur_id;
}

/* low-level next */
static int
qmap_lnext(unsigned *sn, unsigned cur_id)
{
	register qmap_cur_t *cursor
		= &qmap_cursors[cur_id];
	register qmap_t *qmap = &qmaps[cursor->hd];
	unsigned n;
	const void *key;
cagain:
	n = cursor->pos;

	if (n >= qmap->idm.last)
		goto end;

	key = qmap_key(cursor->hd, n);

	if (cursor->flags & QM_RANGE) {
		qmap_type_t *type
			= &qmap_types[qmap->types[QM_KEY]];

		if (!key || type->cmp(key, cursor->key, type->len) < 0) {
			cursor->pos++;
			goto cagain;
		}
	} else {
		if (key == NULL) {
			cursor->pos++;
			goto cagain;
		}

		if (cursor->key && n != cursor->ipos) {
			goto end;
		}
	}

	DEBUG(3, "NEXT! cur_id %u key %p\n",
			cur_id, key);

	cursor->pos++;
	*sn = n;
	return 1;
end:
	idm_del(&cursor_idm, cur_id);
	*sn = QM_MISS;
	return 0;
}

int /* API */
qmap_next(const void ** ckey, const void ** cval,
		unsigned cur_id)
{
	register qmap_cur_t *c;
	unsigned sn;
	int ret = qmap_lnext(&sn, cur_id);

	if (!ret)
		return 0;

	c = &qmap_cursors[cur_id];
	*ckey = qmap_key(c->hd, sn);
	*cval = qmap_val(c->hd, sn);
	return 1;
}

/* }}} */

/* DROP + CLOSE + OTHERS {{{ */

void /* API */
qmap_drop(unsigned hd)
{
	unsigned cur_id = qmap_iter(hd, NULL, 0), sn;

	while (qmap_lnext(&sn, cur_id))
		qmap_ndel(hd, sn);
}

void /* API */
qmap_close(unsigned hd)
{
	qmap_t *qmap = &qmaps[hd];
	idsi_t *cur;
	unsigned ahd;

	if (!qmap->omap)
		return;

	qmap_drop(hd);

	cur = ids_iter(&qmap->linked);
	while (ids_next(&ahd, &cur))
		qmap_close(ahd);

	ids_drop(&qmap->linked);
	idm_drop(&qmap->idm);
	qmap->idm.last = 0;
	free(qmap->map);
	free(qmap->omap);
	if (qmap->phd == hd)
		free(qmap->table);
	qmap->omap = NULL;
	idm_del(&idm, hd);
}

void /* API */
qmap_assoc(unsigned hd, unsigned link, qmap_assoc_t cb)
{
	qmap_t *qmap = &qmaps[hd];

	if (!cb)
		cb = qmap_rassoc;

	ids_push(&qmaps[link].linked, hd);

	qmap->assoc = cb;
	qmap->phd = link;
	free(qmap->table);
}

unsigned /* API */
qmap_reg(size_t len)
{
	unsigned id = types_n ++;
	qmap_type_t *type = &qmap_types[id];

	memset(type, 0, sizeof(qmap_type_t));
	type->len = len;
	type->hash = qmap_chash;
	type->cmp = qmap_ccmp;
	return id;
}

void
qmap_cmp_set(unsigned ref, qmap_cmp_t *cmp)
{
	qmap_type_t *type = &qmap_types[ref];
	type->cmp = cmp;
}

unsigned /* API */
qmap_mreg(qmap_measure_t *measure)
{
	unsigned id = types_n ++;
	qmap_type_t *type = &qmap_types[id];

	memset(type, 0, sizeof(qmap_type_t));
	type->measure = measure;
	type->hash = qmap_chash;
	type->cmp = qmap_ccmp;
	type->len = 0;
	return id;
}

size_t /* API */
qmap_len(unsigned type_id, const void *key)
{
	qmap_type_t *type = &qmap_types[type_id];

	return type->measure
		? type->measure(key)
		: type->len;
}

/* }}} */
