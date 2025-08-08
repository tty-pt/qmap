#ifndef QMAP_H
#define QMAP_H

#include <stddef.h>

#define QMAP_MAX 1024
#define QMAP_MAX_COMBINED_LEN (BUFSIZ * 2)
#define QMAP_MISS ((unsigned) -1)

enum qmap_member {
	QMAP_KEY = 0,
	QMAP_VALUE = 1,
};

enum qmap_flags {
	QMAP_DUP = 1,
	QMAP_AINDEX = 2,
	QMAP_PGET = 4,
	QMAP_TWO_WAY = 8,
};

typedef size_t qmap_measure_t(void *value);
typedef int qmap_compare_t(void *a, void *b, size_t len);
typedef int qmap_print_t(char *target, void *value);

typedef struct qmap_type {
	size_t len; // length if fixed
	qmap_measure_t *measure;   // for variable length
	qmap_compare_t *compare;   // only used in test
	qmap_print_t   *print;     // same
	struct qmap_type *part[2]; // for composed types
} qmap_type_t;

typedef int qmap_assoc_t(void **data, void *key, void *value);

void qmap_init(void);
unsigned qmap_open(qmap_type_t *key_type,
		qmap_type_t *value_type,
		unsigned mask, unsigned flags);
void qmap_close(unsigned hd);

int qmap_get(unsigned hd, void *destiny, void *key);
unsigned qmap_put(unsigned hd, void *key, void *value);
void qmap_del(unsigned hd, void *key, void *value);
void qmap_drop(unsigned hd);

unsigned qmap_iter(unsigned hd, void *key);
int qmap_next(void *key, void *value, unsigned cur_id);
void qmap_cdel(unsigned cur_id);

void qmap_assoc(unsigned hd, unsigned link, qmap_assoc_t cb);
size_t qmap_len(unsigned hd, void *value, enum qmap_member member);
void qmap_print(char *target, unsigned hd,
		unsigned type, void *thing);

#endif
