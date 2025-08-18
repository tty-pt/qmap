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

#define QMAP_SEED 13
#define QMAP_DEFAULT_MASK 0xFF
#define TYPES_MASK 0xFF

#define DEBUG_LVL 1

#define DEBUG(lvl, ...) \
	if (DEBUG_LVL > lvl) WARN(__VA_ARGS__)

static_assert(QMAP_MISS == UINT_MAX, "assume UINT_MAX");

typedef unsigned qmap_hash_t(const void * const key);

typedef struct {
	// these have to do with keys
	unsigned *map;  // id -> n
	unsigned *omap; // n -> id

	unsigned type;
	unsigned m, mask, flags;
	qmap_hash_t *hash;
	idm_t idm;
	ids_t linked;

	unsigned phd;
	qmap_assoc_t *assoc;
} qmap_t;

typedef struct {
	unsigned hd, pos, sub_cur, ipos;
	const void * key;
} qmap_cur_t;

enum qmap_if {
	QMAP_CLOSING = 4,
};

unsigned flag_reset = QMAP_CLOSING;
qmap_t qmaps[QMAP_MAX];
qmap_cur_t qmap_cursors[QMAP_MAX];
idm_t idm, cursor_idm;
size_t type_lens[TYPES_MASK];
unsigned types_n = 0;

/* }}} */

/* BUILT-INS {{{ */

/* HASH FUNCTIONS */

static unsigned
qmap_hash(const void * const key)
{
	return XXH32(&key, sizeof(void *), QMAP_SEED);
}

static unsigned
qmap_nohash(const void * const key)
{
	unsigned u;
	memcpy(&u, key, sizeof(u));
	return u;
}

int
qmap_rassoc(const void **skey,
		const void * const pkey UNUSED,
		const void * const value)
{
	*skey = value;
	return 0;
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
	unsigned id = qmap->hash(key) & qmap->mask;
	return id;
}

/* }}} */

/* OPEN / INITIALIZATION {{{ */

unsigned /* API */
qmap_reg(size_t len) {
	CBUG(types_n >= TYPES_MASK, "too many types\n");
	unsigned n = types_n++;
	type_lens[n] = len;
	return n;
}

/* This is used by qmap_open to open databases. */
unsigned
qmap_sopen(unsigned type, unsigned mask, unsigned flags)
{
	unsigned hd = idm_new(&idm);
	qmap_t *qmap = &qmaps[hd];
	unsigned len;
	size_t ids_len;


	CBUG(type >= types_n, "invalid type\n");

	qmap->hash = qmap_hash;
	if (type_lens[type] <= sizeof(unsigned)
			&& type_lens[type])
		qmap->hash = qmap_nohash;

	mask = mask ? mask : QMAP_DEFAULT_MASK;

	DEBUG(1, "%u %u 0x%x %u\n",
			hd, type,
			mask, flags);

	len = mask + 1u;

	CBUG((len & mask) != 0, "mask must be 2^k - 1\n");
	ids_len = len * sizeof(unsigned);

	qmap->map = malloc(ids_len);
	qmap->omap = malloc(ids_len);
	CBUG(!(qmap->map && qmap->omap), "malloc error\n");
	qmap->m = len;
	qmap->type = type;
	qmap->mask = mask;
	qmap->flags = flags & ~flag_reset;
	qmap->idm = idm_init();
	qmap->phd = hd;
	qmap->linked = ids_init();

	memset(qmap->map, 0xFF, ids_len);
	memset(qmap->omap, 0xFF, ids_len);

	return hd;
}

void /* API */
qmap_init(void)
{
	idm = idm_init();
	cursor_idm = idm_init();

	qmap_reg(0);
	qmap_reg(sizeof(unsigned));
}

/* }}} */

/* PUT {{{ */

/* calculate the n of a keyed put */
static inline unsigned
qmap_keyed_n(unsigned hd, unsigned id, unsigned pn) {
	qmap_t *qmap = &qmaps[hd];

	if (pn == QMAP_MISS) {
		if (!(qmaps[qmap->phd].flags & QMAP_AINDEX))
			return idm_new(&qmap->idm);

		pn = id;
	}

	if (qmap->idm.last < pn + 1)
		idm_push(&qmap->idm, pn);

	return pn;
}

/* This is similar to qmap_mPUT except that it already puts
 * both value and key, and gets an 'n' and an 'id' to insert
 */
static inline unsigned
_qmap_put(unsigned hd, const void * const key, unsigned pn)
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

	CBUG(n >= qmap->m, "Capacity reached\n");

	qmap->omap[n] = id;
	qmap->map[id] = n;
	return n;
}

unsigned /* API */
qmap_put(unsigned hd, const void * const key,
		const void * const value)
{
	unsigned ahd, n;
	idsi_t *cur;

	n = _qmap_put(hd, key, QMAP_MISS);

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
		return QMAP_MISS;

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
    unsigned id = qmap->omap[n], ahd;
    idsi_t *cur;

    if (n >= qmap->m)
	    return;

    if (id != QMAP_MISS) {
        qmap->map[id] = QMAP_MISS;
        qmap->omap[n] = QMAP_MISS;
        idm_del(&qmap->idm, n);
    }

    cur = ids_iter(&qmap->linked);

    while (ids_next(&ahd, &cur))
        qmap_ndel_topdown(ahd, n);
}

void /* API */
qmap_ndel(unsigned hd, unsigned n) {
    qmap_ndel_topdown(qmap_root(hd), n);
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
		id = qmap_id(hd, key);
		cursor->pos = id >= qmap->m
			? qmap->m
			: qmap->map[id];
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
	unsigned n, id;
cagain:
	n = cursor->pos;

	if (cursor->key && n != cursor->ipos)
		goto end;

	if (n >= qmap->idm.last)
		goto end;

	id = qmap->omap[n];
	if (id == QMAP_MISS) {
		cursor->pos++;
		goto cagain;
	}

	DEBUG(3, "NEXT! cur_id %u id %u\n",
			cur_id, id);

	cursor->pos++;
	*sn = n;
	return 1;
end:
	idm_del(&cursor_idm, cur_id);
	*sn = QMAP_MISS;
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

	if (qmap->flags & QMAP_CLOSING)
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

unsigned /* API */
qmap_flags(unsigned hd)
{
	return qmaps[hd].flags;
}

/* }}} */
