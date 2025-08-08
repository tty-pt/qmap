#include "./include/qmap.h"

#include <stdio.h>
#include <string.h>

char *good = "✅";
char *bad = "❌";

unsigned errors = 0;

size_t string_measure(void *value) {
	return strlen(value) + 1;
}

int string_compare(void *a, void *b, size_t _len __attribute__((unused))) {
	return strcmp(a, b);
}

int string_print(char *target, void *value) {
	return sprintf(target, "%s", (char *) value);
}

int other_compare(void *a, void *b, size_t len) {
	return memcmp(a, b, len);
}

int unsigned_print(char *target, void *value) {
	return sprintf(target, "%u", * (unsigned *) value);
}

qmap_type_t type_string = {
	.measure = string_measure,
	.compare = string_compare,
	.print   = string_print,
}, type_unsigned = {
	.len     = sizeof(unsigned),
	.compare = other_compare,
	.print   = unsigned_print,
};

typedef struct {
	qmap_type_t *key;
	qmap_type_t *value;
} dbtype_t;

enum dbtype {
	UTOS,
	STOU,
	UTOU,
	STOS,
};

dbtype_t dbtypes[] = {
	{
		&type_unsigned,
		&type_string,
	}, {
		&type_string,
		&type_unsigned,
	}, {
		&type_unsigned,
		&type_unsigned,
	}, {
		&type_string,
		&type_string,
	}
};

unsigned type_cache[32];

unsigned gen_open(enum dbtype type, unsigned flags) {
	dbtype_t *dbtype = &dbtypes[type];

	unsigned hd = qmap_open(
			dbtype->key,
			dbtype->value,
			0, flags);

	type_cache[hd] = type;

	return hd;
}

typedef struct {
	unsigned key;
	char *value;
} utos_t;

#define MAX_LEN (BUFSIZ * 2)

static int
_gen_get(unsigned hd, void *key, void *expects, int reverse) {
	enum dbtype t = type_cache[hd];
	dbtype_t type = dbtypes[t];
	char ret[MAX_LEN];
	char buf[BUFSIZ];
	char *mark, *rgood = good, *rbad = bad;
	memset(ret, 0, sizeof(ret));

	if (reverse) {
		rgood = bad;
		rbad = good;
	}

	type.key->print(buf, key);
	printf("gen_get_test(%u, %s, ", hd, buf);
	type.value->print(buf, expects);
	printf("%s) = ", buf);

	if (qmap_get(hd, ret, key)) {
		printf("-1 %s\n", rbad);
		return !reverse;
	}

	int cmp = type.value->compare(ret, expects, type.value->len);
	mark = cmp ? rbad : rgood;
	memset(buf, 0, sizeof(buf));
	type.value->print(buf, ret);

	printf("%s %s\n", buf, mark);
	return reverse;
}

static inline int
gen_get(unsigned hd, void *key, void *expects) {
	int ret = _gen_get(hd, key, expects, 0);
	errors += ret;
	return ret;
}

static inline
void gen_put(unsigned hd, void *key, void *value) {
	qmap_put(hd, key, value);
	gen_get(hd, key, value);
}

static inline int
gen_del(unsigned hd, void *key, void *value) {
	int ret;
	qmap_del(hd, key, value);
	ret = _gen_get(hd, key, value, 1);
	errors += ret;
	return ret;
}

static inline
void test_first(void) {
	unsigned hd = gen_open(UTOS, 0);

	unsigned keys[] = { 3, 5 };

	gen_put(hd, &keys[0], "hello");
	gen_put(hd, &keys[1], "hi");

	gen_del(hd, &keys[0], "hello");

	qmap_close(hd);
}

static inline
void test_second(void) {
	unsigned hd = gen_open(STOU, 0);
	unsigned values[] = { 9, 7 };

	gen_put(hd, "hello", &values[0]);
	gen_put(hd, "hi", &values[1]);

	qmap_close(hd);
}

static inline
void test_third(void) {
	unsigned hd = gen_open(UTOU, 0);
	unsigned keys[] = { 3, 9 };
	unsigned values[] = { 5, 7 };

	gen_put(hd, &keys[0], &values[0]);
	gen_put(hd, &keys[1], &values[1]);

	qmap_close(hd);
}

static inline
void test_fourth(void) {
	unsigned hd = gen_open(UTOS, QMAP_TWO_WAY);
	unsigned keys[] = { 3, 9 };

	gen_put(hd, &keys[0], "hello");
	gen_put(hd, &keys[1], "hi");

	type_cache[hd + 1] = STOU;
	gen_get(hd + 1, "hello", &keys[0]);
	gen_get(hd + 1, "hi", &keys[1]);

	qmap_close(hd);
}

static inline
void test_fifth(void) {
	unsigned hd = gen_open(STOU, QMAP_TWO_WAY);
	unsigned values[] = { 3, 9 };

	gen_put(hd, "hello", &values[0]);
	gen_put(hd, "hi", &values[1]);

	type_cache[hd + 1] = UTOS;
	gen_get(hd + 1, &values[0], "hello");
	gen_get(hd + 1, &values[1], "hi");

	qmap_close(hd);
}

static inline
void test_sixth(void) {
	unsigned hd = gen_open(STOS, QMAP_TWO_WAY);

	gen_put(hd, "hello", "hellov");
	gen_put(hd, "hi", "hiv");

	type_cache[hd + 1] = STOS;
	gen_get(hd + 1, "hellov", "hello");
	gen_get(hd + 1, "hiv", "hi");

	qmap_close(hd);
}


static inline
void test_seventh(void) {
	unsigned hd = gen_open(STOS, QMAP_TWO_WAY), cur_id;
	char key[MAX_LEN];
	char value[MAX_LEN];

	gen_put(hd, "hello", "olleh");
	gen_put(hd, "hi", "ih");
	gen_put(hd, "ola", "alo");

	cur_id = qmap_iter(hd, NULL);
	while (qmap_next(key, value, cur_id))
		printf("ITER '%s' - '%s'\n", key, value);

	qmap_close(hd);
}

int main(void) {
	qmap_init();

	test_first();
	test_second();
	test_third();
	test_fourth();
	test_fifth();
	test_sixth();
	test_seventh();

	return -errors;
}
