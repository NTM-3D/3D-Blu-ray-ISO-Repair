#include "clpi.h"
#include <stdlib.h>
#include <string.h>

static void *xm(size_t n){ void*p=malloc(n?n:1); if(!p) udf_error("Out of memory."); return p; }

Clpi parse_clpi(const unsigned char *data, size_t len) {
    Clpi c; memset(&c,0,sizeof(c));
    if (len < 60 || memcmp(data,"HDMV",4)!=0) return c;
    c.pkts = b32(data,56);
    uint32_t ext = b32(data,24);
    if (ext && ext + 12 <= len) {
        for (size_t i=0;i<data[ext+11];i++) {
            size_t e = ext + 12 + 12*i;
            if (e+12 > len) break;
            if (b16(data,e)==2 && b16(data,e+2)==4) {
                uint32_t q = ext + b32(data,e+4);
                if (q+8 > len) break;
                uint32_t cnt = b32(data,q+4);
                c.sp = xm(cnt*sizeof(uint32_t));
                c.sp_count = cnt;
                for (uint32_t k=0;k<cnt;k++) {
                    if (q+8+4*k+4 > len) break;
                    c.sp[k] = b32(data, q+8+4*k);
                }
            }
        }
    }
    c.valid = 1;
    return c;
}

MplsPairs parse_mpls_pairs(const unsigned char *data, size_t len) {
    MplsPairs p; memset(&p,0,sizeof(p));
    if (len < 20 || memcmp(data,"MPLS",4)!=0) return p;
    uint32_t pl = b32(data,8);
    uint32_t ext = b32(data,16);
    /* parse items */
    size_t item_count = 0;
    char **items = NULL;
    if (pl + 8 <= len) {
        item_count = b16(data, pl+6);
        items = xm(item_count*sizeof(char*));
        size_t pos = pl + 10;
        for (size_t i=0;i<item_count;i++) {
            items[i] = xm(6);
            memset(items[i],0,6);
            if (pos+7 <= len) memcpy(items[i], data+pos+2, 5);
            if (pos+2 <= len) pos += 2 + b16(data,pos);
            else break;
        }
    }
    /* parse ext */
    if (ext && ext+12 <= len) {
        for (size_t i=0;i<data[ext+11];i++) {
            size_t e = ext + 12 + 12*i;
            if (e+12 > len) break;
            if (b16(data,e)!=2 || b16(data,e+2)!=2) continue;
            uint32_t q = ext + b32(data,e+4);
            if (q+6 > len) continue;
            uint16_t nsub = b16(data,q+4);
            q += 6;
            for (size_t s=0;s<nsub;s++) {
                if (q+10 > len) break;
                uint32_t sl = b32(data,q);
                unsigned char stype = data[q+5];
                unsigned char nspi = data[q+9];
                size_t r = q + 10;
                for (size_t t=0;t<nspi;t++) {
                    if (r+26 > len) break;
                    char dep[6]; memset(dep,0,6); memcpy(dep, data+r+2, 5);
                    uint16_t sync = b16(data,r+24);
                    if ((stype==8 || stype==9) && item_count>0) {
                        /* add pair */
                        size_t base_idx = (sync < item_count) ? sync : 0;
                        /* append */
                        p.count++;
                        p.bases = realloc(p.bases, p.count*sizeof(char*));
                        p.deps = realloc(p.deps, p.count*sizeof(char*));
                        p.bases[p.count-1] = xm(6); memset(p.bases[p.count-1],0,6); strcpy(p.bases[p.count-1], items[base_idx]);
                        p.deps[p.count-1] = xm(6); memset(p.deps[p.count-1],0,6); strcpy(p.deps[p.count-1], dep);
                    }
                    if (r+2 <= len) r += 2 + b16(data,r);
                    else break;
                }
                q += 4 + sl;
            }
        }
    }
    /* free items */
    for (size_t i=0;i<item_count;i++) free(items[i]);
    free(items);
    return p;
}

void mpls_pairs_free(MplsPairs *p) {
    if (!p) return;
    for (size_t i=0;i<p->count;i++) { free(p->bases[i]); free(p->deps[i]); }
    free(p->bases); free(p->deps);
    memset(p,0,sizeof(*p));
}

uint64_t *chunk_sizes(const Clpi *c, size_t *out_count) {
    if (!c || c->sp_count==0) { *out_count=0; return NULL; }
    uint64_t *out = xm(c->sp_count*sizeof(uint64_t));
    for (size_t i=0;i<c->sp_count;i++) {
        uint32_t next = (i+1 < c->sp_count) ? c->sp[i+1] : c->pkts;
        out[i] = (uint64_t)(next - c->sp[i]) * PKT;
    }
    *out_count = c->sp_count;
    return out;
}
