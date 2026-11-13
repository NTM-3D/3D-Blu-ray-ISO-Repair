#ifndef UDF_H
#define UDF_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <setjmp.h>

#define BS 2048
#define PKT 192
#define MAX_EXT 0x3FFFF800u
#define JOURNAL_MAGIC "UDFFIX01"
#define UDF_FT_REALTIME 249

typedef struct { uint64_t sec; unsigned char data[BS]; } PendingSec;

typedef struct {
    char *path;
    FILE *f;
    uint64_t orig_size;
    uint64_t size;
    PendingSec *pending;
    size_t pending_count;
    size_t pending_cap;
} Image;

typedef struct {
    PendingSec *pending;
    size_t pending_count;
    size_t pending_cap;
    uint64_t size;
} ImageSnap;

typedef struct Report Report;

typedef struct { int ref; uint32_t lbn; uint64_t len; } Extent;

typedef struct { uint32_t *lbn; uint32_t *nb; size_t count; size_t cap; } MetaExtentList;

typedef struct { uint32_t *vals; size_t count; size_t cap; } UintSet;

typedef struct {
    int kind; /* 0 phys, 1 meta */
    int part;
    /* meta only */
    uint32_t meta;
    uint32_t mirror;
    uint32_t bitmap;
    uint32_t alloc_unit;
    uint16_t align_unit;
    uint64_t blocks;
    MetaExtentList ext;
    MetaExtentList mirror_ext;
    int has_mirror_ext;
    int phys_ref;
    UintSet ro_used;
} PartMap;

typedef struct {
    int type; /* 0 recorded, 1, 2, 3 continuation */
    uint32_t len;
    uint32_t lbn;
    int pref; /* -1 for none */
} Ad;

typedef struct { Ad *items; size_t count; size_t cap; } AdList;

typedef struct { int ref; uint32_t lbn; } ChainItem;

typedef struct { ChainItem *items; size_t count; size_t cap; } ChainList;

typedef struct {
    int ref;
    uint32_t lbn;
    uint64_t sec;
    unsigned char buf[BS];
    int tag;
    int hdr;
    uint32_t ea;
    uint32_t lad;
    uint64_t info;
    uint64_t lbr;
    int ftype;
    int alloc;
    unsigned char *adbytes;
    size_t adbytes_len;
    AdList ads;
    ChainList chain;
    char name[512];
} Node;

typedef struct {
    Image *img;
    /* partition descriptors: index by partition number (small) */
    uint32_t pd_start[16];
    uint32_t pd_length[16];
    int pd_valid[16];
    uint64_t *pd_sectors;
    size_t pd_sectors_count;
    size_t pd_sectors_cap;
    uint32_t integrity_loc;
    uint32_t integrity_len;
    PartMap *maps;
    size_t maps_count;
    size_t maps_cap;
    int learned_aed_ref; /* -1 none */
    int mirror_warned;
    /* root */
    int root_ref;
    uint32_t root_lbn;
} Udf;

/* Error handling */
typedef struct { jmp_buf jb; char msg[1024]; } ErrCtx;
extern ErrCtx *g_err;

void udf_error(const char *fmt, ...);

/* Image */
Image *image_open(const char *path, int rw);
void image_close(Image *img);
void image_grow(Image *img, uint64_t new_size);
unsigned char *image_read(Image *img, uint64_t off, size_t n); /* caller frees */
unsigned char *image_sector(Image *img, uint64_t sec);
void image_put(Image *img, uint64_t sec, const unsigned char *data);
int image_apply(Image *img); /* returns count */
int image_undo(const char *path);
void image_snapshot(Image *img, ImageSnap *snap);
void image_restore(Image *img, ImageSnap *snap);

/* UDF */
Udf *udf_create(Image *img);
void udf_destroy(Udf *u);
void udf_refresh(Udf *u);
Node *udf_load(Udf *u, int ref, uint32_t lbn, const char *name);
unsigned char *udf_read_file(Udf *u, Node *n, size_t *out_len);
void node_free(Node *n);
uint64_t udf_sector_of(Udf *u, int ref, uint32_t lbn, int mirror);
unsigned char *udf_read_blocks(Udf *u, int ref, uint32_t lbn, size_t count);
int udf_sync_ok(Udf *u, int ref, uint32_t lbn);
void udf_put_block(Udf *u, int ref, uint32_t lbn, const unsigned char *data);
void udf_grow_iso(Udf *u, uint64_t needed_blocks, Report *rep);
int udf_repair_overlapping_metadata(Udf *u, Report *rep);
uint32_t *udf_alloc_blocks(Udf *u, int ref, size_t count, size_t *out_count, Report *rep);
int udf_rewrite_file(Udf *u, Node *n, Extent *exts, size_t ext_count, uint64_t length, int alloc, int counts_aed, Report *rep);
int udf_set_file_type(Udf *u, Node *n, uint8_t file_type);
/* extents helpers */
Extent *extents_of(Udf *u, Node *n, size_t *out_count); /* NULL if not all recorded; caller frees */

typedef struct { char name[256]; int isdir; int ref; uint32_t lbn; } DirEntry;
typedef struct { DirEntry *items; size_t count; size_t cap; } DirList;
DirList udf_listdir(Udf *u, Node *dir);
Node *udf_lookup_path(Udf *u, const char *path);

/* helpers */
uint16_t u16(const unsigned char *b, size_t o);
uint32_t u32(const unsigned char *b, size_t o);
uint64_t u64(const unsigned char *b, size_t o);
uint16_t b16(const unsigned char *b, size_t o);
uint32_t b32(const unsigned char *b, size_t o);

void seal(unsigned char *buf, size_t crclen);
int tag_id(const unsigned char *b);

#endif
