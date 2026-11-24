#ifndef FDB_EDGE_WAL_SYNC_H
#define FDB_EDGE_WAL_SYNC_H

#include <stddef.h>
#include <stdint.h>

#include "fdb_edge/fdb_edge_config.h"
#include "fdb_edge/fdb_edge_tsdb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FDB_WAL_MSG_DATA = 1,
    FDB_WAL_MSG_ACK = 2,
    FDB_WAL_MSG_NACK = 3,
    FDB_WAL_MSG_HEARTBEAT = 4
} fdb_wal_msg_type_t;

typedef struct {
    fdb_wal_msg_type_t type;
    uint32_t device_id;
    uint64_t from_seq;
    uint64_t to_seq;
    uint16_t flags;
    uint16_t payload_len;
} fdb_wal_frame_hdr_t;

typedef struct {
    fdb_edge_tsdb_t *dest_db;
    uint32_t expected_device_id;
    uint64_t peer_last_seq;
    uint64_t frames_accepted;
    uint64_t frames_corrupt;
    uint64_t frames_duplicate;
    uint64_t frames_out_of_order;
    uint8_t rx_cobs[FDB_EDGE_MAX_FRAME_BYTES];
    size_t rx_len;
    bool overflowed;
} fdb_wal_receiver_t;

int fdb_wal_encode_frame(
    const fdb_wal_frame_hdr_t *hdr,
    const uint8_t *payload,
    uint8_t *out_wire,
    size_t out_cap,
    size_t *out_len);

int fdb_wal_decode_frame(
    const uint8_t *wire_no_delim,
    size_t wire_len,
    fdb_wal_frame_hdr_t *out_hdr,
    uint8_t *out_payload,
    size_t payload_cap);

/* On FDB_EDGE_ERR_BOUNDS, the output length and count describe the complete
 * frame prefix that fit. Advance the cursor using the receiver's ACK. */
int fdb_wal_export_after(
    fdb_edge_tsdb_t *src_db,
    uint32_t device_id,
    uint64_t acked_seq,
    uint8_t *out_stream,
    size_t out_cap,
    size_t *out_stream_len,
    uint32_t *out_frame_count);

void fdb_wal_receiver_init(
    fdb_wal_receiver_t *rx,
    fdb_edge_tsdb_t *dest_db,
    uint32_t expected_device_id);

/* A feed always consumes all input. Replies that do not fit in the supplied
 * buffer are omitted. To retain every reply, feed at most one delimiter per
 * call and provide at least 34 bytes of reply space. */
int fdb_wal_receiver_feed(
    fdb_wal_receiver_t *rx,
    const uint8_t *bytes,
    size_t len,
    uint8_t *out_reply_wire,
    size_t reply_cap,
    size_t *out_reply_len);

#ifdef __cplusplus
}
#endif

#endif /* FDB_EDGE_WAL_SYNC_H */
