#ifndef __UIDL_H__
#define __UIDL_H__

struct uidl_list;

/* copies will be made, basename will be extracted ... */
extern void uidl_list_insert(struct uidl_list ** l, const char *msg, const char *uidl, void *extra);
extern const char* uidl_list_find_uidl(const struct uidl_list *l, const char *msg);
extern void uidl_list_free(struct uidl_list ** l);
extern void uidl_list_foreach(const struct uidl_list, void* pvt, void (*cb)(void *pvt, const char* msg, const char* uidl, void* extra));

#endif
