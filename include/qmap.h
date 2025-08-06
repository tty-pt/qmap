#ifndef QMAP_H
#define QMAP_H

#include <stddef.h>

enum qmap_flags {
	QMAP_HASH = 1,
	QMAP_AINDEX = 2,
};

typedef unsigned qmap_hash_t(void *key, size_t key_len);

typedef struct {
	unsigned *map, *omap;
	char *vmap;
	size_t value_len;
	unsigned m, n, mask, flags;
	qmap_hash_t *hash;
} qmap_t;

qmap_t qmap_new(size_t value_len, unsigned mask, unsigned flags);

void qmap_put(qmap_t *qmap, void *key, size_t key_len, void *value);
void qmap_del(qmap_t *qmap, void *key, size_t key_len);
void *qmap_get(qmap_t *qmap, void *key, size_t key_len);

void *qmap_nth(qmap_t *qmap, unsigned n);

#endif
