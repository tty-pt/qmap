#ifndef IDM_H
#define IDM_H

#include <sys/queue.h>
#include <stdlib.h>

#define IDM_MISS ((unsigned) -1)

struct ids_item {
	unsigned value;
	SLIST_ENTRY(ids_item) entry;
};

SLIST_HEAD(ids, ids_item);

typedef struct ids ids_t;

typedef struct {
	ids_t free;
	unsigned last;
} idm_t;

static inline
ids_t ids_init(void) {
	struct ids list;
	SLIST_INIT(&list);
	return list;
}

static inline
void ids_push(ids_t *list, unsigned id) {
	struct ids_item *item = (struct ids_item *)
		malloc(sizeof(struct ids_item));
	item->value = id;
	SLIST_INSERT_HEAD(list, item, entry);
}

static inline
unsigned ids_pop(ids_t *list) {
	struct ids_item *popped = SLIST_FIRST(list);
	unsigned ret;

	if (!popped)
		return (unsigned) IDM_MISS;

	ret = popped->value;
	SLIST_REMOVE_HEAD(list, entry);
	free(popped);
	return ret;
}

static inline
void ids_drop(ids_t *list) {
	while (!ids_pop(list));
}

static inline
unsigned ids_peek(ids_t *list) {
	struct ids_item *top = SLIST_FIRST(list);
	return top ? top->value : IDM_MISS;
}

static inline
struct ids_item *ids_iter(ids_t *list) {
	return SLIST_FIRST(list);
}

static inline
struct ids_item *ids_next(unsigned *id, struct ids_item *last) {
	*id = last->value;
	return SLIST_NEXT(last, entry);
}

static inline
idm_t idm_init(void) {
	idm_t idm;
	idm.free = ids_init();
	idm.last = 0;
	return idm;
}

static inline
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

static inline
unsigned idm_new(idm_t *idm) {
	unsigned ret = ids_pop(&idm->free);

	if (ret == IDM_MISS)
		return idm->last++;

	return ret;
}

#if 1
static inline
unsigned idm_push(idm_t *idm, unsigned n) {
	unsigned i;

	if (idm->last > n) {
		struct ids_item *item;

		SLIST_FOREACH(item, &idm->free, entry) {
			SLIST_REMOVE(&idm->free, item,
					ids_item, entry);
		}

		return IDM_MISS;
	}
	
	for (i = idm->last; i < n; i++)
		ids_push(&idm->free, i);

	idm->last = n + 1;
	return n;
}
#endif

#if 0
#include <stdio.h>
static inline void
idm_debug(idm_t *idm) {
	struct ids_item *item;
	unsigned i;

	fprintf(stderr, "last %u free", idm->last);
	SLIST_FOREACH(item, &idm->free, entry)
		fprintf(stderr, " %u\n", item->value);

	fprintf(stderr, "\n");
}
#endif

#endif
