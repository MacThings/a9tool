#ifndef A9_MD5_H
#define A9_MD5_H
#include <stddef.h>
#include <stdint.h>
void a9_md5(const void *buffer, size_t length, uint8_t hash[16]);
#endif
