#include "./../include/qmap.h"

#include <stdio.h>
#include <string.h>
#include <qsys.h>

char *good = "✅";
char *bad = "❌";

unsigned errors = 0;

typedef struct {
	char *key, *value;
} dbtype_t;

enum dbtype {
	UTOS,
	STOU,
	UTOU,
	STOS,
};

dbtype_t dbtypes[] = {
	{ "u", "s" },
	{ "s", "u" },
	{ "u", "u" },
	{ "s", "s" },
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
_gen_get(unsigned hd, void *key, void *expects, int reverse)
{
	char ret[MAX_LEN];
	char buf[BUFSIZ];
	char *mark, *rgood = good, *rbad = bad;
	memset(ret, 0, sizeof(ret));

	if (reverse) {
		rgood = bad;
		rbad = good;
	}

	qmap_print(buf, hd, QMAP_KEY, key);
	printf("gen_get_test(%u, %s, ", hd, buf);
	qmap_print(buf, hd, QMAP_VALUE, expects);
	printf("%s) = ", buf);

	if (qmap_get(hd, ret, key)) {
		printf("-1 %s\n", rbad);
		return !reverse;
	}

	if (expects || !(qmap_flags(hd) & QMAP_DUP))
		mark = qmap_cmp(hd, QMAP_VALUE, ret, expects)
			? rbad : rgood;
	else
		mark = rbad;

	memset(buf, 0, sizeof(buf));
	qmap_print(buf, hd, QMAP_VALUE, ret);

	printf("%s %s\n", buf, mark);
	return reverse;
}

static inline int
gen_get(unsigned hd, void *key, void *expects)
{
	int ret = _gen_get(hd, key, expects, 0);
	errors += ret;
	return ret;
}

static inline void
gen_put(unsigned hd, void *key, void *value)
{
	unsigned akey = qmap_put(hd, key, value);
	if (!key)
		key = &akey;
	gen_get(hd, key, value);
}

static inline int
gen_del(unsigned hd, void *key, void *value)
{
	int ret;
	qmap_del(hd, key, value);
	ret = _gen_get(hd, key, value, 1);
	errors += ret;
	return ret;
}

static inline void
test_first(void)
{
	unsigned hd = gen_open(UTOS, 0);

	unsigned keys[] = { 3, 5 };

	gen_put(hd, &keys[0], "hello");
	gen_put(hd, &keys[1], "hi");

	gen_del(hd, &keys[0], "hello");

	qmap_close(hd);
}

static inline void
test_second(void)
{
	unsigned hd = gen_open(STOU, 0);
	unsigned values[] = { 9, 7 };

	gen_put(hd, "hello", &values[0]);
	gen_put(hd, "hi", &values[1]);

	qmap_close(hd);
}

static inline void
test_third(void)
{
	unsigned hd = gen_open(UTOU, 0), cur_id;
	unsigned keys[] = { 3, 9 };
	unsigned values[] = { 5, 7 };
	unsigned key, value;

	gen_put(hd, &keys[0], &values[0]);
	gen_put(hd, &keys[1], &values[1]);

	cur_id = qmap_iter(hd, NULL);
	while (qmap_next(&key, &value, cur_id))
		printf("ITER '%u' - '%u'\n", key, value);

	qmap_close(hd);
}

static inline void
test_fourth(void)
{
	unsigned hd = gen_open(UTOS, QMAP_TWO_WAY);
	unsigned keys[] = { 3, 9 };

	gen_put(hd, &keys[0], "hello");
	gen_put(hd, &keys[1], "hi");

	type_cache[hd + 1] = STOU;
	gen_get(hd + 1, "hello", &keys[0]);
	gen_get(hd + 1, "hi", &keys[1]);

	qmap_close(hd);
}

static inline void
test_fifth(void)
{
	unsigned hd = gen_open(STOU, QMAP_TWO_WAY);
	unsigned values[] = { 3, 9 };

	gen_put(hd, "hello", &values[0]);
	gen_put(hd, "hi", &values[1]);
	fprintf(stderr, "fifth two-way\n");

	type_cache[hd + 1] = UTOS;
	gen_get(hd + 1, &values[0], "hello");
	gen_get(hd + 1, &values[1], "hi");

	qmap_close(hd);
}

static inline void
test_sixth(void)
{
	unsigned hd = gen_open(STOS, QMAP_TWO_WAY);

	gen_put(hd, "hello", "hellov");
	gen_put(hd, "hi", "hiv");

	type_cache[hd + 1] = STOS;
	gen_get(hd + 1, "hellov", "hello");
	gen_get(hd + 1, "hiv", "hi");

	qmap_close(hd);
}


static inline void
test_seventh(void)
{
	unsigned hd = gen_open(STOS, QMAP_TWO_WAY), cur_id;
	char key[MAX_LEN];
	char value[MAX_LEN];

	gen_put(hd, "hello", "olleh");
	gen_put(hd, "hi", "ih");
	gen_put(hd, "ola", "alo");

	cur_id = qmap_iter(hd, NULL);
	while (qmap_next(key, value, cur_id))
		printf("ITER '%s' - '%s'\n", key, value);

	fprintf(stderr, "keyed iter\n");
	cur_id = qmap_iter(hd, "hello");
	while (qmap_next(key, value, cur_id))
		printf("ITER '%s' - '%s'\n", key, value);

	gen_del(hd + 1, "alo", NULL);
	gen_get(hd + 1, "olleh", "hello");
	gen_get(hd + 1, "ih", "hi");

	fprintf(stderr, "reverse iter\n");
	cur_id = qmap_iter(hd + 1, NULL);
	while (qmap_next(key, value, cur_id))
		printf("ITER '%s' - '%s'\n", key, value);

	fprintf(stderr, "reverse keyed iter\n");
	cur_id = qmap_iter(hd + 1, "ih");
	while (qmap_next(key, value, cur_id))
		printf("ITER '%s' - '%s'\n", key, value);

	qmap_close(hd);
}

static inline
void test_eighth(void)
{
	unsigned cur_id, key;
	unsigned hd = gen_open(UTOS,
			QMAP_AINDEX | QMAP_TWO_WAY);
	char value[MAX_LEN];

	gen_put(hd, NULL, "hello");
	gen_put(hd, NULL, "hi");
	gen_put(hd, NULL, "ola");

	cur_id = qmap_iter(hd, NULL);
	while (qmap_next(&key, value, cur_id))
		printf("ITER '%u' - '%s'\n", key, value);

	fprintf(stderr, "reversed\n");
	cur_id = qmap_iter(hd + 1, NULL);
	while (qmap_next(value, &key, cur_id))
		printf("ITER '%s' - '%u'\n", value, key);

	qmap_close(hd);
}

static inline
void test_nineth(void)
{
	unsigned cur_id, key;
	unsigned hd = gen_open(UTOS, QMAP_DUP);
	unsigned keys[] = { 3, 3, 2 };
	char value[MAX_LEN];

	gen_put(hd, &keys[0], "hello");
	qmap_put(hd, &keys[1], "hi");
	gen_put(hd, &keys[2], "ola");

	cur_id = qmap_iter(hd, NULL);
	while (qmap_next(&key, value, cur_id))
		printf("ITER '%u' - '%s'\n", key, value);

	WARN("Keyed iter\n");
	cur_id = qmap_iter(hd, &keys[0]);
	while (qmap_next(&key, value, cur_id))
		printf("ITER '%u' - '%s'\n", key, value);

	gen_del(hd, &keys[0], NULL);
	WARN("After del keyed iter\n");
	cur_id = qmap_iter(hd, &keys[0]);
	while (qmap_next(&key, value, cur_id))
		printf("ITER '%u' - '%s'\n", key, value);
	WARN("After del unkeyed\n");
	cur_id = qmap_iter(hd, NULL);
	while (qmap_next(&key, value, cur_id))
		printf("ITER '%u' - '%s'\n", key, value);

	qmap_close(hd);
}

int main(void) {
	qmap_init();

	test_first();
	fprintf(stderr, "second\n");
	test_second();
	fprintf(stderr, "third\n");
	test_third();
	fprintf(stderr, "fourth\n");
	test_fourth();
	fprintf(stderr, "fifth\n");
	test_fifth();
	fprintf(stderr, "sixth\n");
	test_sixth();
	fprintf(stderr, "seventh\n");
	test_seventh();
	fprintf(stderr, "eighth\n");
	test_eighth();

	return -errors;
}
