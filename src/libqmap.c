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

static_assert(QM_MISS == UINT_MAX, "assume UINT_MAX");

typedef struct {
	// these have to do with keys
	unsigned *map;  	// id -> n
	const void **omap;	// n -> key

	unsigned type;
	unsigned m, mask, flags;
	idm_t idm;
	ids_t linked;

	unsigned phd;
	qmap_assoc_t *assoc;
} qmap_t;

typedef struct {
	unsigned hd, pos, sub_cur, ipos;
	const void * key;
} qmap_cur_t;

typedef unsigned qmap_hash_t(
		const void * const key,
		size_t len);

typedef int qmap_cmp_t(
		const void * const a,
		const void * const b,
		size_t len);

typedef struct {
	size_t len;
	qmap_measure_t *measure;
	qmap_hash_t *hash;
	qmap_cmp_t *cmp;
} qmap_type_t;

enum qmap_if {
	QM_CLOSING = 4,
};

unsigned flag_reset = QM_CLOSING;
qmap_t qmaps[QM_MAX];
qmap_cur_t qmap_cursors[QM_MAX];
idm_t idm, cursor_idm;

qmap_type_t types[TYPES_MASK + 1];
unsigned types_hd;

/* }}} */

/* BUILT-INS {{{ */

static unsigned
qmap_phash(const void * const key, size_t len UNUSED)
{
	return XXH32(key, sizeof(void *), QM_SEED);
}

static unsigned
qmap_nohash(const void * const key, size_t len UNUSED)
{
	unsigned u;
	memcpy(&u, key, sizeof(u));
	return u;
}

unsigned
qmap_chash(const void *data, size_t len) {
	return XXH32(data, len, QM_SEED);
}

static int
qmap_pcmp(const void * const a,
		const void * const b,
		size_t len UNUSED)
{
	return b != a;
}

int
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

/* In some cases we want to calculate the id based on the
 * qmap's hash function and the key, and the mask. Other
 * times it's not useful to do that. This is for when it is.
 */
static inline unsigned
qmap_id(unsigned hd, const void * const key)
{
	qmap_t *qmap = &qmaps[hd];
	qmap_type_t *type = &types[qmap->type];
	size_t len = type->measure
		? type->measure(key)
		: type->len;
	unsigned id = type->hash(key, len)
		& qmap->mask;
	unsigned n;

	if (qmap->type & QM_HNDL)
		return id;

	while (1) {
		n = qmap->map[id];
		if (n == QM_MISS)
			break;
		if (!type->cmp(qmap->omap[n], key, len))
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
_qmap_open(unsigned type, unsigned mask, unsigned flags)
{
	unsigned hd = idm_new(&idm);
	qmap_t *qmap = &qmaps[hd];
	unsigned len;
	size_t ids_len;

	mask = mask ? mask : QM_DEFAULT_MASK;

	DEBUG(1, "%u %u 0x%x %u\n",
			hd, type,
			mask, flags);

	len = mask + 1u;

	CBUG((len & mask) != 0, "mask must be 2^k - 1\n");
	ids_len = len * sizeof(unsigned);

	qmap->map = malloc(ids_len);
	qmap->omap = malloc(len * sizeof(void *));
	CBUG(!(qmap->map && qmap->omap), "malloc error\n");
	qmap->m = len;
	qmap->type = type;
	qmap->mask = mask;
	qmap->flags = flags & ~flag_reset;
	qmap->idm = idm_init();
	qmap->phd = hd;
	qmap->linked = ids_init();

	memset(qmap->map, 0xFF, ids_len);
	for (unsigned i = 0; i < len; i++)
		qmap->omap[i] = NULL;

	return hd;
}

unsigned /* API */
qmap_open(unsigned key_type, unsigned value_type,
		unsigned mask, unsigned flags)
{
	unsigned hd = _qmap_open(key_type, mask, flags);

	if (!(flags & QM_MIRROR))
		return hd;

       	_qmap_open(value_type, mask, flags);
	qmap_assoc(hd + 1, hd, NULL);

	return hd;
}

size_t s_measure(const void *key) {
	return strlen(key) + 1;
}

void /* API */
qmap_init(void)
{
	qmap_type_t *type;
	idm = idm_init();
	cursor_idm = idm_init();

	types_hd = qmap_open(QM_HNDL, QM_HASH,
			TYPES_MASK, QM_AINDEX);

	// QM_HASH (PTR)
	type = &types[qmap_put(types_hd, NULL, NULL)];
	type->hash = qmap_phash;
	type->cmp = qmap_pcmp;

	// QM_HNDL
	type = &types[qmap_put(types_hd, NULL, NULL)];
	type->hash = qmap_nohash;
	type->cmp = qmap_pcmp;

	// QM_STR
	qmap_mreg(s_measure);
}

/* }}} */

/* PUT {{{ */

/* calculate the n of a keyed put */
static inline unsigned
qmap_keyed_n(unsigned hd, unsigned id, unsigned pn) {
	qmap_t *qmap = &qmaps[hd];

	if (pn == QM_MISS)
		pn = id;

	if (qmap->idm.last < pn + 1)
		idm_push(&qmap->idm, pn);

	return pn;
}

/* This is similar to qmap_mPUT except that it already puts
 * both value and key, and gets an 'n' and an 'id' to insert
 */
static inline unsigned
_qmap_put(unsigned hd, const void * key, unsigned pn)
{
	qmap_t *qmap = &qmaps[hd];
	unsigned n;
	unsigned id;

	if (key) {
		id = qmap_id(hd, key);
		n = qmap_keyed_n(hd, id, pn);
	} else {
		n = idm_new(&qmap->idm);
		id = n;
	}

	if (qmap->flags & QM_AINDEX)
		key = &qmap->map[id];

	CBUG(n >= qmap->m, "Capacity reached\n");
	DEBUG(2, "%u %u %u %p\n", hd, n, id, key);

	qmap->omap[n] = key;
	qmap->map[id] = n;
	return n;
}

unsigned /* API */
qmap_put(unsigned hd, const void * const key,
		const void * const value)
{
	unsigned ahd, n;
	idsi_t *cur;

	n = _qmap_put(hd, key, QM_MISS);

	cur = ids_iter(&qmaps[hd].linked);
	while (ids_next(&ahd, &cur)) {
		qmap_t *aqmap;
		const void *skey;

		aqmap = &qmaps[ahd];
		aqmap->assoc(&skey, key, value);

		_qmap_put(ahd, skey, n);
	}

	return n;
}

/* }}} */

/* GET {{{ */

unsigned /* API */
qmap_get(unsigned hd, const void * const key)
{
	unsigned cur_id = qmap_iter(hd, key), sn;

	if (!qmap_next(&sn, cur_id))
		return QM_MISS;

	qmap_fin(cur_id);
	return sn;
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
    unsigned id = qmap_id(hd, qmap->omap[n]), ahd;
    idsi_t *cur;

    if (n >= qmap->m)
	    return;

    if (id != QM_MISS) {
        qmap->map[id] = QM_MISS;
        qmap->omap[n] = NULL;
        idm_del(&qmap->idm, n);
    }

    cur = ids_iter(&qmap->linked);

    while (ids_next(&ahd, &cur))
        qmap_ndel_topdown(ahd, n);
}

/* Delete based on position */
static inline void
qmap_ndel(unsigned hd, unsigned n) {
    qmap_ndel_topdown(qmap_root(hd), n);
}

void /* API */
qmap_del(unsigned hd, const void * const key)
{
	unsigned cur = qmap_iter(hd, key), sn;

	while (qmap_next(&sn, cur))
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
qmap_iter(unsigned hd, const void * const key)
{
	qmap_t *qmap = &qmaps[hd];
	unsigned cur_id = idm_new(&cursor_idm);
	qmap_cur_t *cursor = &qmap_cursors[cur_id];
	unsigned id;

	if (key) {
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
	return cur_id;
}

int /* API */
qmap_next(unsigned *sn, unsigned cur_id)
{
	register qmap_cur_t *cursor
		= &qmap_cursors[cur_id];
	register qmap_t *qmap = &qmaps[cursor->hd];
	unsigned n;
	const void *key;
cagain:
	n = cursor->pos;

	if (cursor->key && n != cursor->ipos)
		goto end;

	if (n >= qmap->idm.last)
		goto end;

	key = qmap->omap[n];
	if (key == NULL) {
		cursor->pos++;
		goto cagain;
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

/* }}} */

/* DROP + CLOSE + OTHERS {{{ */

void /* API */
qmap_drop(unsigned hd)
{
	unsigned cur_id = qmap_iter(hd, NULL), sn;

	while (qmap_next(&sn, cur_id))
		qmap_ndel(hd, sn);
}

void /* API */
qmap_close(unsigned hd)
{
	qmap_t *qmap = &qmaps[hd];
	idsi_t *cur;
	unsigned ahd;

	if (qmap->flags & QM_CLOSING)
		return;

	cur = ids_iter(&qmap->linked);
	while (ids_next(&ahd, &cur))
		qmap_close(ahd);

	qmap_drop(hd);

	ids_drop(&qmap->linked);
	ids_drop(&qmap->idm.free);
	qmap->idm.last = 0;
	qmap->flags &= ~flag_reset;
	free(qmap->map);
	free(qmap->omap);
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
}

void * /* API */
qmap_key(unsigned hd, unsigned id)
{
	qmap_t *qmap = &qmaps[hd];
	return (void *) qmap->omap[id];
}

unsigned /* API */
qmap_reg(size_t len)
{
	unsigned id = qmap_put(types_hd, NULL, NULL);
	qmap_type_t *type = &types[id];

	memset(type, 0, sizeof(qmap_type_t));
	type->len = len;
	type->hash = qmap_chash;
	type->cmp = qmap_ccmp;
	return id;
}

unsigned /* API */
qmap_mreg(qmap_measure_t *measure)
{
	unsigned id = qmap_put(types_hd, NULL, NULL);
	qmap_type_t *type = &types[id];

	memset(type, 0, sizeof(qmap_type_t));
	type->measure = measure;
	type->hash = qmap_chash;
	type->cmp = qmap_ccmp;
	return id;
}

/* }}} */
