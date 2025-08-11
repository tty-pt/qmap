/* TODO
 * - DELETE should use a cursor, because we might want to delete a specific key / value pair
 * - GET should also use cursors, because we might need to get from a secondary database.
 * - PUT maybe should do it as well, to cover recursion correctly. 
 */
#include "./include/qmap.h"
#include "./include/qidm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <xxhash.h>

#define new(a, n, t)    (t *)alloc(a, n, sizeof(t), _Alignof(t))
#define QMAP_SEED 13
#define QMAP_DEFAULT_MASK 0x7FFF

#define DEBUG_LVL 0

#define DEBUG(lvl, fmt, ...) \
	if (DEBUG_LVL > lvl) \
	fprintf(stderr, fmt, __VA_ARGS__)

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

static qmap_type_t type_unsigned = { .len = sizeof(unsigned), };

#if 0
static inline
unsigned ids_peek(ids_t *list) {
	struct ids_item *top = SLIST_FIRST(list);
	return top ? top->value : QMAP_MISS;
}
#endif

#if 0
static inline
struct ids_item *ids_iter(ids_t *list) {
	return SLIST_FIRST(list);
}

static inline
struct ids_item *ids_next(unsigned *id, struct ids_item *last) {
	*id = last->value;
	return SLIST_NEXT(last, entry);
}

#endif

unsigned idm_new(idm_t *idm) {
	unsigned ret = ids_pop(&idm->free);

	if (ret == (unsigned) -1)
		return idm->last++;

	return ret;
}

idm_t idm_init(void) {
	idm_t idm;
	idm.free = ids_init();
	idm.last = 0;
	return idm;
}

int idm_del(idm_t *idm, unsigned id) {
	if (idm->last <= id)
		return 1;
	else if (id + 1 == idm->last) {
		idm->last--;
		return 1;
	} else {
		ids_push(&idm->free, id);
		return 0;
	}
}

void qmap_init(void) {
	idm = idm_init();
	cursor_idm = idm_init();
	assoc_hd = qmap_open(
			&type_unsigned,
			&type_unsigned,
			0, QMAP_DUP);
}

static inline
size_t _qmap_len(qmap_type_t *type, void *value) {
	if (type->len)
		return type->len;
	else if (type->measure)
		return type->measure(value);
	else {
		size_t key_len = _qmap_len(type->part[0], value);
		return key_len
			+ _qmap_len(type->part[1], (char *) value + key_len);
	}
}

size_t qmap_len(unsigned hd, void *value, enum qmap_member member) {
	qmap_t *qmap = &qmaps[hd];
	qmap_type_t *type = qmap->type[member];
	return _qmap_len(type, value);
}

unsigned qmap_hash(void *key, size_t key_len) {
	return XXH32(key, key_len, QMAP_SEED);
}

unsigned qmap_nohash(void *key, size_t key_len __attribute__((unused))) {
	return * (unsigned *) key;
}

unsigned _qmap_open(qmap_type_t *key_type,
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

int
qmap_assoc_rhd(void **skey, void *key __attribute__((unused)), void *value) {
	*skey = value;
	return 0;
}

unsigned
qmap_open(qmap_type_t *key_type, qmap_type_t *value_type, unsigned mask, unsigned flags)
{
	qmap_type_t *backup_key_type = key_type;
	int prim_dup = 0;

	unsigned phd = _qmap_open(key_type, value_type, mask, flags);

	if (!(flags & QMAP_TWO_WAY))
		return phd;

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

static inline unsigned qmap_id(unsigned hd, void *key) {
	qmap_t *qmap = &qmaps[hd];
	return qmap->hash(key, qmap_len(hd, key, QMAP_KEY)) & qmap->mask;
}

static inline
void *qmap_val(qmap_t *qmap, enum qmap_member t, unsigned id) {
	return qmap->vmaps[t] + id * qmap->lens[t];
}

static inline
void *qmap_rval(unsigned hd, enum qmap_member t, unsigned id) {
	qmap_t *qmap = &qmaps[hd];
	void *value = qmap_val(qmap, t, id);

	if (!((qmap->flags & QMAP_DUP) && t == QMAP_VALUE)
			&& !(qmap->type[t]->len)) {
		value = * (char **) value;
	}

	return value;
}

static inline
void ___qmap_put(unsigned hd, enum qmap_member t, void *value, unsigned id) {
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

static inline
unsigned _qmap_put(unsigned hd, void *key, void *value) {
	qmap_t *qmap = &qmaps[hd];
	unsigned n = idm_new(&qmap->idm);
	unsigned id;

	if (key)
		id = qmap_id(hd, key);
	else {
		id = n;
		key = &n;
	}

	if (qmap->m <= id) {
		qmap->m = id;
		qmap->m *= 2; // TODO careful here - id can be big
		qmap->map = realloc(qmap->map, qmap->m * sizeof(unsigned));
		qmap->omap = realloc(qmap->omap, qmap->m * sizeof(unsigned));
		qmap->vmaps[QMAP_KEY] = realloc(qmap->vmaps[QMAP_KEY], qmap->m * qmap->lens[QMAP_KEY]);
		qmap->vmaps[QMAP_VALUE] = realloc(qmap->vmaps[QMAP_VALUE], qmap->m * qmap->lens[QMAP_VALUE]);
	}

	___qmap_put(hd, QMAP_KEY, key, id);
	___qmap_put(hd, QMAP_VALUE, value, id);

	qmap->omap[n] = id;
	qmap->map[id] = n;
	DEBUG(4, "%u's _put %u %u\n", hd, id, n);

	return id;
}

unsigned
__qmap_put(unsigned hd, void *key, void *value) {
	qmap_t *qmap = &qmaps[hd], *iqmap;
	unsigned in_hd, id, n;

	if (!(qmap->flags & QMAP_DUP))
		return _qmap_put(hd, key, value);

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

		_qmap_put(hd, key, &in_hd);
	} else
		in_hd = * (unsigned *)
			qmap_rval(hd, QMAP_VALUE, id);

	return _qmap_put(in_hd, key, value);
}

int
qmap_twin_assoc(void **skey, void *key, void *value __attribute__((unused))) {
	*skey = key;
	return 0;
}

void
qmap_assoc(unsigned hd, unsigned link, qmap_assoc_t cb)
{
	qmap_t *qmap = &qmaps[hd];

	if (!cb)
		cb = qmap_twin_assoc;

	__qmap_put(assoc_hd, &link, &hd);

	qmap->assoc = cb;
	qmap->phd = link;
}

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
			ret = __qmap_put(hd, buf, value);
			goto proceed;
		}

	}
#endif

	ret = __qmap_put(hd, key, value);
	if (!key)
		key = &ret;
proceed:

	cur = qmap_iter(assoc_hd, &hd);
	while (qmap_lnext(cur)) {
		qmap_t *aqmap;
		void *skey;

		qmap_cget(&linked_hd, cur, QMAP_VALUE);
		aqmap = &qmaps[linked_hd];
		aqmap->assoc(&skey, key, value);

		__qmap_put(linked_hd, skey, value);
	}

	return ret;
}

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

#if 1
	if (pid == QMAP_MISS)
		return 1;
#endif

	value = qmap_rval(qmap->phd, QMAP_KEY, pid);
	len = qmap_len(qmap->phd, value, QMAP_KEY);
	memcpy(target, value, len);
	return 0;
}

void qmap_fin(unsigned cur_id) {
	qmap_cur_t *cursor = &qmap_cursors[cur_id];
	if (cursor->sub_cur)
		qmap_fin(cursor->sub_cur);
	idm_del(&cursor_idm, cur_id);
}

int qmap_get(unsigned hd, void *value, void *key)
{
	qmap_t *qmap = &qmaps[hd];
	unsigned cur_id;

	if (qmap->flags & QMAP_PGET)
		return qmap_pget(hd, value, key);

	cur_id = qmap_iter(hd, key);

	if (!qmap_lnext(cur_id))
		return 1;

	qmap_cget(value, cur_id, QMAP_VALUE);
	qmap_fin(cur_id);
	return 0;
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

static inline
int _qmap_next_dup(unsigned cur_id, unsigned id)
{
	qmap_cur_t *cursor = &qmap_cursors[cur_id];
	unsigned in_hd = * (unsigned *)
		qmap_rval(cursor->hd, QMAP_VALUE, id);

	DEBUG(4, "next_dup %u\n", in_hd);

	if (!cursor->sub_cur)
		cursor->sub_cur = qmap_iter(in_hd, cursor->key);

	return qmap_lnext(cursor->sub_cur);
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
		if (_qmap_next_dup(cur_id, id))
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

void qmap_drop(unsigned hd) {
	unsigned cur_id = qmap_iter(hd, NULL);

	while (qmap_lnext(cur_id))
		qmap_cdel(cur_id);
}

static inline unsigned
qmap_low_cur(unsigned cur_id) {
	register qmap_cur_t *cursor = &qmap_cursors[cur_id];

	if (!cursor->sub_cur)
		return cur_id;
	else
		return qmap_low_cur(cursor->sub_cur);
}

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

static inline void *
qmap_cval(qmap_cur_t *cursor, enum qmap_member t) {
	register qmap_t *qmap = &qmaps[cursor->hd];
	unsigned n = cursor->position - 1,
		 id = qmap->omap[n];
	void *value = qmap_val(qmap, t, id);

	if (!(qmap->type[t]->len))
		value = * (void **) value;

	return value;
}

static inline int
qmap_ccmp(unsigned cur_id, enum qmap_member t, void *cmp)
{
	register qmap_cur_t *cursor
		= &qmap_cursors[qmap_low_cur(cur_id)];
	void *value = qmap_cval(cursor, t);
	size_t len = qmap_len(cursor->hd, value, t);
	return memcmp(value, cmp, len);
}

static inline
unsigned qmap_n(unsigned hd, unsigned id) {
	qmap_t *qmap = &qmaps[hd];
	return qmaps[qmap->tophd].map[id];
}

static inline
void *qmap_csget(qmap_cur_t *cursor, enum qmap_member t)
{
	qmap_t *qmap = &qmaps[cursor->hd];

	if ((qmap->flags & QMAP_PGET) && t == QMAP_VALUE)
	{
		void *key = qmap_csget(cursor, QMAP_KEY);
		unsigned id = qmap_id(cursor->hd, key), n, pid;
		qmap_t *pqmap;

		n = qmap_n(cursor->hd, id);
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

void
qmap_close(unsigned hd) {
	qmap_t *qmap = &qmaps[hd];

	if (qmap->flags & QMAP_TWO_WAY) {
		qmap_close(hd + 1);
		if (qmap->flags & QMAP_DUP) {
			qmap_close(hd + 2);
			free(qmap->type[QMAP_KEY]);
		}
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

void qmap_print(char *target, unsigned hd,
		unsigned type, void *thing)
{
	qmap_t *qmap = &qmaps[hd];
	if (qmap->flags & QMAP_TWO_WAY)
		hd += 1;
	qmap->type[type]->print(target, thing);
}
