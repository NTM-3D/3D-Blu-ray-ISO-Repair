#include "udf.h"
#include "report.h"
#include <string.h>
#include <stdlib.h>

static void *xmalloc2(size_t n){ void*p=malloc(n?n:1); if(!p) udf_error("Out of memory."); return p; }
static void *xrealloc2(void*p,size_t n){ void*q=realloc(p,n?n:1); if(!q) udf_error("Out of memory."); return q; }

static int descriptor_valid(const unsigned char *data, int id) {
    uint16_t len = u16(data,10);
    if (tag_id(data) != id || len > BS-16) return 0;
    unsigned char copy[BS];
    memcpy(copy, data, BS);
    seal(copy, len);
    return u16(copy,8) == u16(data,8);
}

int udf_repair_overlapping_metadata(Udf *u, Report *rep) {
    if (u->maps_count <= 1 || u->maps[1].kind != 1) return 0;
    PartMap *m = &u->maps[1];
    if (m->mirror == UINT32_MAX || !m->has_mirror_ext ||
        !m->mirror_ext.count || u->img->size % BS) return 0;
    uint64_t n = u->img->size / BS;
    if (m->part < 0 || m->part >= 16 || !u->pd_valid[m->part])
        udf_error("Cannot inspect metadata: invalid physical partition.");
    uint64_t start = u->pd_start[m->part];
    size_t last = m->mirror_ext.count - 1;
    uint64_t mirror_end = start + m->mirror_ext.lbn[last] + m->mirror_ext.nb[last];
    if (mirror_end != n) return 0;

    if (m->ext.count != 2 || m->mirror_ext.count != 2 ||
        m->phys_ref < 0 || (size_t)m->phys_ref >= u->maps_count ||
        !m->ext.nb[0] || m->ext.nb[0] != m->mirror_ext.nb[0] ||
        !m->ext.nb[1] || m->ext.nb[1] != m->mirror_ext.nb[1] ||
        m->blocks != (uint64_t)m->ext.nb[0] + m->ext.nb[1] ||
        m->mirror_ext.lbn[1] != (uint64_t)m->ext.lbn[1] + m->ext.nb[1] ||
        (uint64_t)m->ext.lbn[1] + m->ext.nb[1] + m->mirror_ext.nb[1] != n - start ||
        u->pd_length[m->part] != n - start)
        udf_error("Cannot repair metadata: end anchor overlaps an unsupported mirror layout.");

    unsigned char *anchor = image_sector(u->img, n - 1);
    if (!descriptor_valid(anchor, 2) || u32(anchor,12) != n - 1) {
        free(anchor);
        udf_error("Cannot repair metadata: overlapping end anchor is invalid.");
    }
    free(anchor);

    uint64_t old_end = start + m->ext.lbn[1];
    if (old_end == 0 || old_end > n)
        udf_error("Cannot repair metadata: invalid original image boundary.");
    unsigned char *old_anchor = image_sector(u->img, old_end - 1);
    int old_anchor_ok = descriptor_valid(old_anchor, 2) && u32(old_anchor,12) == old_end - 1;
    free(old_anchor);
    if (!old_anchor_ok)
        udf_error("Cannot repair metadata: original end anchor is missing.");

    if (!u->integrity_len || u->integrity_len % BS ||
        (uint64_t)u->integrity_loc + u->integrity_len / BS > n)
        udf_error("Cannot repair metadata: unsupported integrity sequence.");
    uint64_t integrity_sec = 0;
    int terminated = 0;
    for (uint32_t i=0; i<u->integrity_len / BS; i++) {
        uint64_t sec = (uint64_t)u->integrity_loc + i;
        unsigned char *b = image_sector(u->img, sec);
        int id = tag_id(b);
        if (id == 8) { terminated = 1; free(b); break; }
        uint32_t count = u32(b,72);
        uint16_t crc_len = u16(b,10);
        if (id != 9 || !descriptor_valid(b, 9) || integrity_sec || u32(b,32) ||
            count != u->maps_count || count > (BS-80)/8 ||
            80 + 8 * count > 16u + crc_len) {
            free(b);
            udf_error("Cannot repair metadata: unsupported integrity descriptor.");
        }
        uint32_t old_part = u32(b,80 + 4*count + 4*m->phys_ref);
        uint32_t old_meta = u32(b,80 + 4*count + 4);
        if (old_meta != m->ext.nb[0] || old_part > old_end - start ||
            (uint64_t)m->ext.lbn[0] + m->ext.nb[0] > old_part ||
            (uint64_t)m->mirror_ext.lbn[0] + m->mirror_ext.nb[0] > old_part) {
            free(b);
            udf_error("Cannot repair metadata: integrity sizes do not match the metadata layout.");
        }
        integrity_sec = sec;
        free(b);
    }
    if (!integrity_sec || !terminated)
        udf_error("Cannot repair metadata: incomplete integrity sequence.");

    for (uint32_t k=0; k<m->blocks; k++) {
        unsigned char *primary = image_sector(u->img, udf_sector_of(u,1,k,0));
        unsigned char *mirror = image_sector(u->img, udf_sector_of(u,1,k,1));
        int differs = memcmp(primary, mirror, BS) != 0;
        if (k == m->blocks - 1) {
            for (size_t j=0; j<BS; j++) if (primary[j]) {
                free(primary); free(mirror);
                udf_error("Cannot repair metadata: overwritten mirror block is not empty in primary.");
            }
            if (!differs) {
                free(primary); free(mirror);
                udf_error("Cannot repair metadata: expected anchor overlap is absent.");
            }
        } else if (differs) {
            free(primary); free(mirror);
            udf_error("Cannot repair metadata: unexpected mirror mismatch at block %u.", k);
        }
        free(primary);
        free(mirror);
    }

    unsigned char zero[BS] = {0};
    image_put(u->img, n - 1, zero);
    unsigned char *integrity = image_sector(u->img, integrity_sec);
    uint32_t count = u32(integrity,72), physical = u->pd_length[m->part];
    uint32_t blocks = (uint32_t)m->blocks;
    memcpy(integrity + 80 + 4*count + 4*m->phys_ref, &physical, 4);
    memcpy(integrity + 80 + 4*count + 4, &blocks, 4);
    seal(integrity, u16(integrity,10));
    image_put(u->img, integrity_sec, integrity);
    free(integrity);
    udf_grow_iso(u, 1, rep);
    return 1;
}

void udf_grow_iso(Udf *u, uint64_t needed_blocks, Report *rep) {
    if (u->maps_count <= 1 || u->maps[1].kind != 1)
        udf_error("Cannot grow: expected a metadata partition at map 1.");
    PartMap *m = &u->maps[1];
    int pref = m->phys_ref, part = m->part;
    int has_mirror = m->mirror != UINT32_MAX;
    if (pref < 0 || (size_t)pref >= u->maps_count || part < 0 || part >= 16 ||
        !u->pd_valid[part] || !m->alloc_unit || !m->align_unit ||
        m->blocks % m->alloc_unit || (has_mirror && !m->has_mirror_ext))
        udf_error("Cannot grow: unsupported metadata partition layout.");
    for (size_t i=0; i<u->maps_count; i++)
        if ((int)i != pref && u->maps[i].kind == 0 && u->maps[i].part == part)
            udf_error("Cannot grow: physical partition has multiple maps.");
    if (u->img->size % BS || !needed_blocks || needed_blocks > MAX_EXT / BS)
        udf_error("Cannot grow: invalid image size or metadata request.");
    uint64_t add = needed_blocks > 128 ? needed_blocks : 128;
    add = ((add + m->alloc_unit - 1) / m->alloc_unit) * m->alloc_unit;
    uint64_t blocks = m->blocks + add;
    if (blocks > MAX_EXT / BS)
        udf_error("Cannot grow: metadata file cannot fit in one allocation extent.");
    uint64_t old_n = u->img->size / BS;
    uint64_t part_start = u->pd_start[part];
    if (old_n < part_start)
        udf_error("Cannot grow: physical partition starts beyond image.");
    uint64_t first = ((old_n - part_start + m->align_unit - 1) / m->align_unit) * m->align_unit;
    uint64_t mirror = first;
    uint64_t primary = has_mirror
        ? ((mirror + blocks + m->align_unit - 1) / m->align_unit) * m->align_unit
        : first;
    uint64_t partition_end = primary + blocks;
    uint64_t new_n = part_start + partition_end + 257;
    if (partition_end > UINT32_MAX || new_n - 1 > UINT32_MAX)
        udf_error("Cannot grow: resulting UDF locations exceed 32-bit limits.");

    Node *meta_node = udf_load(u, pref, m->meta, "metadata");
    Node *mirror_node = has_mirror ? udf_load(u, pref, m->mirror, "metadata-mirror") : NULL;
    if ((meta_node->alloc != 0 && meta_node->alloc != 1) ||
        (mirror_node && mirror_node->alloc != 0 && mirror_node->alloc != 1) ||
        !descriptor_valid(meta_node->buf, meta_node->tag) ||
        (mirror_node && !descriptor_valid(mirror_node->buf, mirror_node->tag)) ||
        meta_node->info != m->blocks * BS ||
        (mirror_node && mirror_node->info != meta_node->info))
        udf_error("Cannot grow: metadata and mirror sizes do not match.");
    for (size_t k=0; k<meta_node->ads.count; k++)
        if (meta_node->ads.items[k].type != 0 || meta_node->ads.items[k].len % BS ||
            (meta_node->ads.items[k].pref >= 0 && meta_node->ads.items[k].pref != pref) ||
            (uint64_t)meta_node->ads.items[k].lbn + meta_node->ads.items[k].len / BS > u->pd_length[part])
            udf_error("Cannot grow: metadata has unsupported extents.");
    if (mirror_node) for (size_t k=0; k<mirror_node->ads.count; k++)
        if (mirror_node->ads.items[k].type != 0 || mirror_node->ads.items[k].len % BS ||
            (mirror_node->ads.items[k].pref >= 0 && mirror_node->ads.items[k].pref != pref) ||
            (uint64_t)mirror_node->ads.items[k].lbn + mirror_node->ads.items[k].len / BS > u->pd_length[part])
            udf_error("Cannot grow: metadata mirror has unsupported extents.");

    unsigned char *anchor = image_sector(u->img, 256);
    if (!descriptor_valid(anchor, 2))
        udf_error("Cannot grow: primary volume anchor is invalid.");
    if (!u->integrity_len || u->integrity_len % BS ||
        (uint64_t)u->integrity_loc + u->integrity_len / BS > old_n)
        udf_error("Cannot grow: unsupported integrity sequence.");
    uint64_t integrity_sec = 0;
    int terminated = 0;
    for (uint32_t i=0; i<u->integrity_len / BS; i++) {
        unsigned char *b = image_sector(u->img, (uint64_t)u->integrity_loc + i);
        int id = tag_id(b);
        if (id == 8) { terminated = 1; free(b); break; }
        if (id == 9) {
            uint32_t count = u32(b,72);
            uint16_t crc_len = u16(b,10);
            if (!descriptor_valid(b, 9) || integrity_sec || u32(b,32) || count != u->maps_count ||
                count > (BS-80)/8 || crc_len > BS-16 ||
                80 + 8 * count > 16u + crc_len ||
                u32(b,80 + 4*count + 4*pref) != u->pd_length[part] ||
                u32(b,80 + 4*count + 4) != m->blocks)
                udf_error("Cannot grow: unsupported or inconsistent integrity descriptor.");
            integrity_sec = (uint64_t)u->integrity_loc + i;
        } else udf_error("Cannot grow: unsupported integrity sequence descriptor.");
        free(b);
    }
    if (!integrity_sec || !terminated) udf_error("Cannot grow: incomplete integrity sequence.");

    size_t partition_descriptors = 0;
    for (size_t i=0; i<u->pd_sectors_count; i++) {
        unsigned char *pd = image_sector(u->img, u->pd_sectors[i]);
        if (!descriptor_valid(pd, 5))
            udf_error("Cannot grow: invalid partition descriptor.");
        if (u16(pd,22) == part) {
            if (u32(pd,188) != u->pd_start[part] || u32(pd,192) != u->pd_length[part])
                udf_error("Cannot grow: inconsistent partition descriptors.");
            partition_descriptors++;
        }
        free(pd);
    }
    if (!partition_descriptors) udf_error("Cannot grow: physical partition descriptor missing.");

    for (uint32_t k=0; k<m->blocks; k++) {
        unsigned char *a = image_sector(u->img, udf_sector_of(u,1,k,0));
        if (has_mirror) {
            unsigned char *b = image_sector(u->img, udf_sector_of(u,1,k,1));
            if (memcmp(a,b,BS))
                udf_error("Cannot grow: metadata mirror differs at block %u.", k);
            free(b);
        }
        image_put(u->img, part_start + primary + k, a);
        if (has_mirror) image_put(u->img, part_start + mirror + k, a);
        free(a);
    }

    uint32_t new_p_len = (uint32_t)partition_end;
    for (size_t i=0; i<u->pd_sectors_count; i++) {
        unsigned char *pd = image_sector(u->img, u->pd_sectors[i]);
        if (tag_id(pd)==5 && u16(pd,22)==part) {
            memcpy(pd+192, &new_p_len, 4);
            uint16_t crc_len = u16(pd,10);
            if (crc_len > BS-16) udf_error("Cannot grow: invalid partition descriptor CRC length.");
            seal(pd, crc_len);
            image_put(u->img, u->pd_sectors[i], pd);
        }
        free(pd);
    }
    unsigned char *integrity = image_sector(u->img, integrity_sec);
    uint32_t count = u32(integrity,72), new_blocks = (uint32_t)blocks;
    memcpy(integrity + 80 + 4*count + 4*pref, &new_p_len, 4);
    memcpy(integrity + 80 + 4*count + 4, &new_blocks, 4);
    seal(integrity, u16(integrity,10));
    image_put(u->img, integrity_sec, integrity);
    free(integrity);
    u->pd_length[part] = new_p_len;

    Extent extent = {pref, (uint32_t)primary, blocks * BS};
    udf_rewrite_file(u, meta_node, &extent, 1, extent.len, meta_node->alloc, 0, rep);
    if (mirror_node) {
        extent.lbn = (uint32_t)mirror;
        udf_rewrite_file(u, mirror_node, &extent, 1, extent.len, mirror_node->alloc, 0, rep);
    }
    node_free(meta_node); node_free(mirror_node);
    free(m->ext.lbn); free(m->ext.nb);
    m->ext.lbn = xmalloc2(sizeof(uint32_t)); m->ext.nb = xmalloc2(sizeof(uint32_t));
    m->ext.lbn[0] = (uint32_t)primary; m->ext.nb[0] = new_blocks;
    m->ext.count = m->ext.cap = 1; m->blocks = blocks;
    if (has_mirror) {
        free(m->mirror_ext.lbn); free(m->mirror_ext.nb);
        m->mirror_ext.lbn = xmalloc2(sizeof(uint32_t)); m->mirror_ext.nb = xmalloc2(sizeof(uint32_t));
        m->mirror_ext.lbn[0] = (uint32_t)mirror; m->mirror_ext.nb[0] = new_blocks;
        m->mirror_ext.count = m->mirror_ext.cap = 1;
    }
    image_grow(u->img, new_n * BS);
    for (int i=0; i<2; i++) {
        uint32_t loc = (uint32_t)(i ? new_n-1 : new_n-257);
        memcpy(anchor+12, &loc, 4);
        seal(anchor, u16(anchor,10));
        image_put(u->img, loc, anchor);
    }
    free(anchor);
    report_say(rep, "  * [ISO Grow] Relocated metadata into contiguous extents and added %llu blocks.",
               (unsigned long long)add);
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

int udf_set_file_type(Udf *u, Node *n, uint8_t file_type) {
    if (n->ftype == file_type) return 0;
    uint16_t crc_len = u16(n->buf, 10);
    if (crc_len > BS - 16)
        udf_error("%s: Invalid descriptor CRC length %u.", n->name, crc_len);

    unsigned char *buf = xmalloc2(BS);
    memcpy(buf, n->buf, BS);
    buf[27] = file_type;
    seal(buf, crc_len);
    udf_put_block(u, n->ref, n->lbn, buf);
    memcpy(n->buf, buf, BS);
    n->ftype = file_type;
    free(buf);
    return 1;
}
