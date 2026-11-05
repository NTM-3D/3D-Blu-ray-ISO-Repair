#include "repair.h"
#include "report.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <strings.h>

#define KIND_A 1  /* rebuild m2ts files from SSIF */
#define KIND_B 2  /* rebuild SSIF from m2ts files */

static void *xm(size_t n){ void*p=malloc(n?n:1); if(!p) udf_error("Out of memory."); return p; }

static int ends_with_ci(const char *s, const char *suf) {
    size_t ls=strlen(s), lu=strlen(suf);
    if (lu>ls) return 0;
    return strcasecmp(s+ls-lu, suf)==0;
}

static DirEntry *find_entry(DirList *dl, const char *name) {
    for (size_t i=0;i<dl->count;i++) if (strcasecmp(dl->items[i].name,name)==0) return &dl->items[i];
    return NULL;
}

static void fmt_size(uint64_t n, char *buf, size_t bufsize) {
    if (n >= (1ULL<<30)) snprintf(buf,bufsize,"%.2f GiB",(double)n/(1024.0*1024*1024));
    else if (n >= (1ULL<<20)) snprintf(buf,bufsize,"%.2f MiB",(double)n/(1024.0*1024));
    else if (n >= 1024) snprintf(buf,bufsize,"%.2f KiB",(double)n/1024.0);
    else snprintf(buf,bufsize,"%llu bytes",(unsigned long long)n);
}

static Node *load_m2ts(Udf *u, DirList *streamdir, const char *stem) {
    char name[256]; snprintf(name,sizeof(name),"%s.M2TS",stem);
    DirEntry *e = find_entry(streamdir, name);
    if (!e) return NULL;
    return udf_load(u, e->ref, e->lbn, e->name);
}

int run_undo(const char *path) { return image_undo(path); }

/* ------------------------------------------------------------------ */
/* Collections                                                         */
/* ------------------------------------------------------------------ */

typedef struct { char **items; size_t count,cap; } StrArr;
typedef struct { char base[64]; char dep[64]; } Pair;
typedef struct { Pair *items; size_t count,cap; } PairArr;
typedef struct { char stem[64]; Clpi c; } ClpiEntry;
typedef struct { ClpiEntry *items; size_t count,cap; } ClpiArr;
typedef struct { Extent *items; size_t count,cap; } ExtArr;

static void sa_push(StrArr *a,const char *s){
    if(a->count==a->cap){a->cap=a->cap?a->cap*2:16; a->items=realloc(a->items,a->cap*sizeof(char*)); if(!a->items)udf_error("Out of memory.");}
    a->items[a->count++]=strdup(s);
}
static void pair_set(PairArr *a,const char *b,const char *d){
    for(size_t i=0;i<a->count;i++){
        if(strcasecmp(a->items[i].base,b)==0){ strcpy(a->items[i].dep,d); return; }
    }
    if(a->count==a->cap){a->cap=a->cap?a->cap*2:16; a->items=realloc(a->items,a->cap*sizeof(Pair)); if(!a->items)udf_error("Out of memory.");}
    strcpy(a->items[a->count].base,b); strcpy(a->items[a->count].dep,d); a->count++;
}
static const char *pair_find(PairArr *a,const char *base){
    for(size_t i=0;i<a->count;i++) if(strcasecmp(a->items[i].base,base)==0) return a->items[i].dep;
    return NULL;
}
static Clpi *clpi_find(ClpiArr *a,const char *stem){
    for(size_t i=0;i<a->count;i++) if(strcasecmp(a->items[i].stem,stem)==0) return &a->items[i].c;
    return NULL;
}
static void ea_push(ExtArr *a,Extent e){
    if(a->count==a->cap){a->cap=a->cap?a->cap*2:16; a->items=realloc(a->items,a->cap*sizeof(Extent)); if(!a->items)udf_error("Out of memory.");}
    a->items[a->count++]=e;
}

static ExtArr slice_extents(Extent *ex,size_t ex_count,uint64_t off,uint64_t length){
    ExtArr out; memset(&out,0,sizeof(out));
    uint64_t pos=0,end=off+length;
    for(size_t i=0;i<ex_count;i++){
        uint64_t s=(off>pos)?off:pos;
        uint64_t e=(end<pos+ex[i].len)?end:pos+ex[i].len;
        if(s<e){
            if((s-pos)%BS) udf_error("Chunk boundary is not block aligned.");
            Extent ne; ne.ref=ex[i].ref; ne.lbn=ex[i].lbn+(uint32_t)((s-pos)/BS); ne.len=e-s;
            ea_push(&out,ne);
        }
        pos+=ex[i].len;
        if(pos>=end) break;
    }
    uint64_t tot=0; for(size_t i=0;i<out.count;i++) tot+=out.items[i].len;
    if(tot!=length) udf_error("Source file is shorter than the layout requires.");
    return out;
}

static int node_ok(Udf *u,Node *n,uint64_t expected,size_t min_extents){
    if(!n) return 0;
    size_t ec; Extent *ex=extents_of(u,n,&ec);
    if(!ex) return 0;
    if(n->info!=expected){free(ex);return 0;}
    uint64_t total=0; for(size_t i=0;i<ec;i++) total+=ex[i].len;
    if(total!=expected){free(ex);return 0;}
    if(ec<min_extents){free(ex);return 0;}
    for(size_t i=0;i<ec;i++){
        uint64_t sec=udf_sector_of(u,ex[i].ref,ex[i].lbn,0);
        if(sec*BS+ex[i].len>u->img->size){free(ex);return 0;}
    }
    free(ex);
    return 1;
}

static int is_physically_contiguous(Extent *ex,size_t n){
    if(n==0) return 0;
    for(size_t i=1;i<n;i++){
        if(ex[i-1].ref!=ex[i].ref) return 0;
        if(ex[i].lbn != ex[i-1].lbn + (ex[i-1].len+BS-1)/BS) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Interleaving layout                                                 */
/* ------------------------------------------------------------------ */

typedef struct { char *keys; int *idxs; uint64_t *sizes; size_t count; } Layout;

static void layout_free(Layout *l){ free(l->keys); free(l->idxs); free(l->sizes); memset(l,0,sizeof(*l)); }

static void interleave_layout(const int *cB,int nB,const int *cD,int nD,const char *order,Layout *out){
    memset(out,0,sizeof(*out));
    if(nB<=0||nD<=0) udf_error("Empty CLPI interleaving table.");
    int paired=(nB<nD)?nB:nD;
    size_t count=(size_t)paired*2;
    if(nB!=nD){ if(nD==1) count+=(size_t)(nB-1); else if(nB==1) count+=(size_t)(nD-1); else udf_error("Unsupported asymmetric CLPI tables (%d base, %d dependent chunks).",nB,nD); }
    out->keys=xm(count);
    out->idxs=xm(count*sizeof(int));
    out->sizes=xm(count*sizeof(uint64_t));
    out->count=count;
    size_t at=0;
    for(int i=0;i<paired;i++){
        for(int o=0;order[o];o++){
            char k=order[o];
            if(at>=count) udf_error("Internal interleaving layout error.");
            out->keys[at]=k;
            out->idxs[at]=i;
            out->sizes[at]=(k=='B')?(uint64_t)cB[i]:(uint64_t)cD[i];
            at++;
        }
    }
    if(nB==nD) return;
    if(nD==1){ for(int i=1;i<nB;i++){ out->keys[at]='B'; out->idxs[at]=i; out->sizes[at]=(uint64_t)cB[i]; at++; } }
    else { for(int i=1;i<nD;i++){ out->keys[at]='D'; out->idxs[at]=i; out->sizes[at]=(uint64_t)cD[i]; at++; } }
}

typedef struct { ExtArr *chunks; size_t count,cap; } ChunkVec;
typedef struct { ChunkVec B,D; } ChunkSet;

static void chunkvec_push(ChunkVec *v,ExtArr e){
    if(v->count==v->cap){v->cap=v->cap?v->cap*2:16; v->chunks=realloc(v->chunks,v->cap*sizeof(ExtArr)); if(!v->chunks)udf_error("Out of memory.");}
    v->chunks[v->count++]=e;
}
static void chunkset_free(ChunkSet *s){
    if(!s) return;
    for(size_t i=0;i<s->B.count;i++) free(s->B.chunks[i].items);
    for(size_t i=0;i<s->D.count;i++) free(s->D.chunks[i].items);
    free(s->B.chunks); free(s->D.chunks);
    memset(s,0,sizeof(*s));
}

/* Layout order puts B and D chunks into alternating lists. */
static void build_per_chunk(Extent *exS,size_t exS_count,const int *cB,int nB,const int *cD,int nD,const char *order,ChunkSet *out){
    memset(out,0,sizeof(*out));
    Layout l; interleave_layout(cB,nB,cD,nD,order,&l);
    uint64_t pos=0;
    for(size_t i=0;i<l.count;i++){
        ExtArr p=slice_extents(exS,exS_count,pos,l.sizes[i]);
        chunkvec_push(l.keys[i]=='B'?&out->B:&out->D,p);
        pos+=l.sizes[i];
    }
    layout_free(&l);
}

static int try_build_per_chunk(Extent *exS,size_t exS_count,const int *cB,int nB,const int *cD,int nD,const char *order,ChunkSet *out){
    ErrCtx *saved=g_err;
    memset(out,0,sizeof(*out));
    if(!saved){ build_per_chunk(exS,exS_count,cB,nB,cD,nD,order,out); return 1; }
    ErrCtx local; g_err=&local;
    int ok=1;
    if(setjmp(local.jb)) ok=0;
    else build_per_chunk(exS,exS_count,cB,nB,cD,nD,order,out);
    g_err=saved;
    if(!ok) chunkset_free(out);
    return ok;
}

static void flatten_chunks(ChunkVec *v,ExtArr *out){
    for(size_t i=0;i<v->count;i++) for(size_t j=0;j<v->chunks[i].count;j++) ea_push(out,v->chunks[i].items[j]);
}

static int extents_match_ssif(Udf *u,Node *B,Node *D,Extent *exS,size_t exS_count,const int *cB,int nB,const int *cD,int nD){
    size_t ecB,ecD;
    Extent *actB=extents_of(u,B,&ecB);
    Extent *actD=extents_of(u,D,&ecD);
    if(!actB||!actD){ free(actB); free(actD); return 1; }
    const char *orders[2]={"DB","BD"};
    for(int oi=0;oi<2;oi++){
        ChunkSet cs;
        if(!try_build_per_chunk(exS,exS_count,cB,nB,cD,nD,orders[oi],&cs)) continue;
        ExtArr flatB,flatD; memset(&flatB,0,sizeof(flatB)); memset(&flatD,0,sizeof(flatD));
        flatten_chunks(&cs.B,&flatB);
        flatten_chunks(&cs.D,&flatD);
        int matchB=(flatB.count==ecB);
        if(matchB) for(size_t i=0;i<ecB;i++) if(flatB.items[i].ref!=actB[i].ref||flatB.items[i].lbn!=actB[i].lbn||flatB.items[i].len!=actB[i].len){matchB=0;break;}
        int matchD=(flatD.count==ecD);
        if(matchD) for(size_t i=0;i<ecD;i++) if(flatD.items[i].ref!=actD[i].ref||flatD.items[i].lbn!=actD[i].lbn||flatD.items[i].len!=actD[i].len){matchD=0;break;}
        free(flatB.items); free(flatD.items); chunkset_free(&cs);
        if(matchB&&matchD){ free(actB); free(actD); return 1; }
    }
    free(actB); free(actD);
    return 0;
}

/* ------------------------------------------------------------------ */
/* SSIF rebuilding helpers                                             */
/* ------------------------------------------------------------------ */

static ExtArr build_ssif_from_views(Extent *exB,size_t exB_count,Extent *exD,size_t exD_count,const int *cB,int nB,const int *cD,int nD,const char *order){
    ExtArr *viewB=xm((nB?nB:1)*sizeof(ExtArr));
    ExtArr *viewD=xm((nD?nD:1)*sizeof(ExtArr));
    memset(viewB,0,(nB?nB:1)*sizeof(ExtArr));
    memset(viewD,0,(nD?nD:1)*sizeof(ExtArr));
    uint64_t pos=0;
    for(int i=0;i<nB;i++){ viewB[i]=slice_extents(exB,exB_count,pos,(uint64_t)cB[i]); pos+=(uint64_t)cB[i]; }
    pos=0;
    for(int i=0;i<nD;i++){ viewD[i]=slice_extents(exD,exD_count,pos,(uint64_t)cD[i]); pos+=(uint64_t)cD[i]; }
    ExtArr out; memset(&out,0,sizeof(out));
    Layout l; interleave_layout(cB,nB,cD,nD,order,&l);
    for(size_t k=0;k<l.count;k++){
        Extent *vp = (l.keys[k]=='B') ? viewB[l.idxs[k]].items : viewD[l.idxs[k]].items;
        size_t vc = (l.keys[k]=='B') ? viewB[l.idxs[k]].count : viewD[l.idxs[k]].count;
        for(size_t j=0;j<vc;j++){
            if(out.count>0 && out.items[out.count-1].ref==vp[j].ref &&
               out.items[out.count-1].lbn + (out.items[out.count-1].len+BS-1)/BS == vp[j].lbn &&
               out.items[out.count-1].len + vp[j].len <= MAX_EXT) {
                out.items[out.count-1].len += vp[j].len;
            } else ea_push(&out,vp[j]);
        }
    }
    layout_free(&l);
    for(int i=0;i<nB;i++) free(viewB[i].items);
    for(int i=0;i<nD;i++) free(viewD[i].items);
    free(viewB); free(viewD);
    return out;
}

static int ssif_alignment_ok(Extent *exS,size_t exS_count,const int *cB,int nB,const int *cD,int nD,const char *order){
    Layout l; interleave_layout(cB,nB,cD,nD,order,&l);
    uint64_t *starts=xm((l.count+1)*sizeof(uint64_t));
    size_t sc=0; starts[sc++]=0;
    uint64_t pos=0;
    for(size_t i=0;i<l.count;i++){ pos+=l.sizes[i]; starts[sc++]=pos; }
    uint64_t off=0; int bad=0;
    for(size_t i=0;i<exS_count;i++){
        if(i>0){
            int found=0;
            for(size_t j=0;j<sc;j++) if(starts[j]==off){found=1;break;}
            if(!found){bad=1;break;}
        }
        off+=exS[i].len;
    }
    free(starts); layout_free(&l);
    return !bad;
}

static int build_aligned_ssif_extents(Extent *exS,size_t exS_count,const int *cB,int nB,const int *cD,int nD,const char *order,ExtArr *out){
    memset(out,0,sizeof(*out));
    if(!exS || !is_physically_contiguous(exS,exS_count)) return 0;
    uint64_t total=0; for(size_t i=0;i<exS_count;i++) total+=exS[i].len;
    int ref=exS[0].ref; uint32_t start_lbn=exS[0].lbn;
    Layout l; interleave_layout(cB,nB,cD,nD,order,&l);
    uint64_t pos=0,cur=0;
    for(size_t i=0;i<l.count;i++){
        uint64_t sz=l.sizes[i];
        if(sz==0 || sz>MAX_EXT){ layout_free(&l); return 0; }
        if(cur==0) cur=sz;
        else if(cur+sz<=MAX_EXT) cur+=sz;
        else {
            if(pos%BS){ layout_free(&l); return 0; }
            Extent e; e.ref=ref; e.lbn=start_lbn+(uint32_t)(pos/BS); e.len=cur; ea_push(out,e);
            pos+=cur; cur=sz;
        }
    }
    if(cur){
        if(pos%BS){ layout_free(&l); return 0; }
        Extent e; e.ref=ref; e.lbn=start_lbn+(uint32_t)(pos/BS); e.len=cur; ea_push(out,e);
        pos+=cur;
    }
    layout_free(&l);
    if(pos!=total){ free(out->items); memset(out,0,sizeof(*out)); return 0; }
    for(size_t i=0;i<out->count;i++) if(out->items[i].len==0 || out->items[i].len>MAX_EXT){ free(out->items); memset(out,0,sizeof(*out)); return 0; }
    return 1;
}

static int sync_ok_safe(Udf *u,int ref,uint32_t lbn){
    ErrCtx *saved=g_err;
    if(!saved) return udf_sync_ok(u,ref,lbn);
    ErrCtx local; int ok=0;
    g_err=&local;
    if(setjmp(local.jb)==0) ok=udf_sync_ok(u,ref,lbn);
    g_err=saved;
    return ok;
}

/* Sorted unique sample indexes: {0,1,2,n/2,n-3,n-2,n-1} within range. */
static size_t sample_idx(size_t n,size_t *out){
    long long cand[7];
    int nc=0;
    cand[nc++]=0; cand[nc++]=1; cand[nc++]=2;
    cand[nc++]=(long long)(n/2);
    cand[nc++]=(long long)n-3; cand[nc++]=(long long)n-2; cand[nc++]=(long long)n-1;
    size_t count=0;
    for(int i=0;i<nc;i++){
        if(cand[i]<0 || (unsigned long long)cand[i]>=n) continue;
        size_t v=(size_t)cand[i];
        int dup=0; for(size_t j=0;j<count;j++) if(out[j]==v){dup=1;break;}
        if(!dup) out[count++]=v;
    }
    for(size_t i=1;i<count;i++){ size_t t=out[i]; size_t j=i; while(j>0&&out[j-1]>t){out[j]=out[j-1];j--;} out[j]=t; }
    return count;
}

static void stem_of_name(const char *name,char *out,size_t outsize){
    size_t i=0; while(name[i] && name[i]!='.' && i+1<outsize){ out[i]=name[i]; i++; }
    out[i]=0;
}

/* ------------------------------------------------------------------ */
/* Repair planner                                                      */
/* ------------------------------------------------------------------ */

static void plan_pair(Udf *u,Options *opt,Report *rep,int kind,const char *stem,const char *dstem,
                      Node *B,Node *D,Node *S,const int *cB,int nB,const int *cD,int nD,int okB,int okD){
    (void)stem; (void)dstem;
    int n=nB;
    int ref_alloc=S->alloc;
    if(ref_alloc!=0 && ref_alloc!=1) udf_error("Reference file uses embedded allocation.");
    int chosen_alloc=ref_alloc;

    size_t exS_count=0;
    Extent *exS=extents_of(u,S,&exS_count);
    if(!exS) udf_error("SSIF allocation is not readable.");

    const char *orders[2];
    int norders=0;
    if(opt->order[0]) orders[norders++]=opt->order;
    else { orders[norders++]="DB"; orders[norders++]="BD"; }

    const char *chosen=NULL;
    for(int oi=0;oi<norders;oi++){
        const char *order=orders[oi];
        if(!opt->no_data_check){
            ChunkSet cs;
            if(!try_build_per_chunk(exS,exS_count,cB,nB,cD,nD,order,&cs)) continue;
            Extent probes[32];
            size_t np=0;
            size_t idxs[7]; size_t ni=sample_idx((size_t)n,idxs);
            const char *keys="BD";
            for(int ki=0;ki<2;ki++){
                ChunkVec *v = (keys[ki]=='B')?&cs.B:&cs.D;
                for(size_t ii=0;ii<ni;ii++){
                    size_t i=idxs[ii];
                    if(i<v->count && v->chunks[i].count>0 && np<32) probes[np++]=v->chunks[i].items[0];
                }
            }
            int allok=(np>0);
            for(size_t pi=0;pi<np && allok;pi++) if(!sync_ok_safe(u,probes[pi].ref,probes[pi].lbn)) allok=0;
            chunkset_free(&cs);
            if(allok){ chosen=order; break; }
        } else { chosen=order; break; }
    }
    if(!chosen) udf_error("Neither DB nor BD interleaving order matched the TS packet data on disc.");
    report_say(rep,"   Interleaving chunk order: %c,%c",chosen[0],chosen[1]);

    char avail[128];

    if(kind==KIND_B){
        stem_of_name(S->name,avail,sizeof(avail));
        report_repair_kind(rep,avail,"SSIF file table repair");
        size_t ecB,ecD;
        Extent *exB=extents_of(u,B,&ecB);
        Extent *exD=extents_of(u,D,&ecD);
        if(!exB || !exD){ free(exB); free(exD); udf_error("Valid view-file allocations could not be read."); }
        ExtArr newS=build_ssif_from_views(exB,ecB,exD,ecD,cB,nB,cD,nD,chosen);
        uint64_t expected=0; for(int i=0;i<nB;i++) expected+=(uint64_t)cB[i]; for(int i=0;i<nD;i++) expected+=(uint64_t)cD[i];
        uint64_t sum=0; for(size_t i=0;i<newS.count;i++) sum+=newS.items[i].len;
        if(sum!=expected){ free(exB); free(exD); free(newS.items); udf_error("Rebuilt SSIF extents do not sum to the stream length."); }
        int counts_aed = S->chain.count>0 && S->lbr == (S->info+BS-1)/BS + S->chain.count;
        int n_aed=udf_rewrite_file(u,S,newS.items,newS.count,expected,chosen_alloc,counts_aed,rep);
        char oldsz[64],newsz[64]; fmt_size(S->info,oldsz,sizeof(oldsz)); fmt_size(expected,newsz,sizeof(newsz));
        if(n_aed) report_say(rep,"  * %s: %s -> %s (%zu extents, %d descriptor block(s))",S->name,oldsz,newsz,newS.count,n_aed);
        else report_say(rep,"  * %s: %s -> %s (%zu extents)",S->name,oldsz,newsz,newS.count);
        report_change(rep,S->name,S->info,expected,newS.count,(size_t)n_aed);
        report_mark_fixed(rep,S->name);
        free(newS.items); free(exB); free(exD); free(exS);
        return;
    }

    if(kind==KIND_A && is_physically_contiguous(exS,exS_count) && !ssif_alignment_ok(exS,exS_count,cB,nB,cD,nD,chosen)){
        stem_of_name(S->name,avail,sizeof(avail));
        report_repair_kind(rep,avail,"SSIF extent alignment repair");
        ExtArr newS;
        if(!build_aligned_ssif_extents(exS,exS_count,cB,nB,cD,nD,chosen,&newS))
            udf_error("Cannot build chunk-aligned SSIF extents.");
        uint64_t sum=0; for(size_t i=0;i<newS.count;i++) sum+=newS.items[i].len;
        if(sum!=S->info){ free(newS.items); udf_error("Chunk-aligned SSIF extents do not sum to SSIF file length."); }
        int counts_aed = S->chain.count>0 && S->lbr == (S->info+BS-1)/BS + S->chain.count;
        udf_rewrite_file(u,S,newS.items,newS.count,S->info,chosen_alloc,counts_aed,rep);
        report_say(rep,"  * %s: SSIF extents realigned to chunk boundaries (%zu -> %zu extents)",S->name,exS_count,newS.count);
        report_mark_realigned(rep,avail);
        report_change(rep,S->name,S->info,S->info,newS.count,0);
        report_mark_fixed(rep,S->name);
        free(exS); exS=newS.items; exS_count=newS.count;
        okB=0; okD=0;
    }

    stem_of_name(S->name,avail,sizeof(avail));
    if(!report_repair_kind_get(rep,avail)) report_repair_kind(rep,avail,"Stream file table repair");

    ChunkSet sets;
    build_per_chunk(exS,exS_count,cB,nB,cD,nD,chosen,&sets);

    if(!opt->no_data_check){
        Extent probes[64]; size_t np=0;
        size_t idxs[7]; size_t ni=sample_idx((size_t)n,idxs);
        const char *keys="BD";
        for(int ki=0;ki<2;ki++){
            ChunkVec *v=(keys[ki]=='B')?&sets.B:&sets.D;
            for(size_t ii=0;ii<ni;ii++){
                size_t i=idxs[ii];
                if(i<v->count && v->chunks[i].count>0 && np<64) probes[np++]=v->chunks[i].items[0];
            }
        }
        int allok=1;
        for(size_t pi=0;pi<np && allok;pi++) if(!sync_ok_safe(u,probes[pi].ref,probes[pi].lbn)) allok=0;
        if(!allok){ chunkset_free(&sets); free(exS); udf_error("MPEG-TS sync-byte verification failed after order selection."); }
        report_say(rep,"   Data verification: %zu chunk boundaries validated.",np);

        int have_sample=0; Extent sample_target; memset(&sample_target,0,sizeof(sample_target));
        if(sets.B.count>0 && sets.B.chunks[0].count>0){ sample_target=sets.B.chunks[0].items[0]; have_sample=1; }
        else if(sets.D.count>0 && sets.D.chunks[0].count>0){ sample_target=sets.D.chunks[0].items[0]; have_sample=1; }
        if(have_sample){
            int scrambled=0;
            unsigned char *blk=udf_read_blocks(u,sample_target.ref,sample_target.lbn,1);
            int maxp=(int)(BS/PKT); if(maxp>10) maxp=10;
            for(int p_idx=0;p_idx<maxp;p_idx++){
                unsigned char *pkt=blk+p_idx*PKT;
                if(pkt[4]==0x47){
                    int pid=((pkt[5]&0x1F)<<8)|pkt[6];
                    if(pid!=0 && pid!=0x100 && pid!=0x1FFF){
                        if(((pkt[7]>>6)&3)!=0){ scrambled=1; break; }
                    }
                }
            }
            free(blk);
            if(scrambled) report_say(rep,"   WARNING: Video stream packets appear AACS encrypted. Virtual drive players may fail unless decrypted!");
        }
    }

    int counts_aed = S->chain.count>0 && S->lbr == (S->info+BS-1)/BS + S->chain.count;

    ChunkVec *targets[2];
    const char *target_names[2];
    Node *target_nodes[2];
    uint64_t target_sizes[2];
    int ntargets=0;
    if(!okB){ targets[ntargets]=&sets.B; target_names[ntargets]=B->name; target_nodes[ntargets]=B; uint64_t s=0; for(int i=0;i<nB;i++) s+=(uint64_t)cB[i]; target_sizes[ntargets]=s; ntargets++; }
    if(!okD){ targets[ntargets]=&sets.D; target_names[ntargets]=D->name; target_nodes[ntargets]=D; uint64_t s=0; for(int i=0;i<nD;i++) s+=(uint64_t)cD[i]; target_sizes[ntargets]=s; ntargets++; }

    for(int ti=0;ti<ntargets;ti++){
        ExtArr exts; memset(&exts,0,sizeof(exts));
        flatten_chunks(targets[ti],&exts);
        int n_aed=udf_rewrite_file(u,target_nodes[ti],exts.items,exts.count,target_sizes[ti],chosen_alloc,counts_aed,rep);
        char oldsz[64],newsz[64]; fmt_size(target_nodes[ti]->info,oldsz,sizeof(oldsz)); fmt_size(target_sizes[ti],newsz,sizeof(newsz));
        if(n_aed) report_say(rep,"  * %s: %s -> %s (%zu extents, %d descriptor block(s))",target_names[ti],oldsz,newsz,exts.count,n_aed);
        else report_say(rep,"  * %s: %s -> %s (%zu extents)",target_names[ti],oldsz,newsz,exts.count);
        report_change(rep,target_names[ti],target_nodes[ti]->info,target_sizes[ti],exts.count,(size_t)n_aed);
        report_mark_fixed(rep,target_names[ti]);
        free(exts.items);
    }
    chunkset_free(&sets); free(exS);
}

/* ------------------------------------------------------------------ */
/* Analysis                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char stem[64];
    char dstem[64];
    int kind;
    Node *B,*D,*S;
    int *cB,*cD;
    int nB,nD;
    int okB,okD;
} Todo;

static int cmp_strp(const void *a,const void *b){ return strcmp(*(const char**)a,*(const char**)b); }

static Report *analyse_iso(Udf *udf, Options *opt) {
    Report *rep = report_create(opt->chatty);

    Node *streams = udf_lookup_path(udf, "BDMV/STREAM");
    Node *ssifd = udf_lookup_path(udf, "BDMV/STREAM/SSIF");
    Node *clipd = udf_lookup_path(udf, "BDMV/CLIPINF");
    Node *pld = udf_lookup_path(udf, "BDMV/PLAYLIST");
    if(!streams || !ssifd || !clipd)
        udf_error("BDMV/STREAM, STREAM/SSIF or BDMV/CLIPINF not found. Is this a 3D Blu-ray?");

    DirList streams_dir = udf_listdir(udf, streams);
    DirList ssif_dir = udf_listdir(udf, ssifd);
    DirList clip_dir = udf_listdir(udf, clipd);

    ClpiArr clpi; memset(&clpi,0,sizeof(clpi));
    for(size_t i=0;i<clip_dir.count;i++){
        if(!ends_with_ci(clip_dir.items[i].name,".CLPI")) continue;
        Node *volatile cn=NULL;
        unsigned char *volatile data=NULL; size_t len=0;
        ErrCtx *saved=g_err; ErrCtx local; int volatile ok=1;
        if(saved){
            g_err=&local;
            if(setjmp(local.jb)) ok=0;
            else { cn=udf_load(udf,clip_dir.items[i].ref,clip_dir.items[i].lbn,clip_dir.items[i].name); data=udf_read_file(udf,cn,&len); }
            g_err=saved;
        } else { cn=udf_load(udf,clip_dir.items[i].ref,clip_dir.items[i].lbn,clip_dir.items[i].name); data=udf_read_file(udf,cn,&len); }
        if(!ok){ if(data) free(data); if(cn) node_free(cn); continue; }
        Clpi c=parse_clpi(data,len);
        free(data); node_free(cn);
        if(!c.valid) continue;
        char stem[64]; stem_of_name(clip_dir.items[i].name,stem,sizeof(stem));
        for(char *p=stem;*p;p++) *p=(char)toupper((unsigned char)*p);
        if(clpi.count==clpi.cap){ clpi.cap=clpi.cap?clpi.cap*2:64; clpi.items=realloc(clpi.items,clpi.cap*sizeof(ClpiEntry)); if(!clpi.items)udf_error("Out of memory."); }
        memset(&clpi.items[clpi.count],0,sizeof(ClpiEntry));
        strcpy(clpi.items[clpi.count].stem,stem);
        clpi.items[clpi.count].c=c;
        clpi.count++;
    }

    PairArr pairs; memset(&pairs,0,sizeof(pairs));
    size_t volatile playlists_count=0;
    if(pld){
        DirList pl_dir=udf_listdir(udf,pld);
        for(size_t volatile i=0;i<pl_dir.count;i++){
            if(!ends_with_ci(pl_dir.items[i].name,".MPLS")) continue;
            playlists_count++;
            Node *volatile pn=NULL; unsigned char *volatile data=NULL; size_t len=0;
            ErrCtx *saved=g_err; ErrCtx local; int volatile ok=1;
            if(saved){
                g_err=&local;
                if(setjmp(local.jb)) ok=0;
                else { pn=udf_load(udf,pl_dir.items[i].ref,pl_dir.items[i].lbn,pl_dir.items[i].name); data=udf_read_file(udf,pn,&len); }
                g_err=saved;
            } else { pn=udf_load(udf,pl_dir.items[i].ref,pl_dir.items[i].lbn,pl_dir.items[i].name); data=udf_read_file(udf,pn,&len); }
            if(!ok){ if(data) free(data); if(pn) node_free(pn); continue; }
            MplsPairs mp=parse_mpls_pairs(data,len);
            free(data); node_free(pn);
            for(size_t j=0;j<mp.count;j++) pair_set(&pairs,mp.bases[j],mp.deps[j]);
            mpls_pairs_free(&mp);
        }
        free(pl_dir.items);
    }

    rep->clips_scanned=clpi.count;
    rep->playlists_scanned=playlists_count;

    StrArr ssif_stems; memset(&ssif_stems,0,sizeof(ssif_stems));
    for(size_t i=0;i<ssif_dir.count;i++){
        if(ends_with_ci(ssif_dir.items[i].name,".SSIF")){
            char stem[64]; stem_of_name(ssif_dir.items[i].name,stem,sizeof(stem));
            for(char *p=stem;*p;p++) *p=(char)toupper((unsigned char)*p);
            sa_push(&ssif_stems,stem);
        }
    }
    qsort(ssif_stems.items,ssif_stems.count,sizeof(char*),cmp_strp);

    report_say(rep,"Scanned %zu clip info files; found %zu SSIF files and %zu 3D playlist pairs.",clpi.count,ssif_stems.count,pairs.count);
    report_say(rep,"");

    Todo *todos=NULL; size_t todo_count=0,todo_cap=0;

    for(size_t si=0;si<ssif_stems.count;si++){
        const char *stem=ssif_stems.items[si];
        report_say(rep,"\n== %s.ssif",stem);
        Clpi *basec=clpi_find(&clpi,stem);
        if(!basec || basec->sp_count==0){
            report_say(rep,"   No interleaving table in %s.clpi - skipped.",stem);
            report_pair_status(rep,stem,NULL,"Skipped. No interleaving table.");
            continue;
        }
        char ssifname[256]; snprintf(ssifname,sizeof(ssifname),"%s.SSIF",stem);
        DirEntry *sse=find_entry(&ssif_dir,ssifname);
        if(!sse) continue;
        Node *S=udf_load(udf,sse->ref,sse->lbn,sse->name);

        char dbuf[64]; dbuf[0]=0;
        const char *pdstem=pair_find(&pairs,stem);
        if(pdstem) snprintf(dbuf,sizeof(dbuf),"%s",pdstem);
        if(!pdstem){
            char cand[64]; int ncand=0;
            for(size_t i=0;i<clpi.count;i++){
                if(strcasecmp(clpi.items[i].stem,stem)==0) continue;
                Clpi *c=&clpi.items[i].c;
                if(!c->sp || c->sp_count!=basec->sp_count) continue;
                uint64_t exp=(uint64_t)(basec->pkts+c->pkts)*PKT;
                if(exp!=S->info) continue;
                snprintf(cand,sizeof(cand),"%s",clpi.items[i].stem);
                ncand++;
            }
            if(ncand==1){
                snprintf(dbuf,sizeof(dbuf),"%s",cand);
                report_say(rep,"   Dependent clip %s identified from packet counts.",dbuf);
            }
        }
        if(dbuf[0]==0 || !clpi_find(&clpi,dbuf) || clpi_find(&clpi,dbuf)->sp_count==0){
            report_say(rep,"   Cannot determine dependent-view clip - skipped.");
            report_pair_status(rep,stem,NULL,"Skipped. No dependent clip found.");
            node_free(S);
            continue;
        }
        Clpi *depc=clpi_find(&clpi,dbuf);

        size_t nB=0,nD=0;
        uint64_t *cB64=chunk_sizes(basec,&nB);
        uint64_t *cD64=chunk_sizes(depc,&nD);
        if(nB!=nD) report_say(rep,"   Asymmetric clip tables (%zu base, %zu dependent chunks).",nB,nD);
        int *icB=xm((nB?nB:1)*sizeof(int));
        int *icD=xm((nD?nD:1)*sizeof(int));
        for(size_t i=0;i<nB;i++) icB[i]=(int)cB64[i];
        for(size_t i=0;i<nD;i++) icD[i]=(int)cD64[i];

        int invalid=0;
        for(size_t i=0;i+1<nB;i++) if(icB[i]%BS) invalid=1;
        for(size_t i=0;i+1<nD;i++) if(icD[i]%BS) invalid=1;
        for(size_t i=1;i<basec->sp_count;i++) if(basec->sp[i-1]>=basec->sp[i]) invalid=1;
        if(basec->sp[0]!=0) invalid=1;
        if(depc->sp[0]!=0) invalid=1;
        if(invalid){
            report_say(rep,"   Extent tables are invalid - skipped.");
            rep->problems++;
            report_pair_status(rep,stem,dbuf,"Cannot repair. Invalid extent tables.");
            free(cB64); free(cD64); free(icB); free(icD); node_free(S);
            continue;
        }

        uint64_t eB=(uint64_t)basec->pkts*PKT;
        uint64_t eD=(uint64_t)depc->pkts*PKT;
        uint64_t eS=eB+eD;

        Node *B=load_m2ts(udf,&streams_dir,stem);
        Node *D=load_m2ts(udf,&streams_dir,dbuf);

        size_t min_extB=basec->sp_count;
        size_t min_extD=depc->sp_count;
        int okB=node_ok(udf,B,eB,min_extB);
        int okD=node_ok(udf,D,eD,min_extD);
        int okS=node_ok(udf,S,eS,1);
        size_t exS_count=0;
        Extent *exS=extents_of(udf,S,&exS_count);

        if(okB && okD && okS && exS){
            if(!extents_match_ssif(udf,B,D,exS,exS_count,icB,(int)nB,icD,(int)nD)){
                report_say(rep,"   Extents valid but not SSIF-aligned - will repoint to SSIF data.");
                okB=0; okD=0;
            }
        }

        report_say(rep,"   Base %s.m2ts (%llu bytes expected), Dependent %s.m2ts (%llu), SSIF %llu",
                   stem,(unsigned long long)eB,dbuf,(unsigned long long)eD,(unsigned long long)eS);
        const char *labs[3]={"Base m2ts","Dependent m2ts","SSIF"};
        Node *nodes[3]={B,D,S};
        int oks[3]={okB,okD,okS};
        for(int li=0;li<3;li++){
            if(!nodes[li]){
                report_say(rep,"   %-15s Missing",labs[li]);
            } else {
                char sz[64]; fmt_size(nodes[li]->info,sz,sizeof(sz));
                report_say(rep,"   %-15s %s (%llu bytes), %zu descriptor(s) %s",labs[li],sz,
                           (unsigned long long)nodes[li]->info,nodes[li]->ads.count,oks[li]?"OK":"<-- WRONG");
            }
        }
        report_say(rep,"");

        if(okB && okD && okS){
            report_pair_status(rep,stem,dbuf,"OK");
            if(B) node_free(B);
            if(D) node_free(D);
            if(S) node_free(S);
            free(exS); free(cB64); free(cD64); free(icB); free(icD);
            continue;
        }

        int kind=0;
        if(okS && B && D){
            kind=KIND_A;
            report_pair_status(rep,stem,dbuf,"Needs repair.");
        } else if(okB && okD && S){
            kind=KIND_B;
            report_pair_status(rep,stem,dbuf,"Needs repair.");
        } else {
            report_say(rep,"   Cannot repair: No complete copy of stream data exists.");
            rep->problems++;
            report_pair_status(rep,stem,dbuf,"Cannot repair. No complete data source.");
            if(B) node_free(B);
            if(D) node_free(D);
            if(S) node_free(S);
            free(exS); free(cB64); free(cD64); free(icB); free(icD);
            continue;
        }

        if(todo_count==todo_cap){ todo_cap=todo_cap?todo_cap*2:16; todos=realloc(todos,todo_cap*sizeof(Todo)); if(!todos)udf_error("Out of memory."); }
        Todo *t=&todos[todo_count++];
        memset(t,0,sizeof(*t));
        strcpy(t->stem,stem); strcpy(t->dstem,dbuf);
        t->kind=kind; t->B=B; t->D=D; t->S=S;
        t->cB=icB; t->cD=icD; t->nB=(int)nB; t->nD=(int)nD; t->okB=okB; t->okD=okD;
        free(exS); free(cB64); free(cD64);
    }

    if(todo_count) report_say(rep,"%s",opt->apply?"\nApplying Repairs:":"\nPlanned Repairs:");

    for(size_t volatile ti=0;ti<todo_count;ti++){
        Todo *t=&todos[ti];
        report_say(rep,"\n  * Clip Pair: %s.m2ts / %s.m2ts (%s)",t->stem,t->dstem,
                   t->kind==KIND_B?"rebuilding SSIF from m2ts":"rebuilding m2ts from SSIF");
        ImageSnap snap; image_snapshot(udf->img,&snap);
        ErrCtx *saved=g_err;
        ErrCtx local;
        int volatile failed=0; char emsg[1024]; emsg[0]=0;
        if(saved){
            g_err=&local;
            if(setjmp(local.jb)){ snprintf(emsg,sizeof(emsg),"%s",local.msg); failed=1; }
            else plan_pair(udf,opt,rep,t->kind,t->stem,t->dstem,t->B,t->D,t->S,t->cB,t->nB,t->cD,t->nD,t->okB,t->okD);
            g_err=saved;
        } else {
            plan_pair(udf,opt,rep,t->kind,t->stem,t->dstem,t->B,t->D,t->S,t->cB,t->nB,t->cD,t->nD,t->okB,t->okD);
        }
        if(!failed){
            free(snap.pending);
            report_pair_set_status(rep,t->stem,opt->apply?"Repaired.":"Can be repaired.");
        } else {
            report_say(rep,"   REFUSED: %s",emsg);
            rep->problems++;
            report_add_error(rep,"%s / %s: %s",t->stem,t->dstem,emsg);
            image_restore(udf->img,&snap);
            char st[512]; snprintf(st,sizeof(st),"Cannot repair. %s",emsg);
            report_pair_set_status(rep,t->stem,st);
        }
        if(t->B) node_free(t->B);
        if(t->D) node_free(t->D);
        if(t->S) node_free(t->S);
        free(t->cB); free(t->cD);
    }
    free(todos);

    for(size_t i=0;i<ssif_stems.count;i++) free(ssif_stems.items[i]);
    free(ssif_stems.items);
    for(size_t i=0;i<clpi.count;i++) free(clpi.items[i].c.sp);
    free(clpi.items);
    for(size_t i=0;i<pairs.count;i++){ (void)i; }
    free(pairs.items);
    free(streams_dir.items); free(ssif_dir.items); free(clip_dir.items);
    if(streams) node_free(streams);
    if(ssifd) node_free(ssifd);
    if(clipd) node_free(clipd);
    if(pld) node_free(pld);
    return rep;
}

/* ------------------------------------------------------------------ */
/* Human-readable output                                               */
/* ------------------------------------------------------------------ */

static char *base_name(const char *path){
    const char *b=strrchr(path,'\\'); if(!b) b=strrchr(path,'/');
    return (char*)(b?b+1:path);
}

static void print_single(const char *path,Image *img,Report *rep,Options *opt){
    printf("\n%s (",base_name(path));
    char sz[64]; fmt_size(img->orig_size,sz,sizeof(sz)); printf("%s)\n",sz);
    printf("%zu clips, %zu playlists, %zu 3D clip pairs.\n",rep->clips_scanned,rep->playlists_scanned,rep->pairs_count);

    int issues=report_issue_count(rep);
    int blocking=report_blocking_count(rep);

    printf("\n");
    if(opt->chatty){
        if(issues){
            printf("Repairable pairs\n");
            for(size_t i=0;i<rep->pairs_count;i++){
                PairStatus *p=&rep->pairs[i];
                if(!report_pair_is_issue(p)) continue;
                const char *label=report_repair_kind_get(rep,p->stem);
                if(!label) label="File table repair";
                printf("  %s / %s   %s\n",p->stem,p->dstem[0]?p->dstem:"-----",label);
            }
        }
        if(blocking){
            printf("Pairs needing manual repair\n");
            for(size_t i=0;i<rep->pairs_count;i++){
                PairStatus *p=&rep->pairs[i];
                if(!report_pair_is_blocking(p)) continue;
                printf("  %s / %s   %s\n",p->stem,p->dstem[0]?p->dstem:"-----",p->status);
            }
        }
        if(!issues && !blocking) printf("No problems found.\n");
    } else {
        if(issues){
            printf("%s: %d clip pair(s).\n",opt->apply?"Repaired":"Can repair",issues);
            /* Sorted label counts. */
            typedef struct { const char *label; int count; } KC;
            KC kinds[64]; size_t nk=0;
            for(size_t i=0;i<rep->pairs_count;i++){
                if(!report_pair_is_issue(&rep->pairs[i])) continue;
                const char *label=report_repair_kind_get(rep,rep->pairs[i].stem);
                if(!label) label="File table repair";
                size_t j; for(j=0;j<nk;j++) if(strcmp(kinds[j].label,label)==0){kinds[j].count++;break;}
                if(j==nk && nk<64){ kinds[nk].label=label; kinds[nk].count=1; nk++; }
            }
            for(size_t i=1;i<nk;i++){ KC t=kinds[i]; size_t j=i; while(j>0&&strcmp(kinds[j-1].label,t.label)>0){kinds[j]=kinds[j-1];j--;} kinds[j]=t; }
            for(size_t i=0;i<nk;i++) printf("  %s: %d\n",kinds[i].label,kinds[i].count);
        }
        if(blocking) printf("Needs manual repair: %d clip pair(s). Use -chatty for details.\n",blocking);
        if(!issues && !blocking) printf("No problems found.\n");
    }

    if(img->pending_count==0){
        printf("\nNo changes were made.\n");
        return;
    }
    printf("\n");
    if(opt->apply) printf("Repairs written successfully.\n");
    else printf("Run again with -apply to write the repairs.\n");
}

int run_single(const char *path, Options *opt) {
    Image *img=image_open(path,opt->apply);
    Udf *udf=udf_create(img);
    Report *rep=analyse_iso(udf,opt);
    print_single(path,img,rep,opt);

    int rc=0;
    if(img->pending_count==0){
        rc=rep->problems?1:0;
        report_free(rep); udf_destroy(udf); image_close(img);
        return rc;
    }
    if(!opt->apply){
        report_free(rep); udf_destroy(udf); image_close(img);
        return 0;
    }

    image_apply(img);
    printf("\nVerifying the repaired image...\n");
    Image *img2=image_open(path,0);
    Udf *udf2=udf_create(img2);
    Options dry=*opt; dry.apply=0; dry.chatty=0;
    Report *rep2=analyse_iso(udf2,&dry);
    if(!rep2->problems && img2->pending_count==0){
        printf("All 3D clip pairs are consistent now.\n");
        rc=0;
    } else {
        printf("Verification found remaining issues. Run with -chatty for details.\n");
        rc=1;
    }
    report_free(rep2); udf_destroy(udf2); image_close(img2);
    report_free(rep); udf_destroy(udf); image_close(img);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Folder / batch mode                                                 */
/* ------------------------------------------------------------------ */

/* Returns 0 on success, -1 if the image could not be read. */
static int scan_folder_image(const char *path, Options *opt, int *issues, int *fixable, char *summary, size_t sumsize) {
    ErrCtx *saved=g_err;
    ErrCtx local;
    int ok=1;
    *issues=0; *fixable=0; summary[0]=0;
    if(saved){ g_err=&local; if(setjmp(local.jb)) ok=0; }
    if(!ok){ g_err=saved; snprintf(summary,sumsize,"%s",local.msg); return -1; }

    Image *img=image_open(path,0);
    Udf *udf=udf_create(img);
    Options dry=*opt; dry.apply=0; dry.chatty=0;
    Report *rep=analyse_iso(udf,&dry);
    int n_issues=report_issue_count(rep);
    int blocking=report_blocking_count(rep)+(int)rep->errors_count;
    report_format_repair_counts(rep,summary,sumsize);
    if(summary[0]==0) snprintf(summary,sumsize,"Needs manual repair");
    int fix=(n_issues>0 && blocking==0 && rep->problems==0 && img->pending_count>0);
    if(n_issues==0) snprintf(summary,sumsize,"Clean");
    *issues=n_issues; *fixable=fix;
    report_free(rep); udf_destroy(udf); image_close(img);
    g_err=saved;
    return 0;
}

static int apply_folder_image(const char *path, Options *opt) {
    ErrCtx *saved=g_err;
    ErrCtx local;
    int ok=1;
    if(saved){ g_err=&local; if(setjmp(local.jb)){ ok=0; } }
    if(!ok){ g_err=saved; return 0; }

    Image *img=image_open(path,1);
    Udf *udf=udf_create(img);
    Options ap=*opt; ap.apply=1; ap.chatty=0;
    Report *rep=analyse_iso(udf,&ap);
    int good=0;
    if(!rep->problems && img->pending_count>0){
        image_apply(img);
        Image *img2=image_open(path,0);
        Udf *udf2=udf_create(img2);
        Options dry=*opt; dry.apply=0; dry.chatty=0;
        Report *rep2=analyse_iso(udf2,&dry);
        good=(!rep2->problems && img2->pending_count==0);
        report_free(rep2); udf_destroy(udf2); image_close(img2);
    }
    report_free(rep); udf_destroy(udf); image_close(img);
    g_err=saved;
    return good;
}

static int iso_name_cmp(const void *a,const void *b){ return strcmp(*(const char**)a,*(const char**)b); }

int run_folder(const char *folder, Options *opt) {
    DIR *d=opendir(folder);
    if(!d) udf_error("Cannot open folder (%s).",folder);
    StrArr names; memset(&names,0,sizeof(names));
    struct dirent *ent;
    while((ent=readdir(d))!=NULL){
        if(!ends_with_ci(ent->d_name,".iso")) continue;
        char full[4096]; snprintf(full,sizeof(full),"%s/%s",folder,ent->d_name);
        struct _stat64 st;
        if(_stat64(full,&st)!=0 || (st.st_mode & _S_IFMT)!=_S_IFREG) continue;
        sa_push(&names,ent->d_name);
    }
    closedir(d);
    if(names.count==0){
        printf("No ISO files found in %s\n",folder);
        free(names.items);
        return 1;
    }
    qsort(names.items,names.count,sizeof(char*),iso_name_cmp);

    if(opt->apply) printf("Fixing ISOs in %s\n\n",folder);

    int width=(int)strlen("ISO");
    for(size_t i=0;i<names.count;i++){ int l=(int)strlen(names.items[i]); if(l>width) width=l; }
    printf("%-*s  %6s  %s\n",width,"ISO","Issues","Status");
    char dashes[256]; int dn=width; if(dn>255) dn=255;
    memset(dashes,'-',(size_t)dn); dashes[dn]=0;
    printf("%s  %6s  %s\n",dashes,"------","----------------------------");

    int failed=0;
    for(size_t i=0;i<names.count;i++){
        const char *name=names.items[i];
        char path[4096]; snprintf(path,sizeof(path),"%s/%s",folder,name);
        int n_issues=0,fixable=0;
        char summary[1024];
        int scan=scan_folder_image(path,opt,&n_issues,&fixable,summary,sizeof(summary));
        if(scan<0){
            printf("%-*s  %6s  Unreadable (%s)\n",width,name,"--",summary);
            failed++;
            continue;
        }
        if(n_issues==0){
            if(opt->show_all) printf("%-*s  %6d  Clean\n",width,name,0);
            continue;
        }
        if(!opt->apply){
            const char *verdict=fixable?"Fixable":"Not fixable";
            printf("%-*s  %6d  %s: %s\n",width,name,n_issues,verdict,summary);
            continue;
        }
        if(!fixable){
            printf("%-*s  %6d  Not fixable - left untouched: %s\n",width,name,n_issues,summary);
            failed++;
            continue;
        }
        int ok=apply_folder_image(path,opt);
        if(ok) printf("%-*s  %6d  Fixed: %s\n",width,name,n_issues,summary);
        else { printf("%-*s  %6d  Failed - left recoverable via undo\n",width,name,n_issues); failed++; }
    }

    for(size_t i=0;i<names.count;i++) free(names.items[i]);
    free(names.items);
    return failed?1:0;
}
