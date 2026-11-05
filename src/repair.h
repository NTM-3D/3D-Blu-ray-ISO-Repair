#ifndef REPAIR_H
#define REPAIR_H
#include "udf.h"
#include "report.h"
#include "clpi.h"

typedef struct {
    int apply;
    int chatty;
    int show_all;
    int no_data_check;
    char order[4]; /* "DB" or "BD" or "" */
} Options;

int run_single(const char *path, Options *opt);
int run_folder(const char *folder, Options *opt);
int run_undo(const char *path);

#endif
