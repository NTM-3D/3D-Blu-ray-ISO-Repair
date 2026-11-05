#include "udf.h"

#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <io.h>

ErrCtx *g_err = NULL;
static void udf_setup_meta(Udf *u);

static void *xmalloc(size_t n) {
    void *volatile p = malloc(n ? n : 1);
    if (!p) { udf_error("Out of memory."); }
    return (void *)p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { udf_error("Out of memory."); }
    return q;
}

void udf_error(const char *fmt, ...) {
    va_list ap;
    if (g_err) {
        va_start(ap, fmt);
        vsnprintf(g_err->msg, sizeof(g_err->msg), fmt, ap);
        va_end(ap);
        longjmp(g_err->jb, 1);
    }
    va_start(ap, fmt);
    va_end(ap);
    exit(1);
}

/* endian helpers */
uint16_t u16(const unsigned char *b, size_t o){ return (uint16_t)(b[o] | (b[o+1]<<8)); }
uint32_t u32(const unsigned char *b, size_t o){ return (uint32_t)(b[o] | (b[o+1]<<8) | (b[o+2]<<16) | (b[o+3]<<24)); }
uint64_t u64(const unsigned char *b, size_t o){ uint64_t v=0; for(int i=7;i>=0;i--) v=(v<<8)|b[o+i]; return v; }
uint16_t b16(const unsigned char *b, size_t o){ return (uint16_t)((b[o]<<8)|b[o+1]); }
uint32_t b32(const unsigned char *b, size_t o){ return (uint32_t)((b[o]<<24)|(b[o+1]<<16)|(b[o+2]<<8)|b[o+3]); }

void seal(unsigned char *buf, size_t crclen) {
    /* crc_hqx */
    uint16_t crc = 0;
    for (size_t i=0;i<crclen;i++) {
        crc ^= (uint16_t)(buf[16+i] << 8);
        for (int k=0;k<8;k++) {
            if (crc & 0x8000) crc = (uint16_t)((crc<<1)^0x1021);
            else crc = (uint16_t)(crc<<1);
        }
    }
    buf[8] = (unsigned char)(crc & 0xFF);
    buf[9] = (unsigned char)(crc >> 8);
    /* crc length at 10 */
    buf[10] = (unsigned char)(crclen & 0xFF);
    buf[11] = (unsigned char)(crclen >> 8);
    buf[4]=0;
    uint8_t chk=0;
    for (int i=0;i<4;i++) chk += buf[i];
    for (int i=5;i<16;i++) chk += buf[i];
    buf[4]=chk;
}

/* Image */
Image *image_open(const char *path, int rw) {
    Image *img = xmalloc(sizeof(Image));
    memset(img,0,sizeof(Image));
    img->path = strdup(path);
    img->f = fopen(path, rw ? "r+b" : "rb");
    if (!img->f) { udf_error("Cannot open %s", path); }
    _fseeki64(img->f, 0, SEEK_END);
    img->orig_size = (uint64_t)_ftelli64(img->f);
    img->size = img->orig_size;
    return img;
}

void image_close(Image *img) {
    if (!img) return;
    if (img->f) fclose(img->f);
    free(img->pending);
    free(img->path);
    free(img);
}

void image_grow(Image *img, uint64_t new_size) {
    if (new_size > img->size) img->size = new_size;
}

static PendingSec *find_pending(Image *img, uint64_t sec) {
    for (size_t i=0;i<img->pending_count;i++) if (img->pending[i].sec==sec) return &img->pending[i];
    return NULL;
}

unsigned char *image_read(Image *img, uint64_t off, size_t n) {
    unsigned char *d = xmalloc(n);
    _fseeki64(img->f, (long long)off, SEEK_SET);
    size_t got = fread(d,1,n,img->f);
    if (got < n) memset(d+got,0,n-got);
    if (img->pending_count) {
        uint64_t first = off / BS;
        uint64_t last = (off + (n?n:1) - 1) / BS;
        for (uint64_t sec=first; sec<=last; sec++) {
            PendingSec *p = find_pending(img, sec);
            if (p) {
                uint64_t s = (sec*BS > off) ? sec*BS : off;
                uint64_t e = ((sec+1)*BS < off+n) ? (sec+1)*BS : (off+n);
                if (s < e) memcpy(d + (s-off), p->data + (s - sec*BS), (size_t)(e-s));
            }
        }
    }
    return d;
}

unsigned char *image_sector(Image *img, uint64_t sec) {
    return image_read(img, sec*BS, BS);
}

void image_put(Image *img, uint64_t sec, const unsigned char *data) {
    if ((sec+1)*BS > img->size) img->size = (sec+1)*BS;
    PendingSec *p = find_pending(img, sec);
    if (!p) {
        if (img->pending_count == img->pending_cap) {
            img->pending_cap = img->pending_cap ? img->pending_cap*2 : 64;
            img->pending = xrealloc(img->pending, img->pending_cap*sizeof(PendingSec));
        }
        p = &img->pending[img->pending_count++];
        p->sec = sec;
    }
    memcpy(p->data, data, BS);
}

void image_snapshot(Image *img, ImageSnap *snap) {
    snap->pending_count = img->pending_count;
    snap->pending_cap = img->pending_cap;
    snap->size = img->size;
    snap->pending = NULL;
    if (img->pending_count) {
        snap->pending = xmalloc(img->pending_count*sizeof(PendingSec));
        memcpy(snap->pending, img->pending, img->pending_count*sizeof(PendingSec));
    }
}

void image_restore(Image *img, ImageSnap *snap) {
    free(img->pending);
    img->pending = snap->pending;
    img->pending_count = snap->pending_count;
    img->pending_cap = snap->pending_cap;
    img->size = snap->size;
    snap->pending = NULL;
    snap->pending_count = snap->pending_cap = 0;
}

int image_apply(Image *img) {
    char jpath[4096];
    snprintf(jpath, sizeof(jpath), "%s.udffix-undo", img->path);
    FILE *probe = fopen(jpath, "rb");
    if (probe) {
        fclose(probe);
        udf_error("Undo journal already exists (%s). Run -undo first or remove it.", jpath);
    }
    FILE *j = fopen(jpath, "wb");
    if (!j) { udf_error("Cannot create undo journal."); }
    fwrite(JOURNAL_MAGIC,1,8,j);
    /* orig_size (8) + count (4) */
    unsigned char hdr[12];
    memcpy(hdr, &img->orig_size, 8);
    uint32_t cnt = (uint32_t)img->pending_count;
    memcpy(hdr+8, &cnt, 4);
    fwrite(hdr,1,12,j);
    /* sort pending by sec */
    /* simple insertion sort */
    for (size_t a=1;a<img->pending_count;a++) {
        PendingSec tmp = img->pending[a];
        size_t b=a;
        while (b>0 && img->pending[b-1].sec > tmp.sec) { img->pending[b]=img->pending[b-1]; b--; }
        img->pending[b]=tmp;
    }
    for (size_t i=0;i<img->pending_count;i++) {
        uint64_t sec = img->pending[i].sec;
        fwrite(&sec,1,8,j);
        /* read original sector */
        unsigned char *old = xmalloc(BS);
        _fseeki64(img->f, (long long)(sec*BS), SEEK_SET);
        size_t got = fread(old,1,BS,img->f);
        if (got < BS) memset(old+got,0,BS-got);
        fwrite(old,1,BS,j);
        free(old);
    }
    fflush(j); /* fsync not available easily */ fclose(j);
    /* write pending */
    for (size_t i=0;i<img->pending_count;i++) {
        _fseeki64(img->f, (long long)(img->pending[i].sec*BS), SEEK_SET);
        fwrite(img->pending[i].data,1,BS,img->f);
    }
    fflush(img->f);
    int n = (int)img->pending_count;
    img->pending_count = 0;
    return n;
}

int image_undo(const char *path) {
    char jpath[4096];
    snprintf(jpath, sizeof(jpath), "%s.udffix-undo", path);
    FILE *j = fopen(jpath, "rb");
    if (!j) { udf_error("No undo journal found (%s).", jpath); }
    char magic[8];
    if (fread(magic,1,8,j)!=8 || memcmp(magic,JOURNAL_MAGIC,8)!=0) { fclose(j); udf_error("Journal has invalid magic signature."); }
    uint64_t orig_size; uint32_t n;
    fread(&orig_size,1,8,j);
    fread(&n,1,4,j);
    /* read records */
    typedef struct { uint64_t sec; unsigned char data[BS]; } Rec;
    Rec *recs = xmalloc((size_t)n*sizeof(Rec));
    for (uint32_t i=0;i<n;i++) {
        fread(&recs[i].sec,1,8,j);
        if (fread(recs[i].data,1,BS,j)!=BS) { fclose(j); udf_error("Journal is truncated."); }
    }
    fclose(j);
    FILE *f = fopen(path, "r+b");
    if (!f) { udf_error("Cannot open ISO."); }
    for (uint32_t i=0;i<n;i++) {
        if (recs[i].sec*BS < orig_size) {
            _fseeki64(f, (long long)(recs[i].sec*BS), SEEK_SET);
            fwrite(recs[i].data,1,BS,f);
        }
    }
    _chsize_s(_fileno(f), (long long)orig_size);
    fflush(f); fclose(f);
    free(recs);
    /* rename journal to .done */
    char done[4096];
    snprintf(done, sizeof(done), "%s.udffix-undo.done", path);
    remove(done);
    rename(jpath, done);
    return (int)n;
}

/* UDF creation */
static void add_pd_sector(Udf *u, uint64_t sec) {
    if (u->pd_sectors_count == u->pd_sectors_cap) {
        u->pd_sectors_cap = u->pd_sectors_cap ? u->pd_sectors_cap*2 : 64;
        u->pd_sectors = xrealloc(u->pd_sectors, u->pd_sectors_cap*sizeof(uint64_t));
    }
    u->pd_sectors[u->pd_sectors_count++] = sec;
}

int tag_id(const unsigned char *b) {
    if (!b) return 0;
    /* need at least 16 */
    uint8_t chk = 0;
    for (int i=0;i<4;i++) chk += b[i];
    for (int i=5;i<16;i++) chk += b[i];
    if (chk != b[4]) return 0;
    uint16_t tid = u16(b,0);
    return tid ? tid : 0;
}

Udf *udf_create(Image *img) {
    Udf *u = xmalloc(sizeof(Udf));
    memset(u,0,sizeof(Udf));
    u->img = img;
    u->learned_aed_ref = -1;
    /* parse VDS */
    uint64_t n = img->size / BS;
    unsigned char *avdp = NULL;
    for (int s_idx=0; s_idx<3; s_idx++) {
        uint64_t s = (s_idx==0)?256 : (s_idx==1)? (n>=257? n-257:0) : (n? n-1:0);
        if (s < n) {
            unsigned char *b = image_sector(img, s);
            if (tag_id(b)==2) { avdp=b; break; }
            free(b);
        }
    }
    if (!avdp) udf_error("No UDF anchor volume descriptor found. Is this a valid 3D Blu-ray ISO?");
    uint32_t mlen = u32(avdp,16);
    uint32_t mloc = u32(avdp,20);
    /* parse VDS at mloc */
    unsigned char *lvd = NULL;
    for (uint32_t i=0;i<(mlen+BS-1)/BS;i++) {
        unsigned char *b = image_sector(img, mloc+i);
        int tid = tag_id(b);
        if (tid==5) {
            int part = u16(b,22);
            if (part < 16) {
                u->pd_valid[part]=1;
                u->pd_start[part]=u32(b,188);
                u->pd_length[part]=u32(b,192);
            }
            add_pd_sector(u, mloc+i);
        } else if (tid==6) {
            lvd = b; /* keep */
        } else if (tid==8) {
            free(b);
            break;
        }
        if (tid!=6) free(b);
    }
    if (!lvd) udf_error("LVD not found.");
    /* reserve VDS */
    uint32_t rlen = u32(avdp,24);
    uint32_t rloc = u32(avdp,28);
    if (rlen && rloc) {
        for (uint32_t i=0;i<(rlen+BS-1)/BS;i++) {
            unsigned char *b = image_sector(img, rloc+i);
            if (tag_id(b)==5) add_pd_sector(u, rloc+i);
            free(b);
        }
    }
    /* check bs */
    if (u32(lvd,212) != BS) udf_error("Logical block size is not 2048.");
    /* partition maps */
    /* parse at offset 440, count u32(lvd,268) */
    int nmaps = (int)u32(lvd,268);
    /* ensure maps array */
    u->maps_cap = nmaps+4;
    u->maps = xmalloc(u->maps_cap*sizeof(PartMap));
    memset(u->maps,0,u->maps_cap*sizeof(PartMap));
    u->maps_count=0;
    size_t off = 440;
    for (int mi=0; mi<nmaps; mi++) {
        int t = lvd[off];
        int l = lvd[off+1];
        if (t==1) {
            PartMap *m = &u->maps[u->maps_count++];
            memset(m,0,sizeof(PartMap));
            m->kind=0;
            m->part = u16(lvd, off+4);
        } else if (t==2 && memcmp(lvd+off+5, "*UDF Metadata Partition", 23)==0) {
            PartMap *m = &u->maps[u->maps_count++];
            memset(m,0,sizeof(PartMap));
            m->kind=1;
            m->part = u16(lvd, off+38);
            m->meta = u32(lvd, off+40);
            m->mirror = u32(lvd, off+44);
            m->bitmap = u32(lvd, off+48);
            m->blocks=0;
            m->has_mirror_ext=0;
            m->phys_ref=-1;
        } else {
            udf_error("Unsupported partition map.");
        }
        off += l;
    }
    free(avdp);
    /* setup meta */
    for (size_t i=0;i<u->maps_count;i++) {
        PartMap *m = &u->maps[i];
        if (m->kind!=1) continue;
        /* phys ref */
        int pref=-1;
        for (size_t j=0;j<u->maps_count;j++) {
            if (u->maps[j].kind==0 && u->maps[j].part==m->part) { pref=(int)j; break; }
        }
        if (pref<0) udf_error("Metadata partition refers to unknown partition.");
        m->phys_ref=pref;
        /* load meta file node */
        /* need udf_load, but udf_load uses u. We'll implement load after. For now store meta lbn; load later? */
        /* We'll load now by calling a helper after function definitions? Forward declare. */
    }
    /* We need parse fsd etc. But udf_load not yet defined. We'll define udf_load below and call setup in a second pass. */
    /* For simplicity, store and do setup after returning? Can't. We'll implement setup inline by duplicating load logic? */
    /* Instead, move _setup_meta to after udf_load definition: call a function. */
    /* We'll call udf_setup_meta(u) declared below. */
    udf_setup_meta(u);
    /* parse fsd */
    /* root_ref/lbn from LVD: u16(lvd,256), u32(lvd,252) */
    int ref = u16(lvd,256);
    uint32_t lbn = u32(lvd,252);
    uint64_t sec = udf_sector_of(u, ref, lbn, 0);
    int found=0;
    for (int i=0;i<8;i++) {
        unsigned char *b = image_sector(img, sec+i);
        if (tag_id(b)==256) {
            u->root_ref = u16(b,408);
            u->root_lbn = u32(b,404);
            found=1;
            free(b);
            break;
        }
        free(b);
    }
    if (!found) udf_error("File set descriptor not found.");
    free(lvd);
    return u;
}

/* Forward declarations for functions defined later in same file */
/* sector_of */
uint64_t udf_sector_of(Udf *u, int ref, uint32_t lbn, int mirror) {
    if (ref < 0 || (size_t)ref >= u->maps_count) udf_error("Partition reference %d does not exist.", ref);
    PartMap *m = &u->maps[ref];
    if (m->kind==0) {
        if (m->part >= 16 || !u->pd_valid[m->part]) udf_error("Bad physical partition.");
        return (uint64_t)u->pd_start[m->part] + lbn;
    }
    MetaExtentList *ext = mirror ? &m->mirror_ext : &m->ext;
    if (ext->count==0) udf_error("Metadata mirror not available.");
    uint32_t off = lbn;
    for (size_t i=0;i<ext->count;i++) {
        if (off < ext->nb[i]) {
            if (m->part >= 16 || !u->pd_valid[m->part]) udf_error("Bad partition.");
            return (uint64_t)u->pd_start[m->part] + ext->lbn[i] + off;
        }
        off -= ext->nb[i];
    }
    udf_error("Metadata block outside partition.");
    return 0;
}

static void extent_list_from_node(Udf *u, Node *node, MetaExtentList *out) {
    (void)u;
    /* node already loaded; node.ads contains descriptors */
    /* Build extent list: for each ad type 0/1, (data_ref, lbn, len) but MetaExtentList here stores lbn,nb for metadata file extents (physical lbn,nb) */
    /* For metadata partition extent list, parse ads as (lbn, nb) in same partition */
    /* We'll handle in _setup_meta by reading node and using its ads: type 0/1 -> lbn, nb */
    /* This function assumes node->ads populated */
    out->count=0;
    for (size_t i=0;i<node->ads.count;i++) {
        Ad *ad = &node->ads.items[i];
        if (ad->type==0 || ad->type==1) {
            if (out->count == out->cap) {
                out->cap = out->cap ? out->cap*2 : 16;
                out->lbn = xrealloc(out->lbn, out->cap*sizeof(uint32_t));
                out->nb = xrealloc(out->nb, out->cap*sizeof(uint32_t));
            }
            out->lbn[out->count]=ad->lbn;
            out->nb[out->count]=(ad->len + BS -1)/BS;
            out->count++;
        }
    }
}

static Node *load_node(Udf *u, int ref, uint32_t lbn, const char *name);

static void udf_setup_meta(Udf *u) {
    for (size_t volatile i=0;i<u->maps_count;i++) {
        PartMap *m = &u->maps[i];
        if (m->kind!=1) continue;
        int pref=-1;
        for (size_t j=0;j<u->maps_count;j++) if (u->maps[j].kind==0 && u->maps[j].part==m->part) { pref=(int)j; break; }
        if (pref<0) udf_error("Metadata partition refers to unknown partition.");
        m->phys_ref=pref;
        /* load meta node */
        /* temporarily set name */
        Node *volatile node = load_node(u, pref, m->meta, "metadata");
        extent_list_from_node(u, node, &m->ext);
        m->blocks=0;
        for (size_t k=0;k<m->ext.count;k++) m->blocks += m->ext.nb[k];
        if (m->mirror != 0xFFFFFFFF) {
            ErrCtx *saved = g_err;
            ErrCtx local;
            Node *volatile mnode = NULL;
            int volatile loaded = 0;
            if (saved) {
                g_err = &local;
                if (setjmp(local.jb)==0) {
                    mnode = load_node(u, pref, m->mirror, "metadata-mirror");
                    loaded = 1;
                }
                g_err = saved;
            } else {
                mnode = load_node(u, pref, m->mirror, "metadata-mirror");
                loaded = 1;
            }
            if (loaded) {
                extent_list_from_node(u, mnode, &m->mirror_ext);
                m->has_mirror_ext=1;
                node_free(mnode);
            } else {
                m->has_mirror_ext=0;
            }
        } else {
            m->has_mirror_ext=0;
        }
        node_free(node);
    }
}

/* Need implement load_node before use; but called via udf_load public. We'll define udf_load below and make load_node alias. */
Node *udf_load(Udf *u, int ref, uint32_t lbn, const char *name) {
    return load_node(u, ref, lbn, name);
}
void node_free(Node *n) {
    if (!n) return;
    free(n->adbytes);
    free(n->ads.items);
    free(n->chain.items);
    free(n);
}

/* extents_of */
Extent *extents_of(Udf *u, Node *n, size_t *out_count) {
    if (!n) { *out_count=0; return NULL; }
    for (size_t i=0;i<n->ads.count;i++) if (n->ads.items[i].type != 0) { *out_count=0; return NULL; }
    Extent *e = xmalloc((n->ads.count?n->ads.count:1)*sizeof(Extent));
    for (size_t i=0;i<n->ads.count;i++) {
        int ref = n->ads.items[i].pref;
        if (ref == -1) {
            PartMap *m = &u->maps[n->ref];
            if (m->kind==1) ref = (n->ftype==4) ? n->ref : m->phys_ref;
            else ref = n->ref;
        }
        e[i].ref = ref;
        e[i].lbn = n->ads.items[i].lbn;
        e[i].len = n->ads.items[i].len;
    }
    *out_count = n->ads.count;
    return e;
}

void udf_destroy(Udf *u) {
    if (!u) return;
    for (size_t i=0;i<u->maps_count;i++) {
        free(u->maps[i].ext.lbn); free(u->maps[i].ext.nb);
        free(u->maps[i].mirror_ext.lbn); free(u->maps[i].mirror_ext.nb);
        free(u->maps[i].ro_used.vals);
    }
    free(u->maps);
    free(u->pd_sectors);
    free(u);
}

unsigned char *udf_read_blocks(Udf *u, int ref, uint32_t lbn, size_t count) {
    size_t n = count*BS;
    unsigned char *out = xmalloc(n);
    for (size_t i=0;i<count;i++) {
        uint64_t sec = udf_sector_of(u, ref, lbn+i, 0);
        if ((sec+1)*BS > u->img->size) {
            free(out);
            udf_error("Block lies beyond end of image.");
        }
        unsigned char *s = image_sector(u->img, sec);
        memcpy(out + i*BS, s, BS);
        free(s);
    }
    return out;
}

int udf_sync_ok(Udf *u, int ref, uint32_t lbn) {
    unsigned char *b = udf_read_blocks(u, ref, lbn, 1);
    int ok = 1;
    for (int k=0;k<10;k++) if (b[4 + PKT*k] != 0x47) { ok=0; break; }
    free(b);
    return ok;
}

void udf_put_block(Udf *u, int ref, uint32_t lbn, const unsigned char *data) {
    PartMap *m = &u->maps[ref];
    uint64_t sec = udf_sector_of(u, ref, lbn, 0);
    unsigned char *old = image_sector(u->img, sec);
    image_put(u->img, sec, data);
    if (m->kind==1 && m->has_mirror_ext) {
        ErrCtx *saved = g_err;
        ErrCtx local;
        int ok = 1;
        uint64_t msec = 0;
        if (saved) {
            g_err = &local;
            if (setjmp(local.jb)!=0) ok = 0;
            else msec = udf_sector_of(u, ref, lbn, 1);
            g_err = saved;
        } else {
            msec = udf_sector_of(u, ref, lbn, 1);
        }
        if (ok) {
            unsigned char *mold = image_sector(u->img, msec);
            if (memcmp(mold, old, BS)==0) image_put(u->img, msec, data);
            free(mold);
        }
    }
    free(old);
}


#include "udf.h"
#include <stdlib.h>
#include <string.h>

static void *xm(size_t n){ void*p=malloc(n?n:1); if(!p) udf_error("OOM"); return p; }

unsigned char *udf_read_file(Udf *u, Node *n, size_t *out_len) {
    if (n->alloc==3) {
        size_t len = n->lad;
        unsigned char *d = xm(len?len:1);
        memcpy(d, n->adbytes, len);
        *out_len = n->info < len ? n->info : len;
        /* Embedded data: return exactly the recorded file length. */
        unsigned char *out = xm(n->info?n->info:1);
        /* For embedded, adbytes contains data; copy info bytes */
        if (n->info <= len) memcpy(out, n->adbytes, n->info);
        else { memcpy(out, n->adbytes, len); memset(out+len,0,n->info-len); }
        *out_len = (size_t)n->info;
        free(d);
        return out;
    }
    /* recorded extents */
    size_t total_blocks=0;
    for (size_t i=0;i<n->ads.count;i++) total_blocks += (n->ads.items[i].len + BS -1)/BS;
    size_t raw_len = total_blocks*BS;
    unsigned char *raw = xm(raw_len?raw_len:1);
    size_t pos=0;
    for (size_t i=0;i<n->ads.count;i++) {
        Ad *ad=&n->ads.items[i];
        size_t nb=(ad->len+BS-1)/BS;
        if (ad->type==0) {
            int ref = ad->pref;
            if (ref==-1) {
                PartMap *m=&u->maps[n->ref];
                if (m->kind==1) ref = (n->ftype==4)? n->ref : m->phys_ref;
                else ref = n->ref;
            }
            unsigned char *blk = udf_read_blocks(u, ref, ad->lbn, nb);
            memcpy(raw+pos, blk, nb*BS);
            free(blk);
        } else {
            memset(raw+pos,0,nb*BS);
        }
        pos += nb*BS;
    }
    size_t info = (size_t)n->info;
    unsigned char *out = xm(info?info:1);
    if (info <= raw_len) memcpy(out, raw, info);
    else { memcpy(out, raw, raw_len); memset(out+raw_len,0,info-raw_len); }
    free(raw);
    *out_len = info;
    return out;
}

DirList udf_listdir(Udf *u, Node *dir) {
    DirList dl; memset(&dl,0,sizeof(dl));
    size_t len;
    unsigned char *data = udf_read_file(u, dir, &len);
    size_t o=0;
    while (o + 38 <= len) {
        if (tag_id(data+o)!=257) break;
        unsigned char chars = data[o+18];
        unsigned char lfi = data[o+19];
        uint16_t liu = u16(data+o,36);
        uint32_t ln = (38 + liu + lfi + 3) & ~3u;
        if (!(chars & 0x0C) && lfi) {
            /* name */
            unsigned char *raw = data + o + 38 + liu;
            char name[256]; memset(name,0,sizeof(name));
            if (raw[0]==8) {
                size_t nn = lfi-1;
                if (nn >= sizeof(name)) nn=sizeof(name)-1;
                memcpy(name, raw+1, nn);
            } else if (raw[0]==16) {
                /* utf-16-be -> latin-ish; store raw bytes? We'll just copy high bytes? For filenames ASCII mostly */
                size_t nn=0;
                for (size_t i=1;i<lfi && nn<sizeof(name)-1;i+=2) name[nn++] = raw[i+1];
            }
            /* add entry */
            if (dl.count==dl.cap) { dl.cap=dl.cap?dl.cap*2:64; dl.items=realloc(dl.items,dl.cap*sizeof(DirEntry)); if(!dl.items) udf_error("OOM"); }
            DirEntry *e=&dl.items[dl.count++];
            memset(e,0,sizeof(*e));
            strcpy(e->name, name);
            e->isdir = (chars & 2) ? 1 : 0;
            e->ref = u16(data+o,28);
            e->lbn = u32(data+o,24);
        }
        o += ln;
    }
    free(data);
    return dl;
}

Node *udf_lookup_path(Udf *u, const char *path) {
    Node *n = udf_load(u, u->root_ref, u->root_lbn, "/");
    /* split path by / */
    char buf[1024]; strncpy(buf,path,sizeof(buf)-1); buf[sizeof(buf)-1]=0;
    char *tok = strtok(buf, "/");
    while (tok) {
        DirList dl = udf_listdir(u, n);
        int found=0;
        for (size_t i=0;i<dl.count;i++) {
            if (strcasecmp(dl.items[i].name, tok)==0) {
                Node *child = udf_load(u, dl.items[i].ref, dl.items[i].lbn, dl.items[i].name);
                node_free(n);
                n=child;
                found=1;
                break;
            }
        }
        /* free dl */
        free(dl.items);
        if (!found) { node_free(n); return NULL; }
        tok = strtok(NULL, "/");
    }
    return n;
}

static Node *load_node(Udf *u, int ref, uint32_t lbn, const char *name) {
    uint64_t sec = udf_sector_of(u, ref, lbn, 0);
    Node *n = xmalloc(sizeof(Node));
    memset(n,0,sizeof(Node));
    n->ref=ref;
    n->lbn=lbn;
    n->sec=sec;
    unsigned char *buf = image_sector(u->img, sec);
    memcpy(n->buf, buf, BS);
    free(buf);
    n->tag = tag_id(n->buf);
    if (n->tag != 261 && n->tag != 266) udf_error("%s: ICB not file entry (tag %d).", name?name:"?", n->tag);
    n->hdr = (n->tag==261)?176:216;
    n->ea = u32(n->buf, (n->tag==261)?168:208);
    n->lad = u32(n->buf, (n->tag==261)?172:212);
    n->info = u64(n->buf, 56);
    n->lbr = u64(n->buf, (n->tag==261)?64:72);
    n->ftype = n->buf[27];
    n->alloc = u16(n->buf,34) & 7;
    if (n->hdr + n->ea + n->lad > BS) udf_error("%s: File entry larger than one block.", name?name:"?");
    n->adbytes_len = n->lad;
    n->adbytes = xmalloc(n->lad ? n->lad : 1);
    if (n->lad) memcpy(n->adbytes, n->buf + n->hdr + n->ea, n->lad);
    if (name) { strncpy(n->name, name, sizeof(n->name)-1); }
    n->ads.items=NULL; n->ads.count=0; n->ads.cap=0;
    n->chain.items=NULL; n->chain.count=0; n->chain.cap=0;

    if (n->alloc==0 || n->alloc==1) {
        unsigned char *cur = n->adbytes;
        size_t curlen = n->adbytes_len;
        int guard=0;
        while (guard++ < 100000) {
            int size = (n->alloc==0)?8:16;
            int cont_found=0;
            Ad cont; memset(&cont,0,sizeof(cont));
            for (size_t o=0; o + (size_t)size <= curlen; o += size) {
                uint32_t w = u32(cur,o);
                Ad ad;
                ad.type = w >> 30;
                ad.len = w & 0x3FFFFFFF;
                ad.lbn = u32(cur,o+4);
                if (n->alloc==1) ad.pref = u16(cur,o+8); else ad.pref = -1;
                if (ad.type==3) { cont=ad; cont_found=1; break; }
                if (n->ads.count==n->ads.cap) { n->ads.cap = n->ads.cap? n->ads.cap*2:64; n->ads.items=xrealloc(n->ads.items,n->ads.cap*sizeof(Ad)); }
                n->ads.items[n->ads.count++]=ad;
            }
            if (!cont_found) break;
            /* find AED */
            if (cont.pref != -1) {
                /* explicit reference */
            }
            /* candidates */
            int cands[16]; int ncand=0;
            if (cont.pref != -1) cands[ncand++]=cont.pref;
            else {
                if (u->learned_aed_ref != -1) cands[ncand++]=u->learned_aed_ref;
                cands[ncand++]=n->ref;
                for (size_t r=0;r<u->maps_count;r++) {
                    int dup=0; for (int k=0;k<ncand;k++) if (cands[k]==(int)r) dup=1;
                    if (!dup && ncand<16) cands[ncand++]=(int)r;
                }
            }
            unsigned char *volatile blk=NULL;
            int volatile foundaed=0;
            for (int volatile ci=0; ci<ncand; ci++) {
                int r = cands[ci];
                ErrCtx *saved = g_err;
                ErrCtx local; g_err=&local;
                if (setjmp(local.jb)==0) {
                    uint64_t sec2 = udf_sector_of(u, r, cont.lbn, 0);
                    unsigned char *b = image_sector(u->img, sec2);
                    if (tag_id(b)==258) { blk=b; 
                        if (n->chain.count==n->chain.cap) { n->chain.cap=n->chain.cap?n->chain.cap*2:16; n->chain.items=xrealloc(n->chain.items,n->chain.cap*sizeof(ChainItem)); }
                        n->chain.items[n->chain.count].ref=r;
                        n->chain.items[n->chain.count].lbn=cont.lbn;
                        n->chain.count++;
                        u->learned_aed_ref=r;
                        foundaed=1;
                    } else free(b);
                } else {
                    /* ignore */
                }
                g_err = saved;
                if (foundaed) break;
            }
            if (!blk) udf_error("%s: Allocation extent descriptor not found.", n->name);
            uint32_t adlen = u32(blk,20);
            unsigned char *newcur = xmalloc(adlen ? adlen : 1);
            memcpy(newcur, blk+24, adlen);
            if (cur != n->adbytes) free(cur);
            cur = newcur;
            curlen = adlen;
            free(blk);
        }
        if (cur != n->adbytes) free(cur);
    }
    return n;
}
