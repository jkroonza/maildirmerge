#ifndef __FILETOOLS_H__
#define __FILETOOLS_H__

#include <stdbool.h>

struct stat;

int files_identical(int fd1, const char* path1, const struct stat* st1, int fd2, const char* path2, const struct stat* st2);
int is_maildir(int fd, const char* folder);
int get_maildir_fd_at(int bfd, const char* folder);
int get_maildir_fd(const char* folder);
int maildir_create_sub(int bfd, const char* target, const char* foldername, bool dry_run);

int message_seen(const char* filename);

void maildir_move(int sfd, const char* source, int tfd, const char* target, const char* sub, const char* fname, bool dry_run);

#endif
