#ifndef CSVP_SCHEMA_H
#define CSVP_SCHEMA_H
#include <stdint.h>
#include <stddef.h>
typedef struct {uint32_t record;uint16_t columns;} CsvWidth;
typedef struct {uint32_t record;uint32_t hash;} CsvAnchor;
typedef struct {
    const char *filename;
    uint32_t records, max_physical_line;
    uint16_t default_columns;
    const CsvWidth *widths;
    size_t width_count;
    const CsvAnchor *anchors;
    size_t anchor_count;
} CsvSchema;
const CsvSchema *csvp_find_schema(const char *filename);
uint32_t csvp_anchor_hash(const uint16_t *text,size_t len);
#endif
