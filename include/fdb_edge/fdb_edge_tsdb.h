#ifndef FDB_EDGE_TSDB_H
#define FDB_EDGE_TSDB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fdb_edge/fdb_edge_codec.h"
#include "fdb_edge/fdb_edge_config.h"
#include "fdb_edge/fdb_edge_hlc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FDB_EDGE_SECTOR_HDR_BYTES    96U
#define FDB_EDGE_BLOCK_HDR_BYTES     64U

typedef enum {
    FDB_EDGE_SEC_CLEAN_ERASED = 0,
    FDB_EDGE_SEC_ACTIVE_UNSEALED = 1,
    FDB_EDGE_SEC_SEALED = 2,
    FDB_EDGE_SEC_TORN_SEAL = 3,
    FDB_EDGE_SEC_TORN_HDR = 4
} fdb_edge_sec_state_t;

typedef struct {
    uint8_t num_channels;
    uint16_t batch_size;      /* 1..FDB_EDGE_MAX_BLOCK_SAMPLES (0 => default 64) */
    uint32_t max_block_bytes; /* 0 => auto max fitting one sector */
    bool rollover;            /* true => overwrite oldest sector when full */
    bool allow_torn_recovery; /* true => skip uncommitted/torn tail blocks on open */
} fdb_edge_tsdb_config_t;

typedef struct {
    uint64_t total_samples;
    uint64_t committed_blocks;
    uint64_t flushed_samples;
    uint64_t staged_samples;
    uint64_t rollback_events;
    uint64_t torn_blocks_recovered;
    uint64_t last_mono_seq;
    int64_t last_wall_ms;
    uint32_t current_epoch;
    uint32_t sector_count;
    uint32_t sealed_sectors;
} fdb_edge_tsdb_stats_t;

typedef struct {
    fdb_edge_sec_state_t state;
    uint64_t sector_seq;
    uint32_t erase_count;
    uint32_t write_offset;
    bool has_torn_tail;
    uint32_t block_count;
    uint32_t sample_count;
    uint64_t min_mono_seq;
    uint64_t max_mono_seq;
    int64_t min_wall_ms;
    int64_t max_wall_ms;
} fdb_edge_sec_meta_t;

typedef bool (*fdb_edge_sample_cb)(const fdb_edge_sample_t *sample, void *user_ctx);
typedef bool (*fdb_edge_raw_block_cb)(
    const uint8_t *payload,
    size_t payload_len,
    uint64_t min_mono_seq,
    uint64_t max_mono_seq,
    int64_t min_wall_ms,
    int64_t max_wall_ms,
    uint32_t sample_count,
    void *user_ctx);

typedef struct {
    fdb_flash_ops_t flash;
    fdb_edge_tsdb_config_t cfg;
    fdb_edge_hlc_t hlc;
    bool initialized;
    bool commit_readback_pending;

    uint32_t sector_count;
    uint32_t commit_aligned_bytes;
    uint32_t effective_max_block_bytes;
    uint64_t next_sector_seq;
    int32_t cur_sec_idx;

    uint64_t torn_blocks_recovered;
    uint64_t committed_blocks;
    uint64_t flushed_samples;

    fdb_edge_sec_meta_t sectors[FDB_EDGE_MAX_TRACKED_SECTORS];

    /* Zero-malloc static RAM staging buffer and codec workspace */
    fdb_edge_sample_t stage[FDB_EDGE_MAX_BLOCK_SAMPLES];
    size_t stage_len;
    uint8_t work_buf[FDB_EDGE_MAX_WORK_BUF_BYTES];
    fdb_edge_sample_t decode_buf[FDB_EDGE_MAX_BLOCK_SAMPLES];
} fdb_edge_tsdb_t;

void fdb_edge_tsdb_default_config(fdb_edge_tsdb_config_t *cfg, uint8_t num_channels);

int fdb_edge_tsdb_open(
    fdb_edge_tsdb_t *db,
    const fdb_flash_ops_t *flash,
    const fdb_edge_tsdb_config_t *cfg);

/* An automatic flush can fail after accepting this sample. A returned stamp in
 * that case identifies the accepted sample; retry flush before resubmitting. */
int fdb_edge_tsdb_append(
    fdb_edge_tsdb_t *db,
    int64_t wall_ts_ms,
    const double *values,
    uint8_t num_channels,
    fdb_edge_stamp_t *out_stamp);

int fdb_edge_tsdb_append_raw_block(
    fdb_edge_tsdb_t *db,
    const uint8_t *payload,
    size_t payload_len);

/* Import only if the decoded endpoints match the transport's advertised range. */
int fdb_edge_tsdb_append_raw_block_range(
    fdb_edge_tsdb_t *db,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t from_seq,
    uint64_t to_seq);

int fdb_edge_tsdb_flush(fdb_edge_tsdb_t *db);

int fdb_edge_tsdb_query_range_wall(
    fdb_edge_tsdb_t *db,
    int64_t from_wall_ms,
    int64_t to_wall_ms,
    fdb_edge_sample_cb cb,
    void *user_ctx);

int fdb_edge_tsdb_query_range_mono(
    fdb_edge_tsdb_t *db,
    uint64_t from_seq,
    uint64_t to_seq,
    fdb_edge_sample_cb cb,
    void *user_ctx);

int fdb_edge_tsdb_iter_raw_blocks_after(
    fdb_edge_tsdb_t *db,
    uint64_t after_seq,
    fdb_edge_raw_block_cb cb,
    void *user_ctx);

void fdb_edge_tsdb_get_stats(const fdb_edge_tsdb_t *db, fdb_edge_tsdb_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* FDB_EDGE_TSDB_H */
