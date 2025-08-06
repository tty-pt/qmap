#include "./include/qmap.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <xxhash.h>

#define new(a, n, t)    (t *)alloc(a, n, sizeof(t), _Alignof(t))
#define QMAP_INITIAL_LEN 16
#define QMAP_SEED 13
#define QMAP_DEFAULT_MASK 0x7FFF

unsigned qmap_hash(void *key, size_t key_len) {
	return XXH32(key, key_len, QMAP_SEED);
}

unsigned qmap_nohash(void *key, size_t key_len __attribute__((unused))) {
	return * (unsigned *) key;
}

qmap_t qmap_new(qmap_type_t *key_type, qmap_type_t *value_type,
		unsigned mask, unsigned flags)
{
	unsigned len;
	size_t keys_len, values_len;
	qmap_t qmap;

	if (value_type->len) {
		qmap.value_len = value_type->len;
		qmap.hash = qmap_nohash;
	} else {
		qmap.value_len = sizeof(char *); // allow variable len
		flags |= QMAP_HASH;
		qmap.hash = qmap_hash;
	}

	mask = mask ? mask : QMAP_DEFAULT_MASK;
	len = ((unsigned) -1) & mask;
	keys_len = len * sizeof(unsigned);
	values_len = len * qmap.value_len;

	qmap.map = malloc(keys_len);
	qmap.omap = malloc(keys_len);
	qmap.vmap = malloc(values_len);
	qmap.m = len;
	qmap.n = 0;
	qmap.type[QMAP_KEY] = key_type;
	qmap.type[QMAP_VALUE] = value_type;
	qmap.mask = mask;
	qmap.flags = flags;

	memset(qmap.map, 0, keys_len);
	memset(qmap.omap, 0, keys_len);
	memset(qmap.vmap, 0, values_len);

	return qmap;
}

static inline unsigned qmap_id(qmap_t *qmap, void *key) {
	return qmap->hash(key, qmap_len(qmap, key, QMAP_KEY)) & qmap->mask;
}

static inline
void *qmap_value(qmap_t *qmap, unsigned id) {
	return qmap->vmap + id * qmap->value_len;
}

void qmap_put(qmap_t *qmap, void *key, void *value) {
	unsigned id;

	if (key)
		id = qmap_id(qmap, key);
	else {
		assert((qmap->flags & QMAP_AINDEX));
		id = qmap->n;
	}

	if (qmap->m <= id) {
		qmap->m = id;
		qmap->m *= 2; // TODO careful here - id can be big
		qmap->map = realloc(qmap->map, qmap->m * sizeof(unsigned));
		qmap->omap = realloc(qmap->omap, qmap->m * sizeof(unsigned));
		qmap->vmap = realloc(qmap->vmap, qmap->m * qmap->value_len);
	}

	unsigned n = qmap->map[id];
	void *real_value = value;

	if (!(qmap->type[QMAP_VALUE]->len || (qmap->flags & QMAP_NO_ALLOC))) {
		size_t real_len = qmap_len(qmap, value, QMAP_VALUE);
		real_value = malloc(real_len);
		memcpy(real_value, value, real_len);
	}

	value = qmap_value(qmap, id);

	if (qmap->n > n && qmap->omap[n] == id) {
		memcpy(value, real_value, qmap->value_len);
		return;
	}

	qmap->omap[qmap->n] = id;
	qmap->map[id] = qmap->n;
	memcpy(value, real_value, qmap->value_len);
	qmap->n++;
}

void qmap_del(qmap_t *qmap, void *key) {
	unsigned n, id = qmap_id(qmap, key);

	if (qmap->m < id)
		return; // not present

	n = qmap->map[id];

	if (n > qmap->n)
		return; // not present

	if (!(qmap->type[QMAP_VALUE]->len || (qmap->flags & QMAP_NO_ALLOC))) {
		void *value = qmap->vmap + id * qmap->value_len;
		free(value);
	}

	memmove(&qmap->omap[n], &qmap->omap[n + 1],
			(qmap->n - n - 1) * sizeof(unsigned));

	qmap->map[id] = qmap->n;
	qmap->n--;
	for (; n < qmap->n; n++) {
		unsigned id = qmap->omap[n];
		qmap->map[id] = n;
	}
}

int qmap_get(qmap_t *qmap, void *destiny, void *key) {
	unsigned n, id = qmap_id(qmap, key);

	if (!qmap->n || id >= qmap->m)
		return 1;

	n = qmap->map[id];

	if (n >= qmap->n || qmap->omap[n] != id)
		return 1;

	void *value = qmap_value(qmap, id);
	memcpy(destiny, value, qmap_len(qmap, value, QMAP_VALUE));

	return 0;
}

int qmap_nth(qmap_t *qmap, void *destiny, unsigned n) {
	if (n >= qmap->n)
		return 1;

	unsigned id = qmap->omap[n];
	void *value = qmap_value(qmap, id);
	memcpy(destiny, value, qmap_len(qmap, value, QMAP_VALUE));
	return 0;
}
