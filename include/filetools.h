#ifndef __FILETOOLS_H__
#define __FILETOOLS_H__

struct stat;

int files_identical(int fd1, const char* path1, const struct stat* st1, int fd2, const char* path2, const struct stat* st2);

#endif
