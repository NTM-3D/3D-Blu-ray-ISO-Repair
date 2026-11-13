#ifndef REPORT_H
#define REPORT_H
#include "udf.h"

typedef struct { char stem[64]; char dstem[64]; char status[160]; } PairStatus;

typedef struct {
    char name[128];
    uint64_t old_size, new_size;
    size_t n_extents, n_aed;
} ChangeRec;

typedef struct { char stem[64]; char label[128]; } RepairKindRec;

struct Report {
    int chatty;
    int problems;
    int metadata_repaired;
    int metadata_fragmented;
    PairStatus *pairs; size_t pairs_count, pairs_cap;
    ChangeRec *changes; size_t changes_count, changes_cap;
    char **fixed; size_t fixed_count, fixed_cap;
    char **realigned; size_t realigned_count, realigned_cap;
    char **errors; size_t errors_count, errors_cap;
    RepairKindRec *rk; size_t rk_count, rk_cap;
    size_t clips_scanned, playlists_scanned;
};

Report *report_create(int chatty);
void report_free(Report *r);
void report_say(Report *r, const char *fmt, ...);
void report_pair_status(Report *r, const char *stem, const char *dstem, const char *status);
void report_pair_set_status(Report *r, const char *stem, const char *status);
void report_change(Report *r, const char *name, uint64_t old_size, uint64_t new_size, size_t n_extents, size_t n_aed);
void report_repair_kind(Report *r, const char *stem, const char *label);
const char *report_repair_kind_get(Report *r, const char *stem);
void report_mark_fixed(Report *r, const char *name);
void report_mark_realigned(Report *r, const char *stem);
int report_is_realigned(Report *r, const char *stem);
void report_add_error(Report *r, const char *fmt, ...);
int report_issue_count(Report *r);
int report_blocking_count(Report *r);
int report_pair_is_issue(const PairStatus *p);
int report_pair_is_blocking(const PairStatus *p);
/* Format "label: count; label: count" in sorted label order. */
void report_format_repair_counts(Report *r, char *buf, size_t bufsize);
#endif
