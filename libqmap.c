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

typedef unsigned qmap_hash_t(void *key, size_t key_len);

typedef struct {
	// these have to do with keys
	unsigned *map,  // id -> n
		 *omap; // n -> id

	// this is for values
	char *vmap;     // id -> value

	qmap_type_t *type[2];
	unsigned m, mask, flags;
	size_t value_len;
	qmap_hash_t *hash;
	idm_t idm;

	unsigned phd;
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
	if (id + 1 == idm->last) {
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
	size_t keys_len, values_len;

#if QMAP_DEBUG
	fprintf(stderr, "_open %u %p %p %u %u\n",
			hd, (void *) key_type,
			(void *) value_type, mask, flags);
#endif

	if (value_type->len) {
		qmap->value_len = value_type->len;
	} else
		qmap->value_len = sizeof(char *); // allow variable len

	qmap->hash = qmap_hash;
	if (key_type->len == sizeof(unsigned))
		qmap->hash = qmap_nohash;

	mask = mask ? mask : QMAP_DEFAULT_MASK;
	len = ((unsigned) -1) & mask;
	keys_len = len * sizeof(unsigned);
	values_len = len * qmap->value_len;

	qmap->map = malloc(keys_len);
	qmap->omap = malloc(keys_len);
	qmap->vmap = malloc(values_len);
	qmap->m = len;
	qmap->type[QMAP_KEY] = key_type;
	qmap->type[QMAP_VALUE] = value_type;
	qmap->mask = mask;
	qmap->flags = flags;
	qmap->idm = idm_init();

	memset(qmap->map, 0, keys_len);
	memset(qmap->omap, 0, keys_len);
	memset(qmap->vmap, 0, values_len);

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

	if (!(flags & QMAP_TWO_WAY))
		return _qmap_open(key_type, value_type, mask, flags);

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

	unsigned phd = _qmap_open(key_type, value_type, mask, flags);

	flags &= ~(QMAP_TWO_WAY | QMAP_AINDEX);
	flags |= QMAP_DUP;

	key_type = backup_key_type;

	_qmap_open(value_type, value_type, mask, flags | QMAP_PGET);
	qmap_assoc(phd + 1, phd, qmap_assoc_rhd);

	if (prim_dup) {
		_qmap_open(key_type, key_type, mask, flags);
		qmap_assoc(phd + 2, phd, NULL);
	}

	return phd;
}

static inline unsigned qmap_id(unsigned hd, void *key) {
	qmap_t *qmap = &qmaps[hd];
	return qmap->hash(key, qmap_len(hd, key, QMAP_KEY)) & qmap->mask;
}

static inline
void *qmap_value(qmap_t *qmap, unsigned id) {
	return qmap->vmap + id * qmap->value_len;
}

static inline
unsigned _qmap_put(unsigned hd, void *key, void *value) {
	qmap_t *qmap = &qmaps[hd];
	unsigned id;
	unsigned n = idm_new(&qmap->idm);

	if (key)
		id = qmap_id(hd, key);
	else
		id = n;

	if (qmap->m <= id) {
		qmap->m = id;
		qmap->m *= 2; // TODO careful here - id can be big
		qmap->map = realloc(qmap->map, qmap->m * sizeof(unsigned));
		qmap->omap = realloc(qmap->omap, qmap->m * sizeof(unsigned));
		qmap->vmap = realloc(qmap->vmap, qmap->m * qmap->value_len);
	}

	void *real_value = value, *tmp = value;

	if (!(qmap->type[QMAP_VALUE]->len)) {
		size_t real_len = qmap_len(hd, value, QMAP_VALUE);
		tmp = malloc(real_len);
		memcpy(tmp, value, real_len);
		real_value = &tmp;
	}

	value = qmap_value(qmap, id);

	qmap->omap[n] = id;
	qmap->map[id] = n;

	memcpy(value, real_value, qmap->value_len);
	return id;
}

static inline
int _qmap_get(unsigned hd, void *destiny, void *key) {
	qmap_t *qmap = &qmaps[hd];
	unsigned n, id = qmap_id(hd, key);

	if (id >= qmap->m)
		return 1;

	n = qmap->map[id];

	if (n >= qmap->idm.last || qmap->omap[n] != id)
		return 1;

	void *value = qmap_value(qmap, id);
	if (!(qmap->type[QMAP_VALUE]->len))
		value = * (char **) value;
	memcpy(destiny, value, qmap_len(hd, value, QMAP_VALUE));

	return 0;
}

unsigned
__qmap_put(unsigned hd, void *key_r, void *value) {
	qmap_t *qmap = &qmaps[hd];

	if (!(qmap->flags & QMAP_DUP))
		return _qmap_put(hd, key_r, value);

	unsigned in_hd;
	if (_qmap_get(hd, &in_hd, key_r)) {
		in_hd = qmap_open(
				&type_unsigned,
				qmap->type[QMAP_VALUE],
				0, 0);
		_qmap_put(in_hd, &in_hd, value);
		return _qmap_put(hd, key_r, &in_hd);
	}

	return _qmap_put(hd, value, value);
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
	qmap_t *qmap = &qmaps[hd];
	size_t key_len, value_len;
	unsigned flags = qmap->flags;
	unsigned ret, cur, linked_hd;

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

	ret =__qmap_put(hd, key, value);
proceed:

	cur = qmap_iter(assoc_hd, &hd);
	while (qmap_next(&hd, &linked_hd, cur)) {
		qmap_t *aqmap = &qmaps[linked_hd];
		void *skey;
		aqmap->assoc(&skey, key, value);
		_qmap_put(linked_hd, skey, key);
	}

	return ret;
}

static inline
int qmap_sget(unsigned hd, void *value, void *key) {
	qmap_t *qmap = &qmaps[hd];
	char pkey[QMAP_MAX_COMBINED_LEN];
	if (_qmap_get(hd, pkey, key))
		return 1;
	return _qmap_get(qmap->phd, value, pkey);
}

int qmap_get(unsigned hd, void *value, void *key)
{
	qmap_t *qmap = &qmaps[hd];

	if (qmap->assoc && !(qmap->flags & QMAP_PGET))
		return qmap_sget(hd, value, key);

	return _qmap_get(hd, value, key);
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

int qmap_next(void *key, void *value, unsigned cur_id);

static inline
int _qmap_next_dup(void *value, unsigned cur_id)
{
	qmap_cur_t *cursor = &qmap_cursors[cur_id];
	unsigned in_hd;

	_qmap_get(cursor->hd, &in_hd, cursor->key);

	if (!cursor->sub_cur)
		cursor->sub_cur = qmap_iter(in_hd, NULL);

	if (qmap_next(&in_hd, value, cursor->sub_cur))
		return 1;

	return 0;
}

int qmap_next(void *key, void *value, unsigned cur_id)
{
	register qmap_cur_t *cursor = &qmap_cursors[cur_id];
	register qmap_t *qmap = &qmaps[cursor->hd];
	void *inner_value;
	size_t value_len;
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

	if (cursor->key) {
		if (!(id == * (unsigned *) cursor->key
				&& (qmap->flags & QMAP_DUP)
				&& _qmap_next_dup(value, cur_id)))
			goto end;

		return 1;
	}

	if (qmap->flags & QMAP_DUP) {
		if (_qmap_next_dup(value, cur_id))
			return 1;

		cursor->position++;
		cursor->sub_cur = 0;
		goto cagain;
	}

	inner_value = qmap_value(qmap, id);
	memcpy(key, &id, sizeof(id));

	if (!qmap->type[QMAP_VALUE]->len)
		inner_value = * (void **) inner_value;

	value_len = qmap_len(cursor->hd,
			inner_value, QMAP_VALUE);

	memcpy(value, inner_value, value_len);
	cursor->position++;
	return 1;
end:
	idm_del(&cursor_idm, cur_id);
	return 0;
}

void _qmap_del(unsigned hd, void *key) {
	qmap_t *qmap = &qmaps[hd];
	unsigned n, id = qmap_id(hd, key);

	if (qmap->m < id)
		return; // not present

	n = qmap->map[id];

	if (n > qmap->idm.last)
		return; // not present

	if (!(qmap->type[QMAP_VALUE]->len)) {
		void *value = qmap->vmap + id * qmap->value_len;
		free(* (void **) value);
	}

	idm_del(&qmap->idm, n);
	qmap->map[id] = QMAP_MISS;
	qmap->omap[n] = QMAP_MISS;
}

void qmap_del(unsigned hd, void *key, void *value)
{
	qmap_t *qmap = &qmaps[hd];

	if (!(qmap->flags & QMAP_DUP)) {
		_qmap_del(hd, key); 
		return;
	}

	unsigned in_hd;
	if (_qmap_get(hd, &in_hd, key))
		return; // not present

	_qmap_del(in_hd, value);
}

void qmap_cdel(unsigned cur_id)
{
	register qmap_cur_t *cursor = &qmap_cursors[cur_id];
	register qmap_t *qmap = &qmaps[cursor->hd];
	unsigned n, id;
	n = cursor->position;
	id = qmap->omap[n];

	if (!(qmap->type[QMAP_VALUE]->len)) {
		void *value = qmap->vmap + id * qmap->value_len;
		free(* (void **) value);
	}

	idm_del(&qmap->idm, n);
	qmap->map[id] = QMAP_MISS;
	qmap->omap[n] = QMAP_MISS;
}

void qmap_drop(unsigned hd) {
	unsigned cur_id = qmap_iter(hd, NULL);
	char key[QMAP_MAX_COMBINED_LEN];
	char value[QMAP_MAX_COMBINED_LEN];

	while (qmap_next(key, value, cur_id))
		qmap_del(hd, key, value);
}

void
qmap_close(unsigned hd) {
	qmap_t *qmap = &qmaps[hd];
	qmap_drop(hd);
	if ((qmap->flags & QMAP_TWO_WAY)
			&& (qmap->flags & QMAP_DUP))
		free(qmap->type[QMAP_KEY]);
	ids_drop(&qmap->idm.free);
	qmap->idm.last = 0;
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
