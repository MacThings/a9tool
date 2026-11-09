#ifndef A9_COMPARE_H
#define A9_COMPARE_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint64_t added_rows, modified_rows, removed_rows;
    uint32_t new_files, removed_files, changed_files, identical_files;
} A9CompareStats;

/* Compare two embedded UTF-16LE CSV files without extracting to disk.
 * Physical file line numbers are retained for multiline quoted records.
 * old/new data may be NULL if the CSV file is newly added or removed.
 * Returns 0 for successful comparison, -1 for unparseable data or OOM.
 */
int a9_compare_csv(FILE *report, const char *name,
                   const uint8_t *old_data, size_t old_size,
                   const uint8_t *new_data, size_t new_size,
                   int additions_only, A9CompareStats *stats);
#endif
