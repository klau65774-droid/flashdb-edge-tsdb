#ifndef FDB_EDGE_CODEC_H
#define FDB_EDGE_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "fdb_edge/fdb_edge_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int64_t wall_ts_ms;
    uint64_t mono_seq;
    uint32_t epoch;
    uint32_t logical_sub;
    uint8_t num_channels;
    double values[FDB_EDGE_MAX_CHANNELS];
} fdb_edge_sample_t;

int fdb_edge_codec_encode(
    const fdb_edge_sample_t *samples,
    size_t sample_count,
    uint8_t *out_buf,
    size_t out_cap,
    size_t *out_len);

int fdb_edge_codec_decode(
    const uint8_t *buf,
    size_t buf_len,
    fdb_edge_sample_t *out_samples,
    size_t out_sample_cap,
    size_t *out_sample_count);

#ifdef __cplusplus
}
#endif

#endif /* FDB_EDGE_CODEC_H */
