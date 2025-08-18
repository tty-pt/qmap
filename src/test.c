#include "./../include/qmap.h"

#include <stdio.h>
#include <string.h>

#include <qsys.h>

#define DB_MASK 0xF
#define MAX_LEN (BUFSIZ * 2)

char *good = "✅";
char *bad = "❌";

unsigned errors = 0;

typedef unsigned dbtype_t[2];

enum dbtype {
	UTOS,
	STOU,
	UTOU,
	STOS,
};

dbtype_t dbtypes[] = {
	{ QMAP_WORD, QMAP_DYNA },
	{ QMAP_DYNA, QMAP_WORD },
	{ QMAP_WORD, QMAP_WORD },
	{ QMAP_DYNA, QMAP_DYNA },
};

typedef int print_t(void *key);
typedef int cmp_t(void *a, void *b);

typedef struct {
	print_t *print;
	cmp_t *cmp;
} type_meta_t;

int s_print(void *key) {
	return printf("%s", (char *) key);
}

int u_print(void *key) {
	return printf("%u", * (unsigned *) key);
}

int s_cmp(void *a, void *b) {
	return strcmp(a, b);
}

int u_cmp(void *a, void *b) {
	return (* (unsigned *) b)
		- (* (unsigned *) a);
}

type_meta_t type_meta[] = {
	{ .print = s_print, .cmp = s_cmp, },
	{ .print = u_print, .cmp = u_cmp, }
};

enum qmap_mbr {
	QMAP_KEY = 0,
	QMAP_VALUE = 1,
};

enum meta_flags {
	QMAP_REVERSE = 4,
};

void *values[DB_MASK + 1],
     *keys[DB_MASK + 1];

typedef struct {
	unsigned type, flags;
} hd_meta_t;

hd_meta_t hd_meta[8];

static inline int
rmbr_get(unsigned hd, unsigned mbr)
{
	hd_meta_t *meta = &hd_meta[hd];
	unsigned rmbr = meta->flags & QMAP_REVERSE
		? !mbr : mbr;
	return rmbr;
}

static inline void *
_get(unsigned hd, unsigned mbr, unsigned n)
{
	unsigned rmbr = rmbr_get(hd, mbr);
	
	if (rmbr == QMAP_KEY)
		return keys[n];

	return values[n];
}

static int
type_print(unsigned hd, unsigned mbr, void *key)
{
	hd_meta_t *meta = &hd_meta[hd];
	unsigned rmbr = rmbr_get(hd, mbr);
	unsigned type = dbtypes[meta->type][rmbr];
	return type_meta[type].print(key);
}

static int
type_cmp(unsigned hd, unsigned mbr, void *a, void *b)
{
	hd_meta_t *meta = &hd_meta[hd];
	unsigned rmbr = rmbr_get(hd, mbr);
	unsigned type = dbtypes[meta->type][rmbr];
	return type_meta[type].cmp(a, b);
}

static
unsigned _gen_open(enum dbtype type, unsigned flags) {
	unsigned hd;

	if (flags & QMAP_REVERSE) {
		hd = 1;
	} else {
		dbtype_t *dbtype = &dbtypes[type];
		hd = qmap_open((*dbtype)[QMAP_KEY],
				(*dbtype)[QMAP_VALUE],
				DB_MASK, flags);
	}

	hd_meta[hd].type = type;
	hd_meta[hd].flags = flags;

	return hd;
}

unsigned gen_open(enum dbtype type, unsigned flags) {
	unsigned hd = _gen_open(type, flags);
	if (flags & QMAP_MIRROR)
		_gen_open(type, flags | QMAP_REVERSE);
	return hd;
}

static int
_gen_get(unsigned hd, void *key, void *value, void *expects, int reverse)
{
	char *mark, *rgood = good, *rbad = bad;
	unsigned sn;
	void *svalue;

	if (reverse) {
		rgood = bad;
		rbad = good;
	}

	printf("gen_get_test(%u, ", hd);
	type_print(hd, QMAP_KEY, key);
	printf(", ");
	type_print(hd, QMAP_VALUE, value);
	printf(") = ");

	sn = qmap_get(hd, key);
	if (sn == QMAP_MISS) {
		printf("-1 %s\n", rbad);
		return !reverse;
	}

	svalue = _get(hd, QMAP_VALUE, sn);
	mark = type_cmp(hd, QMAP_VALUE, svalue, expects)
		? rbad : rgood;

	type_print(hd, QMAP_VALUE, svalue);

	printf(" %s\n", mark);
	return reverse;
}

static inline int
gen_get(unsigned hd, void *key, void *expects)
{
	int ret = _gen_get(hd, key, expects, expects, 0);
	errors += ret;
	return ret;
}

static inline unsigned
gen_put(unsigned hd, void *key, void *value)
{
	static unsigned akeys[8];
	static unsigned akey;

	akey = qmap_put(hd, key, value);
	if (!key) {
		akeys[akey] = akey;
		key = &akeys[akey];
	}

	keys[akey] = key;
	values[akey] = value;

	gen_get(hd, key, value);
	return akey;
}

static inline int
gen_del(unsigned hd, void *key, void *value)
{
	int ret;
	qmap_del(hd, key);
	ret = _gen_get(hd, key, value, value, 1);
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

static inline void iter_print(unsigned hd, unsigned sn) {
	void **rkeys = keys;
	void **rvalues = values;

	if (hd_meta[hd].flags & QMAP_REVERSE) {
		rkeys = values;
		rvalues = keys;
	}

	printf("ITER '");
	type_print(hd, QMAP_KEY, rkeys[sn]);
	printf("' - '");
	type_print(hd, QMAP_VALUE, rvalues[sn]);
	printf("'\n");
}

static inline void
test_third(void)
{
	unsigned hd = gen_open(UTOU, 0), cur_id;
	unsigned keys[] = { 3, 9 };
	unsigned values[] = { 5, 7 };
	unsigned sn;

	gen_put(hd, &keys[0], &values[0]);
	gen_put(hd, &keys[1], &values[1]);

	cur_id = qmap_iter(hd, NULL);
	while (qmap_next(&sn, cur_id))
		iter_print(hd, sn);

	qmap_close(hd);
}

static inline void
test_fourth(void)
{
	unsigned hd = gen_open(UTOS, QMAP_MIRROR),
		 rhd = hd + 1;
	unsigned keys[] = { 3, 9 };

	gen_put(hd, &keys[0], "hello");
	gen_put(hd, &keys[1], "hi");

	gen_get(rhd, "hello", &keys[0]);
	gen_get(rhd, "hi", &keys[1]);

	qmap_close(hd);
}

static inline void
test_fifth(void)
{
	unsigned hd = gen_open(STOU, QMAP_MIRROR),
		 rhd = hd + 1;
	unsigned values[] = { 3, 9 };

	gen_put(hd, "hello", &values[0]);
	gen_put(hd, "hi", &values[1]);
	printf("fifth two-way\n");

	gen_get(rhd, &values[0], "hello");
	gen_get(rhd, &values[1], "hi");

	qmap_close(hd);
}

static inline void
test_sixth(void)
{
	unsigned hd = gen_open(STOS, QMAP_MIRROR),
		 rhd = hd + 1;

	gen_put(hd, "hello", "hellov");
	gen_put(hd, "hi", "hiv");

	gen_get(rhd, "hellov", "hello");
	gen_get(rhd, "hiv", "hi");

	qmap_close(hd);
}

static inline void
test_seventh(void)
{
	unsigned hd = gen_open(STOS, QMAP_MIRROR),
		 rhd = hd + 1;
	unsigned cur_id, sn;

	gen_put(hd, "hello", "olleh");
	gen_put(hd, "hi", "ih");
	gen_put(hd, "ola", "alo");

	cur_id = qmap_iter(hd, NULL);
	while (qmap_next(&sn, cur_id))
		iter_print(hd, sn);

	printf("keyed iter\n");
	cur_id = qmap_iter(hd, "hello");
	while (qmap_next(&sn, cur_id))
		iter_print(hd, sn);

	gen_del(rhd, "alo", NULL);
	gen_get(rhd, "olleh", "hello");
	gen_get(rhd, "ih", "hi");

	printf("reverse iter\n");
	cur_id = qmap_iter(rhd, NULL);
	while (qmap_next(&sn, cur_id))
		iter_print(rhd, sn);

	printf("reverse keyed iter\n");
	cur_id = qmap_iter(rhd, "ih");
	while (qmap_next(&sn, cur_id))
		iter_print(rhd, sn);

	printf("final iter\n");
	cur_id = qmap_iter(hd, NULL);
	while (qmap_next(&sn, cur_id))
		iter_print(hd, sn);

	qmap_close(hd);
}

static inline
void test_eighth(void)
{
	unsigned cur_id, sn;
	unsigned hd = gen_open(UTOS, QMAP_AINDEX | QMAP_MIRROR),
		 rhd = hd + 1;

	gen_put(hd, NULL, "hello");
	gen_put(hd, NULL, "hi");
	gen_put(hd, NULL, "ola");

	cur_id = qmap_iter(hd, NULL);
	while (qmap_next(&sn, cur_id))
		iter_print(hd, sn);

	printf("reversed\n");
	cur_id = qmap_iter(rhd, NULL);
	while (qmap_next(&sn, cur_id))
		iter_print(rhd, sn);

	qmap_close(hd);
}

#if 0

static inline
void test_nineth(void)
{
	unsigned cur_id, key;
	unsigned hd = gen_open(UTOS, QMAP_DUP);
	unsigned keys[] = { 3, 3, 2 };
	char value[MAX_LEN];

	gen_put(hd, &keys[0], "hello");
	qmap_put(hd, &keys[1], "hi");
	/* errors += _gen_get(hd, &keys[1], "hi", "hello", 0); */
	gen_put(hd, &keys[2], "ola");

	cur_id = qmap_iter(hd, NULL);
	while (qmap_next(&key, value, cur_id))
		printf("ITER '%u' - '%s'\n", key, value);

	printf("Keyed iter\n");
	cur_id = qmap_iter(hd, &keys[0]);
	while (qmap_next(&key, value, cur_id))
		printf("ITER '%u' - '%s'\n", key, value);

	gen_del(hd, &keys[0], NULL);
	printf("After del keyed iter\n");
	cur_id = qmap_iter(hd, &keys[0]);
	while (qmap_next(&key, value, cur_id))
		printf("ITER '%u' - '%s'\n", key, value);
	printf("After del unkeyed\n");
	cur_id = qmap_iter(hd, NULL);
	while (qmap_next(&key, value, cur_id))
		printf("ITER '%u' - '%s'\n", key, value);

	qmap_close(hd);
}
#endif

static inline
void test_tenth(void)
{
	unsigned cur_id, sn;
	unsigned hd = gen_open(UTOS, QMAP_AINDEX);
	unsigned keys[] = { 3, 3, 2 };

	keys[0] = gen_put(hd, NULL, "hello");
	gen_put(hd, &keys[0], "hi");
	gen_put(hd, NULL, "ola");

	cur_id = qmap_iter(hd, NULL);
	while (qmap_next(&sn, cur_id))
		iter_print(hd, sn);

	printf("Keyed iter\n");
	cur_id = qmap_iter(hd, &keys[0]);
	while (qmap_next(&sn, cur_id))
		iter_print(hd, sn);

	qmap_close(hd);
}

static inline
void test_eleventh(void)
{
	unsigned cur_id, sn;
	unsigned hd = gen_open(UTOU, QMAP_AINDEX);
	unsigned keys[] = { 3, 6, 2 };
	unsigned values[] = { 2, 4, 3 };

	keys[0] = gen_put(hd, &keys[0], &values[0]);
	gen_put(hd, &keys[1], &values[1]);
	gen_put(hd, NULL, &values[2]);

	cur_id = qmap_iter(hd, NULL);
	while (qmap_next(&sn, cur_id))
		iter_print(hd, sn);

	printf("Keyed iter\n");
	cur_id = qmap_iter(hd, &keys[0]);
	while (qmap_next(&sn, cur_id))
		iter_print(hd, sn);

	qmap_close(hd);
}

int main(void) {
	qmap_init();

	printf("first\n");
	test_first();
	printf("second\n");
	test_second();
	printf("third\n");
	test_third();
	printf("fourth\n");
	test_fourth();
	printf("fifth\n");
	test_fifth();
	printf("sixth\n");
	test_sixth();
	printf("seventh\n");
	test_seventh();
	printf("eighth\n");
	test_eighth();
#if 0
	printf("nineth\n");
	test_nineth();
#endif
	printf("tenth\n");
	test_tenth();
	printf("eleventh\n");
	test_eleventh();

	return -errors;
}
