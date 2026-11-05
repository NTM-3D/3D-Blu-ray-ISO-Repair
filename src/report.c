#include "report.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

static void *xm(size_t n){ void*p=malloc(n?n:1); if(!p) udf_error("Out of memory."); return p; }

Report *report_create(int chatty){ Report*r=xm(sizeof(Report)); memset(r,0,sizeof(Report)); r->chatty=chatty; return r; }

static void free_strs(char **a, size_t n){ for(size_t i=0;i<n;i++) free(a[i]); free(a); }

void report_free(Report*r){
    if(!r) return;
    free(r->pairs); free(r->changes);
    free_strs(r->fixed, r->fixed_count);
    free_strs(r->realigned, r->realigned_count);
    free_strs(r->errors, r->errors_count);
    free(r->rk); free(r);
}

void report_say(Report *r, const char *fmt, ...) {
    if (!r || !r->chatty) return;
    va_list ap; va_start(ap,fmt); vprintf(fmt,ap); va_end(ap); printf("\n");
}

int report_pair_is_issue(const PairStatus *p) {
    if (strcmp(p->status,"OK")==0) return 0;
    if (strncmp(p->status,"Skipped",7)==0) return 0;
    if (strncmp(p->status,"Cannot repair",13)==0) return 0;
    if (strncmp(p->status,"REFUSED",7)==0) return 0;
    return 1;
}

int report_pair_is_blocking(const PairStatus *p) {
    return strncmp(p->status,"Cannot repair",13)==0 || strncmp(p->status,"REFUSED",7)==0;
}

void report_pair_status(Report *r, const char *stem, const char *dstem, const char *status) {
    if (r->pairs_count==r->pairs_cap) {
        r->pairs_cap = r->pairs_cap ? r->pairs_cap*2 : 16;
        r->pairs = realloc(r->pairs, r->pairs_cap*sizeof(PairStatus));
        if (!r->pairs) udf_error("Out of memory.");
    }
    PairStatus *p=&r->pairs[r->pairs_count++];
    memset(p,0,sizeof(*p));
    if (stem) strncpy(p->stem,stem,sizeof(p->stem)-1);
    if (dstem) strncpy(p->dstem,dstem,sizeof(p->dstem)-1);
    if (status) strncpy(p->status,status,sizeof(p->status)-1);
}

void report_pair_set_status(Report *r, const char *stem, const char *status) {
    for (size_t i=0;i<r->pairs_count;i++) {
        if (strcasecmp(r->pairs[i].stem, stem)==0) {
            strncpy(r->pairs[i].status, status, sizeof(r->pairs[i].status)-1);
            r->pairs[i].status[sizeof(r->pairs[i].status)-1]=0;
        }
    }
}

void report_change(Report *r, const char *name, uint64_t old_size, uint64_t new_size, size_t n_extents, size_t n_aed) {
    if (r->changes_count==r->changes_cap) {
        r->changes_cap = r->changes_cap ? r->changes_cap*2 : 16;
        r->changes = realloc(r->changes, r->changes_cap*sizeof(*r->changes));
        if (!r->changes) udf_error("Out of memory.");
    }
    ChangeRec *c = &r->changes[r->changes_count++];
    memset(c,0,sizeof(*c));
    c->old_size=old_size; c->new_size=new_size; c->n_extents=n_extents; c->n_aed=n_aed;
    if (name) strncpy(c->name,name,sizeof(c->name)-1);
}

void report_repair_kind(Report *r, const char *stem, const char *label) {
    for (size_t i=0;i<r->rk_count;i++) {
        if (strcasecmp(r->rk[i].stem, stem)==0) {
            if (label) strncpy(r->rk[i].label,label,sizeof(r->rk[i].label)-1);
            return;
        }
    }
    if (r->rk_count==r->rk_cap) {
        r->rk_cap = r->rk_cap ? r->rk_cap*2 : 16;
        r->rk = realloc(r->rk, r->rk_cap*sizeof(*r->rk));
        if (!r->rk) udf_error("Out of memory.");
    }
    if (stem) strncpy(r->rk[r->rk_count].stem,stem,sizeof(r->rk[r->rk_count].stem)-1);
    else r->rk[r->rk_count].stem[0]=0;
    if (label) strncpy(r->rk[r->rk_count].label,label,sizeof(r->rk[r->rk_count].label)-1);
    else r->rk[r->rk_count].label[0]=0;
    r->rk_count++;
}

const char *report_repair_kind_get(Report *r, const char *stem) {
    for (size_t i=0;i<r->rk_count;i++) if (strcasecmp(r->rk[i].stem, stem)==0) return r->rk[i].label;
    return NULL;
}

static void push_str(char ***arr, size_t *count, size_t *cap, const char *s) {
    if (*count==*cap) { *cap=*cap?*cap*2:16; *arr=realloc(*arr,*cap*sizeof(char*)); if(!*arr) udf_error("Out of memory."); }
    (*arr)[(*count)++]=strdup(s?s:"");
}

void report_mark_fixed(Report *r, const char *name){ push_str(&r->fixed,&r->fixed_count,&r->fixed_cap,name); }
void report_mark_realigned(Report *r, const char *stem){ push_str(&r->realigned,&r->realigned_count,&r->realigned_cap,stem); }

int report_is_realigned(Report *r, const char *stem) {
    for (size_t i=0;i<r->realigned_count;i++) if (strcasecmp(r->realigned[i],stem)==0) return 1;
    return 0;
}

void report_add_error(Report *r, const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap);
    push_str(&r->errors,&r->errors_count,&r->errors_cap,buf);
}

int report_issue_count(Report *r) {
    int n=0;
    for (size_t i=0;i<r->pairs_count;i++) if (report_pair_is_issue(&r->pairs[i])) n++;
    return n;
}

int report_blocking_count(Report *r) {
    int n=0;
    for (size_t i=0;i<r->pairs_count;i++) if (report_pair_is_blocking(&r->pairs[i])) n++;
    return n;
}

typedef struct { const char *label; int count; } KindCount;

static int kindcmp(const void *a, const void *b) {
    const KindCount *ka=a, *kb=b;
    return strcmp(ka->label,kb->label);
}

void report_format_repair_counts(Report *r, char *buf, size_t bufsize) {
    KindCount kinds[64]; size_t nk=0;
    for (size_t i=0;i<r->pairs_count;i++) {
        if (!report_pair_is_issue(&r->pairs[i])) continue;
        const char *label = report_repair_kind_get(r, r->pairs[i].stem);
        if (!label) label = "File table repair";
        size_t j;
        for (j=0;j<nk;j++) if (strcmp(kinds[j].label,label)==0) { kinds[j].count++; break; }
        if (j==nk && nk<64) { kinds[nk].label=label; kinds[nk].count=1; nk++; }
    }
    qsort(kinds,nk,sizeof(kinds[0]),kindcmp);
    buf[0]=0;
    size_t pos=0;
    for (size_t i=0;i<nk;i++) {
        int wrote = snprintf(buf+pos, bufsize>pos?bufsize-pos:0, "%s%s: %d", i?"; ":"", kinds[i].label, kinds[i].count);
        if (wrote<=0) break;
        pos += (size_t)wrote;
        if (pos>=bufsize) { pos=bufsize?bufsize-1:0; break; }
    }
    if (bufsize) buf[pos]=0;
}
