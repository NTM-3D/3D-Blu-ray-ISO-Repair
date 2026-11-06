#include "udf.h"
#include "report.h"
#include <string.h>
#include <stdlib.h>

static void *xmalloc2(size_t n){ void*p=malloc(n?n:1); if(!p) udf_error("Out of memory."); return p; }
static void *xrealloc2(void*p,size_t n){ void*q=realloc(p,n?n:1); if(!q) udf_error("Out of memory."); return q; }

void udf_grow_iso(Udf *u, uint64_t needed_blocks, Report *rep) {
    PartMap *m = &u->maps[1];
    int pref = m->phys_ref;
    uint64_t alloc_unit = 128;
    uint64_t add_blocks = ((needed_blocks + alloc_unit - 1)/alloc_unit)*alloc_unit;
    int has_mirror = (m->mirror != 0xFFFFFFFF);
    uint64_t total_add = add_blocks * (has_mirror ? 2 : 1);
    uint64_t old_iso_size = u->img->size;
    uint64_t old_N = old_iso_size / BS;
    uint64_t new_N = old_N + total_add;
    image_grow(u->img, new_N*BS);

    /* partition 0 length */
    uint64_t grown_len = (has_mirror ? (old_N + add_blocks) : old_N) - u->pd_start[0] + add_blocks;
    uint32_t new_p_len = (uint32_t)(grown_len > u->pd_length[0] ? grown_len : u->pd_length[0]);
    u->pd_length[0] = new_p_len;

    for (size_t i=0;i<u->pd_sectors_count;i++) {
        unsigned char *pd = image_sector(u->img, u->pd_sectors[i]);
        if (tag_id(pd)==5) {
            pd[192]=(unsigned char)(new_p_len & 0xFF);
            pd[193]=(unsigned char)((new_p_len>>8)&0xFF);
            pd[194]=(unsigned char)((new_p_len>>16)&0xFF);
            pd[195]=(unsigned char)((new_p_len>>24)&0xFF);
            uint16_t crclen = u16(pd,10);
            if (crclen) seal(pd, crclen);
            image_put(u->img, u->pd_sectors[i], pd);
        }
        free(pd);
    }

    /* meta node */
    Node *meta_node = udf_load(u, pref, m->meta, "metadata");
    uint64_t meta_phys_lbn = old_N - u->pd_start[0];
    size_t ec = m->ext.count + 1;
    Extent *exts = xmalloc2(ec*sizeof(Extent));
    size_t ei=0;
    for (size_t k=0;k<m->ext.count;k++) {
        exts[ei].ref=pref; exts[ei].lbn=m->ext.lbn[k]; exts[ei].len=(uint64_t)m->ext.nb[k]*BS; ei++;
    }
    exts[ei].ref=pref; exts[ei].lbn=(uint32_t)meta_phys_lbn; exts[ei].len=add_blocks*BS; ei++;
    uint64_t total_len=0; for(size_t k=0;k<ei;k++) total_len+=exts[k].len;
    udf_rewrite_file(u, meta_node, exts, ei, total_len, meta_node->alloc, 0, rep);
    /* update ext list */
    free(m->ext.lbn); free(m->ext.nb); m->ext.lbn=NULL; m->ext.nb=NULL; m->ext.count=0; m->ext.cap=0;
    for (size_t k=0;k<ei;k++) {
        if (m->ext.count==m->ext.cap) { m->ext.cap=m->ext.cap?m->ext.cap*2:16; m->ext.lbn=xrealloc2(m->ext.lbn,m->ext.cap*sizeof(uint32_t)); m->ext.nb=xrealloc2(m->ext.nb,m->ext.cap*sizeof(uint32_t)); }
        m->ext.lbn[m->ext.count]=exts[k].lbn;
        m->ext.nb[m->ext.count]=(uint32_t)(exts[k].len/BS);
        m->ext.count++;
    }
    m->blocks=0; for(size_t k=0;k<m->ext.count;k++) m->blocks+=m->ext.nb[k];
    free(exts);
    node_free(meta_node);

    if (has_mirror) {
        Node *mirror_node = udf_load(u, pref, m->mirror, "metadata-mirror");
        uint64_t mirror_phys_lbn = (old_N + add_blocks) - u->pd_start[0];
        size_t ec2 = m->mirror_ext.count + 1;
        Extent *exts2 = xmalloc2(ec2*sizeof(Extent));
        size_t ei2=0;
        for (size_t k=0;k<m->mirror_ext.count;k++) { exts2[ei2].ref=pref; exts2[ei2].lbn=m->mirror_ext.lbn[k]; exts2[ei2].len=(uint64_t)m->mirror_ext.nb[k]*BS; ei2++; }
        exts2[ei2].ref=pref; exts2[ei2].lbn=(uint32_t)mirror_phys_lbn; exts2[ei2].len=add_blocks*BS; ei2++;
        uint64_t total2=0; for(size_t k=0;k<ei2;k++) total2+=exts2[k].len;
        udf_rewrite_file(u, mirror_node, exts2, ei2, total2, mirror_node->alloc, 0, rep);
        free(m->mirror_ext.lbn); free(m->mirror_ext.nb); m->mirror_ext.lbn=NULL; m->mirror_ext.nb=NULL; m->mirror_ext.count=0; m->mirror_ext.cap=0;
        for (size_t k=0;k<ei2;k++) {
            if (m->mirror_ext.count==m->mirror_ext.cap) { m->mirror_ext.cap=m->mirror_ext.cap?m->mirror_ext.cap*2:16; m->mirror_ext.lbn=xrealloc2(m->mirror_ext.lbn,m->mirror_ext.cap*sizeof(uint32_t)); m->mirror_ext.nb=xrealloc2(m->mirror_ext.nb,m->mirror_ext.cap*sizeof(uint32_t)); }
            m->mirror_ext.lbn[m->mirror_ext.count]=exts2[k].lbn;
            m->mirror_ext.nb[m->mirror_ext.count]=(uint32_t)(exts2[k].len/BS);
            m->mirror_ext.count++;
        }
        free(exts2);
        node_free(mirror_node);
    }

    /* AVDP */
    uint64_t old_avdp_sec = old_N - 1;
    unsigned char *old_avdp = image_sector(u->img, old_avdp_sec);
    if (tag_id(old_avdp)==2) {
        unsigned char *new_avdp = xmalloc2(BS); memcpy(new_avdp, old_avdp, BS);
        uint64_t new_avdp_sec = new_N - 1;
        new_avdp[12]=(unsigned char)(new_avdp_sec & 0xFF);
        new_avdp[13]=(unsigned char)((new_avdp_sec>>8)&0xFF);
        new_avdp[14]=(unsigned char)((new_avdp_sec>>16)&0xFF);
        new_avdp[15]=(unsigned char)((new_avdp_sec>>24)&0xFF);
        uint16_t crclen = u16(new_avdp,10);
        if (crclen) seal(new_avdp, crclen);
        image_put(u->img, new_avdp_sec, new_avdp);
        free(new_avdp);
    }
    free(old_avdp);

    uint64_t old_res_avdp_sec = (old_N >= 257) ? old_N - 257 : 0;
    unsigned char *old_res = image_sector(u->img, old_res_avdp_sec);
    if (tag_id(old_res)==2) {
        unsigned char *new_res = xmalloc2(BS); memcpy(new_res, old_res, BS);
        uint64_t new_res_sec = new_N - 257;
        new_res[12]=(unsigned char)(new_res_sec & 0xFF);
        new_res[13]=(unsigned char)((new_res_sec>>8)&0xFF);
        new_res[14]=(unsigned char)((new_res_sec>>16)&0xFF);
        new_res[15]=(unsigned char)((new_res_sec>>24)&0xFF);
        uint16_t crclen = u16(new_res,10);
        if (crclen) seal(new_res, crclen);
        image_put(u->img, new_res_sec, new_res);
        free(new_res);
    }
    free(old_res);

    report_say(rep, "  * [ISO Grow] Expanded ISO by %llu sectors (%llu KiB) to provide %llu metadata blocks.",
               (unsigned long long)total_add, (unsigned long long)(total_add*BS/1024),
               (unsigned long long)add_blocks);
}

uint32_t *udf_alloc_blocks(Udf *u, int ref, size_t count, size_t *out_count, Report *rep) {
    PartMap *m = &u->maps[ref];
    if (!(m->kind==1 && m->bitmap==0xFFFFFFFF)) udf_error("Unsupported allocation partition %d.", ref);
    uint32_t *got = xmalloc2((count?count:1)*sizeof(uint32_t));
    size_t gotn=0;
    UintSet *used = &m->ro_used;
    int try_pass=0;
    while (gotn < count) {
        for (uint64_t lbn=0; lbn<m->blocks && gotn<count; lbn++) {
            int isused=0;
            for (size_t i=0;i<used->count;i++) if (used->vals[i]==(uint32_t)lbn) { isused=1; break; }
            if (isused) continue;
            uint64_t sec = udf_sector_of(u, ref, (uint32_t)lbn, 0);
            unsigned char *s = image_sector(u->img, sec);
            int zero=1;
            for (int i=0;i<BS;i++) if (s[i]!=0) { zero=0; break; }
            free(s);
            if (zero) {
                got[gotn++] = (uint32_t)lbn;
                if (used->count==used->cap) { used->cap=used->cap?used->cap*2:64; used->vals=xrealloc2(used->vals,used->cap*sizeof(uint32_t)); }
                used->vals[used->count++] = (uint32_t)lbn;
            }
        }
        if (gotn < count) {
            if (try_pass) break;
            try_pass=1;
            udf_grow_iso(u, count-gotn, rep);
        }
    }
    if (gotn < count) udf_error("Metadata partition has only %zu free blocks, %zu needed.", gotn, count);
    *out_count = gotn;
    return got;
}

int udf_rewrite_file(Udf *u, Node *n, Extent *exts, size_t ext_count, uint64_t length, int alloc, int counts_aed, Report *rep) {
    int s = (alloc==0)?8:16;
    if (alloc==0) {
        PartMap *m = &u->maps[n->ref];
        int want = (m->kind==1) ? m->phys_ref : n->ref;
        for (size_t i=0;i<ext_count;i++) if (exts[i].ref != want) udf_error("%s: Extents live in a different partition than short allocation descriptors can address.", n->name);
    }
    size_t dat_len = ext_count*s;
    unsigned char *dat = xmalloc2(dat_len?dat_len:1);
    size_t p=0;
    for (size_t i=0;i<ext_count;i++) {
        if (exts[i].len > MAX_EXT) udf_error("Internal: Extent too long.");
        if (alloc==0) {
            uint32_t ln=(uint32_t)exts[i].len; uint32_t l=exts[i].lbn;
            memcpy(dat+p,&ln,4); p+=4; memcpy(dat+p,&l,4); p+=4;
        } else {
            uint32_t ln=(uint32_t)exts[i].len; uint32_t l=exts[i].lbn; uint16_t r=(uint16_t)exts[i].ref;
            memcpy(dat+p,&ln,4); p+=4; memcpy(dat+p,&l,4); p+=4; memcpy(dat+p,&r,2); p+=2; memset(dat+p,0,6); p+=6;
        }
    }
    size_t fe_cap = (BS - n->hdr - n->ea) / s;
    size_t a_cap = (BS - 24) / s;
    if (fe_cap == 0 || a_cap < 2) udf_error("%s: File entry has no room for allocation descriptors.", n->name);
    size_t n_aed;
    if (ext_count <= fe_cap) n_aed=0;
    else {
        size_t rest = ext_count - (fe_cap - 1);
        n_aed=1;
        while (rest > a_cap) { rest -= a_cap - 1; n_aed++; }
    }
    int aed_ref = (u->learned_aed_ref != -1) ? u->learned_aed_ref : n->ref;
    uint32_t *blocks = xmalloc2((n_aed?n_aed:1)*sizeof(uint32_t));
    size_t blocks_count=0;
    for (size_t i=0;i<n->chain.count;i++) {
        if (n->chain.items[i].ref == aed_ref && blocks_count < n_aed) {
            blocks[blocks_count++] = n->chain.items[i].lbn;
            PartMap *mm = &u->maps[aed_ref];
            if (mm->ro_used.count==mm->ro_used.cap) { mm->ro_used.cap=mm->ro_used.cap?mm->ro_used.cap*2:64; mm->ro_used.vals=xrealloc2(mm->ro_used.vals,mm->ro_used.cap*sizeof(uint32_t)); }
            mm->ro_used.vals[mm->ro_used.count++] = n->chain.items[i].lbn;
        }
    }
    if (n_aed > blocks_count) {
        size_t need = n_aed - blocks_count;
        size_t gotn=0;
        uint32_t *got = udf_alloc_blocks(u, aed_ref, need, &gotn, rep);
        for (size_t i=0;i<gotn;i++) blocks[blocks_count++] = got[i];
        free(got);
    }
    /* rewrite AEDs */
    size_t pos = n_aed ? (fe_cap - 1) : 0;
    uint32_t prev = n->lbn;
    for (size_t k=0;k<n_aed;k++) {
        size_t take = (k < n_aed-1) ? (a_cap - 1) : (ext_count - pos);
        if (take > a_cap || take > ext_count - pos)
            udf_error("%s: Internal allocation descriptor accounting error.", n->name);
        size_t body_bytes = take * s;
        unsigned char *body = xmalloc2(body_bytes + (k<n_aed-1?s:0));
        size_t body_len = body_bytes;
        memcpy(body, dat + pos*s, body_bytes);
        pos += take;
        if (k < n_aed-1) {
            uint32_t w = (3u<<30) | BS;
            if (alloc==0) {
                memcpy(body+body_len,&w,4); body_len+=4;
                uint32_t lbnv=blocks[k+1]; memcpy(body+body_len,&lbnv,4); body_len+=4;
            } else {
                memcpy(body+body_len,&w,4); body_len+=4;
                uint32_t lbnv=blocks[k+1]; memcpy(body+body_len,&lbnv,4); body_len+=4;
                uint16_t r=(uint16_t)aed_ref; memcpy(body+body_len,&r,2); body_len+=2;
                memset(body+body_len,0,6); body_len+=6;
            }
        }
        unsigned char *blk = xmalloc2(BS); memset(blk,0,BS);
        uint16_t tid=258;
        uint16_t ver=u16(n->buf,2);
        uint16_t serial=u16(n->buf,6);
        uint32_t bias = u32(n->buf,12) - n->lbn;
        blk[0]=(unsigned char)(tid&0xFF); blk[1]=(unsigned char)(tid>>8);
        blk[2]=(unsigned char)(ver&0xFF); blk[3]=(unsigned char)(ver>>8);
        blk[6]=(unsigned char)(serial&0xFF); blk[7]=(unsigned char)(serial>>8);
        uint32_t tagloc = blocks[k] + bias;
        memcpy(blk+12,&tagloc,4);
        memcpy(blk+16,&prev,4);
        uint32_t adlen=(uint32_t)body_len;
        memcpy(blk+20,&adlen,4);
        memcpy(blk+24,body,body_len);
        seal(blk, 8 + (int)body_len);
        udf_put_block(u, aed_ref, blocks[k], blk);
        prev = blocks[k];
        free(body); free(blk);
    }
    /* rewrite FE */
    unsigned char *buf = xmalloc2(BS); memcpy(buf, n->buf, BS);
    uint16_t allocfield = u16(buf,34);
    allocfield = (allocfield & ~7) | (uint16_t)alloc;
    buf[34]=(unsigned char)(allocfield&0xFF);
    buf[35]=(unsigned char)(allocfield>>8);
    memcpy(buf+56, &length, 8);
    uint64_t lbr = (length + BS - 1)/BS + (counts_aed ? n_aed : 0);
    if (n->tag==261) {
        memcpy(buf+64, &lbr, 8);
    } else {
        memcpy(buf+64, &length, 8);
        memcpy(buf+72, &lbr, 8);
    }
    size_t lad_off = (n->tag==261)?172:212;
    size_t start = n->hdr + n->ea;
    memset(buf+start, 0, BS-start);
    unsigned char *fe_ads;
    size_t fe_ads_len;
    if (n_aed) {
        size_t first_count = fe_cap - 1;
        fe_ads_len = first_count*s + s;
        fe_ads = xmalloc2(fe_ads_len);
        memcpy(fe_ads, dat, first_count*s);
        size_t off2 = first_count*s;
        uint32_t w = (3u<<30) | BS;
        if (alloc==0) {
            memcpy(fe_ads+off2,&w,4); off2+=4;
            uint32_t lbnv=blocks[0]; memcpy(fe_ads+off2,&lbnv,4); off2+=4;
        } else {
            memcpy(fe_ads+off2,&w,4); off2+=4;
            uint32_t lbnv=blocks[0]; memcpy(fe_ads+off2,&lbnv,4); off2+=4;
            uint16_t r=(uint16_t)aed_ref; memcpy(fe_ads+off2,&r,2); off2+=2;
            memset(fe_ads+off2,0,6); off2+=6;
        }
    } else {
        fe_ads = xmalloc2(dat_len?dat_len:1);
        memcpy(fe_ads, dat, dat_len);
        fe_ads_len = dat_len;
    }
    memcpy(buf+start, fe_ads, fe_ads_len);
    uint32_t ladval = (uint32_t)fe_ads_len;
    memcpy(buf+lad_off, &ladval, 4);
    seal(buf, start + (int)fe_ads_len - 16);
    udf_put_block(u, n->ref, n->lbn, buf);
    free(buf); free(fe_ads); free(dat); free(blocks);
    return (int)n_aed;
}
