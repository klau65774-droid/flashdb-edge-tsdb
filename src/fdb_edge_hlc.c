#include "fdb_edge/fdb_edge_hlc.h"

#include <string.h>

#include "fdb_edge/fdb_edge_config.h"

void fdb_edge_hlc_init(
    fdb_edge_hlc_t *hlc,
    int64_t initial_wall_ms,
    uint64_t initial_mono_seq,
    uint32_t initial_epoch)
{
    if (!hlc) {
        return;
    }
    memset(hlc, 0, sizeof(*hlc));
    hlc->last_wall_ms = initial_wall_ms;
    hlc->monotonic_seq = initial_mono_seq;
    hlc->epoch = initial_epoch;
    hlc->logical_sub = 0U;
    hlc->rollback_count = 0U;
    hlc->initialized = (initial_mono_seq > 0ULL);
}

int fdb_edge_hlc_next(fdb_edge_hlc_t *hlc, int64_t wall_ms, fdb_edge_stamp_t *out_stamp)
{
    bool was_rollback = false;

    if (!hlc || !out_stamp) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (hlc->monotonic_seq == UINT64_MAX) {
        return FDB_EDGE_ERR_BOUNDS;
    }

    hlc->monotonic_seq++;
    if (!hlc->initialized) {
        hlc->initialized = true;
        hlc->last_wall_ms = wall_ms;
        hlc->logical_sub = 0U;
    } else if (wall_ms < hlc->last_wall_ms) {
        if (hlc->epoch == UINT32_MAX) {
            hlc->monotonic_seq--;
            return FDB_EDGE_ERR_BOUNDS;
        }
        hlc->epoch++;
        hlc->rollback_count++;
        hlc->last_wall_ms = wall_ms;
        hlc->logical_sub = 0U;
        was_rollback = true;
    } else if (wall_ms == hlc->last_wall_ms) {
        if (hlc->logical_sub < UINT32_MAX) {
            hlc->logical_sub++;
        }
    } else {
        hlc->last_wall_ms = wall_ms;
        hlc->logical_sub = 0U;
    }

    out_stamp->wall_ts_ms = wall_ms;
    out_stamp->mono_seq = hlc->monotonic_seq;
    out_stamp->epoch = hlc->epoch;
    out_stamp->logical_sub = hlc->logical_sub;
    out_stamp->was_rollback = was_rollback;
    return FDB_EDGE_OK;
}
