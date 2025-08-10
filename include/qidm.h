#ifndef IDM_H
#define IDM_H

#include <sys/queue.h>
#include <stdlib.h>

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

idm_t idm_init(void);
unsigned idm_new(idm_t *idm);
int idm_del(idm_t *idm, unsigned id);

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
		return -1;

	ret = popped->value;
	SLIST_REMOVE_HEAD(list, entry);
	free(popped);
	return ret;
}


static inline
ids_t ids_init(void) {
	struct ids list;
	SLIST_INIT(&list);
	return list;
}

static inline
void ids_drop(ids_t *list) {
	while (!ids_pop(list));
}

#endif
