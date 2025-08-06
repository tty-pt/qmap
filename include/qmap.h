#ifndef QMAP_H
#define QMAP_H

#include <stddef.h>

enum qmap_member {
	QMAP_KEY = 0,
	QMAP_VALUE = 1,
};

enum qmap_flags {
	QMAP_HASH = 1,
	QMAP_PTR = 2,
	QMAP_NO_ALLOC = 4,
	QMAP_AINDEX = 8,
};

typedef unsigned qmap_hash_t(void *key, size_t key_len);
typedef size_t qmap_measure_t(void *value);

typedef struct {
	size_t len;
	qmap_measure_t *measure;
} qmap_type_t;

// FIXME internal. How about I hide this?
typedef struct {
	unsigned *map, *omap;
	char *vmap;
	qmap_type_t *type[2];
	unsigned m, n, mask, flags;
	size_t value_len;
	qmap_hash_t *hash;
} qmap_t;

static inline size_t qmap_len(qmap_t *qmap, void *value, enum qmap_member member) {
	qmap_type_t *type = qmap->type[member];
	return type->len ? type->len : type->measure(value);
}

qmap_t qmap_new(qmap_type_t *key_type, qmap_type_t *value_type,
		unsigned mask, unsigned flags);

void qmap_put(qmap_t *qmap, void *key, void *value);
void qmap_del(qmap_t *qmap, void *key);
int qmap_get(qmap_t *qmap, void *destiny, void *key);

int qmap_nth(qmap_t *qmap, void *destiny, unsigned n);

#endif
