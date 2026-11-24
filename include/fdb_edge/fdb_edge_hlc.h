#ifndef FDB_EDGE_HLC_H
#define FDB_EDGE_HLC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int64_t last_wall_ms;
    uint64_t monotonic_seq;
    uint32_t epoch;
    uint32_t logical_sub;
    uint32_t rollback_count;
    bool initialized;
} fdb_edge_hlc_t;

typedef struct {
    int64_t wall_ts_ms;
    uint64_t mono_seq;
    uint32_t epoch;
    uint32_t logical_sub;
    bool was_rollback;
} fdb_edge_stamp_t;

void fdb_edge_hlc_init(
    fdb_edge_hlc_t *hlc,
    int64_t initial_wall_ms,
    uint64_t initial_mono_seq,
    uint32_t initial_epoch);

int fdb_edge_hlc_next(fdb_edge_hlc_t *hlc, int64_t wall_ms, fdb_edge_stamp_t *out_stamp);

#ifdef __cplusplus
}
#endif

#endif /* FDB_EDGE_HLC_H */
