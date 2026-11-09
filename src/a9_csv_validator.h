#ifndef A9_CSV_VALIDATOR_H
#define A9_CSV_VALIDATOR_H
/* Return 0 on success, nonzero on invalid files; always prints a summary.
 * Explicit file/physical-line diagnostics using the reference schema. */
int a9_csv_check_folder(const char *folder);
/* True if files.txt names CSV files, or if a direct folder has CSV files. */
int a9_csv_folder_has_csv(const char *folder);
#endif
