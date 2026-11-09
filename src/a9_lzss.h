#ifndef A9_LZSS_H
#define A9_LZSS_H
#include <stddef.h>
#include <stdint.h>
/* Allocated buffers returned to caller; 0 on success, -1 on invalid input/memory. */
int a9_lzss_decode(const uint8_t *in, size_t input_size, uint8_t **out, size_t *output_size);
int a9_lzss_encode(const uint8_t *in, size_t input_size, uint8_t **out, size_t *output_size);
#endif
