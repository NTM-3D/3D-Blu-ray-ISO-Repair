#ifndef CLPI_H
#define CLPI_H

#include "udf.h"

typedef struct {
    uint32_t pkts;
    uint32_t *sp;
    size_t sp_count;
    int valid;
} Clpi;

typedef struct {
    char **bases;
    char **deps;
    size_t count;
} MplsPairs;

Clpi parse_clpi(const unsigned char *data, size_t len);
MplsPairs parse_mpls_pairs(const unsigned char *data, size_t len);
void mpls_pairs_free(MplsPairs *p);
uint64_t *chunk_sizes(const Clpi *c, size_t *out_count);

#endif
