#include "servertypes.h"
#include "uidl.h"

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

struct courier_data {
	const char* folder;
	int dirfd;

	struct uidl_list *uidl;
	char *uidl_headline;
};

struct courier_uidl_extra {
	char *flags;
	int utf8;
	int size;
};

static
int courier_detect(const char* folder)
{
	int detected;
	struct stat dummy;

	int dirfd = open(folder, O_RDONLY);
	if (dirfd < 0) {
		perror(folder);
		exit(1);
	}

	detected = fstatat(dirfd, "courierimapuiddb", &dummy, 0) == 0
		|| fstatat(dirfd, "courierpop3dsizelist", &dummy, 0) == 0;

	close(dirfd);
	return detected;
}

static
void* courier_open(const char* folder, int dirfd)
{
	struct courier_data *p = malloc(sizeof(struct courier_data));
	if (!p) {
		perror("malloc");
		exit(1);
	}

	p->folder = folder;
	p->dirfd = dirfd;
	p->uidl = NULL;

	return p;
}

//static
//char* courier_pop3_get_uidl(void *_p)
//{
//	struct courier_data *p = _p;
//	int fd = openat(p->dirfd, "courierpop3dsizelist");
//
//	return NULL;
//}

static
void courier_close(void* _p)
{
	free(_p);
}

static
int courier_is_pop3(void* _p)
{
	struct stat st;
	struct courier_data *p = _p;

	return fstatat(p->dirfd, "courierpop3dsizelist", &st, 0) == 0;
}

static struct maildir_type maildir_courier = {
	.label = "Courier-IMAP",
	.detect = courier_detect,
	.open = courier_open,
	.is_pop3 = courier_is_pop3,
//	.pop3_get_uidl = courier_pop3_get_uidl,
//	.pop3_set_uidl = courier_pop3_set_uidl,
	.close = courier_close,
};

static void __attribute__((constructor)) _open_courier()
{
	register_maildir_type(&maildir_courier);
}