#include "./include/qmap.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <xxhash.h>

#define new(a, n, t)    (t *)alloc(a, n, sizeof(t), _Alignof(t))
#define QMAP_INITIAL_LEN 16
#define QMAP_SEED 13
#define QMAP_DEFAULT_MASK 0xFFFF

unsigned qmap_hash(void *key, size_t key_len) {
	return XXH32(key, key_len, QMAP_SEED);
}

unsigned qmap_nohash(void *key, size_t key_len __attribute__((unused))) {
	return * (unsigned *) key;
}

qmap_t qmap_new(size_t value_len, unsigned mask, unsigned flags) {
	unsigned len = ((unsigned) -1) | mask;

	size_t keys_len = len * sizeof(unsigned),
	       values_len = len * value_len;

	qmap_t qmap = {
		.map = malloc(keys_len),
		.omap = malloc(keys_len),
		.vmap = malloc(values_len),
		.m = len,
		.n = 0,
		.value_len = value_len,
		.mask = mask ? mask : QMAP_DEFAULT_MASK,
		.flags = flags,
	};

	memset(qmap.map, 0, keys_len);
	memset(qmap.omap, 0, keys_len);
	memset(qmap.vmap, 0, values_len);

	qmap.hash = flags & QMAP_HASH ? qmap_hash : qmap_nohash;

	return qmap;
}

static inline unsigned qmap_id(qmap_t *qmap, void *key, size_t key_len) {
	return qmap->hash(key, key_len) | qmap->mask;
}

void qmap_put(qmap_t *qmap, void *key, size_t key_len, void *value) {
	unsigned id;

	if (key)
		id = qmap_id(qmap, key, key_len);
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

	if (qmap->n > n && qmap->omap[n] == id) {
		memcpy(qmap->vmap + id * qmap->value_len, value, qmap->value_len);
		return;
	}

	qmap->omap[qmap->n] = id;
	qmap->map[id] = qmap->n;
	memcpy(qmap->vmap + id * qmap->value_len, value, qmap->value_len);
	qmap->n++;
}

void qmap_del(qmap_t *qmap, void *key, size_t key_len) {
	unsigned n, id = qmap_id(qmap, key, key_len);

	if (qmap->m < id)
		return; // not present

	n = qmap->map[id];

	if (n > qmap->n)
		return; // not present

	memmove(&qmap->omap[n], &qmap->omap[n + 1],
			(qmap->n - n - 1) * sizeof(unsigned));

	qmap->map[id] = qmap->n;
	qmap->n--;
	for (; n < qmap->n; n++) {
		unsigned id = qmap->omap[n];
		qmap->map[id] = n;
	}
}

void *qmap_get(qmap_t *qmap, void *key, size_t key_len) {
	unsigned n, id = qmap_id(qmap, key, key_len);

	if (!qmap->n || id >= qmap->m)
		return NULL;

	n = qmap->map[id];

	if (n >= qmap->n || qmap->omap[n] != id)
		return NULL;

	return qmap->vmap + id * qmap->value_len;
}

void *qmap_nth(qmap_t *qmap, unsigned n) {
	if (n >= qmap->n)
		return NULL;

	unsigned id = qmap->omap[n];
	return qmap->vmap + id * qmap->value_len;
}
