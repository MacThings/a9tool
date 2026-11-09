#ifndef A9_PORT_H
#define A9_PORT_H
#include <stdio.h>
#include <stddef.h>

/* UTF-8 path aware cross-platform filesystem / POSIX compatibility layer. */
typedef struct A9Dir A9Dir;
typedef struct { char name[1024]; } A9Dirent;
FILE *a9_fopen(const char *path, const char *mode);
int a9_exists(const char *path);
int a9_is_directory(const char *path);
int a9_mkdir(const char *path);
A9Dir *a9_dir_open(const char *path);
const A9Dirent *a9_dir_read(A9Dir *dir);
void a9_dir_close(A9Dir *dir);
ptrdiff_t a9_getline(char **line, size_t *cap, FILE *stream);
char *a9_strdup(const char *src);
#endif
