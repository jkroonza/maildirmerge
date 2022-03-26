#include "uidl.h"

struct uidl_list {
	const char *msgname;
	const char *uidl;
	struct uidl_list *next;
};

extern void uidl_list_insert(struct uidl_list ** l, const char *msg, const char *uidl);
extern const char* uidl_list_find_uidl(const struct uidl_list *l, const char *msg);
extern void uild_list_free(struct uild_list ** l);
