#include "fdb_edge/fdb_edge_wal_sync.h"

#include <string.h>

#include "fdb_edge/fdb_edge_crc32c.h"

#define WAL_HDR_SIZE     28U
#define WAL_CRC_SIZE     4U
#define WAL_MAGIC_0      'W'
#define WAL_MAGIC_1      'L'
#define WAL_WIRE_VER     1U

static void put_u16_le(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFFU);
    dst[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static uint16_t get_u16_le(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static void put_u32_le(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFFU);
    dst[1] = (uint8_t)((v >> 8) & 0xFFU);
    dst[2] = (uint8_t)((v >> 16) & 0xFFU);
    dst[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint32_t get_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0]
        | ((uint32_t)src[1] << 8)
        | ((uint32_t)src[2] << 16)
        | ((uint32_t)src[3] << 24);
}

static void put_u64_le(uint8_t *dst, uint64_t v)
{
    size_t i;
    for (i = 0; i < 8U; i++) {
        dst[i] = (uint8_t)((v >> (i * 8U)) & 0xFFULL);
    }
}

static uint64_t get_u64_le(const uint8_t *src)
{
    uint64_t v = 0ULL;
    size_t i;
    for (i = 0; i < 8U; i++) {
        v |= ((uint64_t)src[i]) << (i * 8U);
    }
    return v;
}

/* A NULL destination measures the exact wire size before any output is changed. */
static size_t cobs_encode_frame(
    const uint8_t *header,
    const uint8_t *payload,
    size_t payload_len,
    const uint8_t *crc,
    uint8_t *dst)
{
    const uint8_t *parts[] = {header, payload, crc};
    const size_t part_sizes[] = {WAL_HDR_SIZE, payload_len, WAL_CRC_SIZE};
    size_t remaining = WAL_HDR_SIZE + payload_len + WAL_CRC_SIZE;
    size_t write_idx = 1U;
    size_t code_idx = 0U;
    uint8_t code = 1U;
    size_t part;

    for (part = 0U; part < sizeof(parts) / sizeof(parts[0]); part++) {
        size_t i;
        for (i = 0U; i < part_sizes[part]; i++) {
            uint8_t byte = parts[part][i];
            remaining--;
            if (byte != 0U) {
                if (dst) {
                    dst[write_idx] = byte;
                }
                write_idx++;
                code++;
            }
            if (byte == 0U || (code == 0xFFU && remaining > 0U)) {
                if (dst) {
                    dst[code_idx] = code;
                }
                code_idx = write_idx++;
                code = 1U;
            }
        }
    }
    if (dst) {
        dst[code_idx] = code;
        dst[write_idx] = 0U;
    }
    return write_idx + 1U;
}

typedef struct {
    const uint8_t *src;
    size_t len;
    size_t pos;
    size_t run_len;
    bool zero_before_next;
} cobs_reader_t;

/* Return one decoded byte, EOF, or a negative error. The raw byte's wire
 * position is pos - 1, including zeros represented by the next run code. */
static int cobs_next(cobs_reader_t *reader, uint8_t *byte)
{
    while (reader->run_len == 0U) {
        uint8_t code;
        bool insert_zero = reader->zero_before_next;

        if (reader->pos == reader->len) {
            return 0;
        }
        code = reader->src[reader->pos++];
        if (code == 0U || (size_t)(code - 1U) > reader->len - reader->pos) {
            return FDB_EDGE_ERR_CORRUPT;
        }
        reader->run_len = (size_t)(code - 1U);
        reader->zero_before_next = code != 0xFFU;
        if (insert_zero) {
            *byte = 0U;
            return 1;
        }
    }
    *byte = reader->src[reader->pos++];
    reader->run_len--;
    return *byte != 0U ? 1 : FDB_EDGE_ERR_CORRUPT;
}

static int cobs_decode(const uint8_t *src, size_t len, uint8_t *dst, size_t dst_cap, size_t *out_len)
{
    cobs_reader_t reader = {src, len, 0U, 0U, false};
    size_t write_idx = 0U;
    uint8_t byte;
    int rc;

    while ((rc = cobs_next(&reader, &byte)) > 0) {
        if (write_idx >= dst_cap) {
            return FDB_EDGE_ERR_BOUNDS;
        }
        dst[write_idx++] = byte;
    }
    *out_len = write_idx;
    return rc;
}

static int cobs_read(cobs_reader_t *reader, uint8_t *out, size_t len)
{
    size_t i;
    for (i = 0U; i < len; i++) {
        uint8_t byte;
        if (cobs_next(reader, &byte) != 1) {
            return FDB_EDGE_ERR_CORRUPT;
        }
        out[i] = byte;
    }
    return FDB_EDGE_OK;
}

int fdb_wal_encode_frame(
    const fdb_wal_frame_hdr_t *hdr,
    const uint8_t *payload,
    uint8_t *out_wire,
    size_t out_cap,
    size_t *out_len)
{
    uint8_t header[WAL_HDR_SIZE];
    uint8_t crc_bytes[WAL_CRC_SIZE];
    const uint8_t *staged_payload = NULL;
    size_t wire_len;
    size_t payload_len;
    uint32_t crc;

    if (!hdr || !out_wire || !out_len || (hdr->payload_len > 0U && !payload)) {
        return FDB_EDGE_ERR_INVALID;
    }
    payload_len = hdr->payload_len;
    if (payload_len + WAL_HDR_SIZE + WAL_CRC_SIZE + 2U > out_cap ||
        payload_len + WAL_HDR_SIZE + WAL_CRC_SIZE + 2U > FDB_EDGE_MAX_FRAME_BYTES) {
        return FDB_EDGE_ERR_BOUNDS;
    }

    header[0] = WAL_MAGIC_0;
    header[1] = WAL_MAGIC_1;
    header[2] = WAL_WIRE_VER;
    header[3] = (uint8_t)hdr->type;
    put_u32_le(header + 4U, hdr->device_id);
    put_u64_le(header + 8U, hdr->from_seq);
    put_u64_le(header + 16U, hdr->to_seq);
    put_u16_le(header + 24U, hdr->payload_len);
    put_u16_le(header + 26U, hdr->flags);
    crc = fdb_edge_crc32c_update(fdb_edge_crc32c_init(), header, sizeof(header));
    crc = fdb_edge_crc32c_update(crc, payload, payload_len);
    put_u32_le(crc_bytes, fdb_edge_crc32c_final(crc));

    wire_len = cobs_encode_frame(header, payload, payload_len, crc_bytes, NULL);
    if (wire_len > out_cap || wire_len > FDB_EDGE_MAX_FRAME_BYTES) {
        return FDB_EDGE_ERR_BOUNDS;
    }
    if (payload_len > 0U) {
        /* Stage at the end so forward encoding never catches unread payload.
         * This also preserves callers that pass overlapping input and output. */
        staged_payload = memmove(out_wire + wire_len - payload_len, payload, payload_len);
    }
    (void)cobs_encode_frame(header, staged_payload, payload_len, crc_bytes, out_wire);
    *out_len = wire_len;
    return FDB_EDGE_OK;
}

static int decode_header(const uint8_t *raw, fdb_wal_frame_hdr_t *out_hdr)
{
    if (raw[0] != WAL_MAGIC_0 || raw[1] != WAL_MAGIC_1 || raw[2] != WAL_WIRE_VER) {
        return FDB_EDGE_ERR_CORRUPT;
    }
    out_hdr->type = (fdb_wal_msg_type_t)raw[3];
    out_hdr->device_id = get_u32_le(raw + 4U);
    out_hdr->from_seq = get_u64_le(raw + 8U);
    out_hdr->to_seq = get_u64_le(raw + 16U);
    out_hdr->payload_len = get_u16_le(raw + 24U);
    out_hdr->flags = get_u16_le(raw + 26U);
    return FDB_EDGE_OK;
}

static int decode_raw_frame(const uint8_t *raw, size_t raw_len, fdb_wal_frame_hdr_t *out_hdr)
{
    size_t crc_offset;
    if (raw_len < WAL_HDR_SIZE + WAL_CRC_SIZE ||
        decode_header(raw, out_hdr) != FDB_EDGE_OK ||
        raw_len - WAL_HDR_SIZE - WAL_CRC_SIZE != out_hdr->payload_len) {
        return FDB_EDGE_ERR_CORRUPT;
    }
    crc_offset = raw_len - WAL_CRC_SIZE;
    return get_u32_le(raw + crc_offset) == fdb_edge_crc32c(raw, crc_offset) ?
        FDB_EDGE_OK : FDB_EDGE_ERR_CORRUPT;
}

/* Only a destination beginning after its wire data needs a mapping. A full
 * 254-byte run introduces one skipped code. Other COBS codes map to zeros.
 * Keep this uncommon case's stack outside the normal, bounded decode path. */
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
static int copy_shifted_payload(cobs_reader_t reader, uint8_t *out, size_t len)
{
    uint8_t zeros[(FDB_EDGE_MAX_FRAME_BYTES + 7U) / 8U] = {0};
    uint16_t gaps[FDB_EDGE_MAX_FRAME_BYTES / 254U + 1U];
    size_t gap_count = 0U;
    size_t first_pos = 0U;
    size_t displacement;
    size_t pivot;
    size_t skipped;
    size_t i;

    for (i = 0U; i < len; i++) {
        uint8_t byte;
        if (cobs_next(&reader, &byte) != 1) {
            return FDB_EDGE_ERR_CORRUPT;
        }
        if (i == 0U) {
            first_pos = reader.pos - 1U;
        } else if (reader.pos - 1U > first_pos + i + gap_count) {
            if (gap_count == sizeof(gaps) / sizeof(gaps[0])) {
                return FDB_EDGE_ERR_CORRUPT;
            }
            gaps[gap_count++] = (uint16_t)i;
        }
        if (byte == 0U) {
            zeros[i / 8U] |= (uint8_t)(1U << (i % 8U));
        }
    }

    displacement = (size_t)((uintptr_t)out - (uintptr_t)(reader.src + first_pos));
    /* The source falls farther ahead after each skipped code. Copy the
     * right-shifted prefix backwards and the remaining suffix forwards. */
    pivot = displacement <= gap_count ? gaps[displacement - 1U] : len;
    skipped = pivot < len ? displacement - 1U : gap_count;
    for (i = pivot; i > 0U;) {
        i--;
        out[i] = (zeros[i / 8U] & (1U << (i % 8U))) != 0U ?
            0U : reader.src[first_pos + i + skipped];
        if (skipped > 0U && gaps[skipped - 1U] == i) {
            skipped--;
        }
    }
    skipped = displacement;
    for (i = pivot; i < len; i++) {
        out[i] = (zeros[i / 8U] & (1U << (i % 8U))) != 0U ?
            0U : reader.src[first_pos + i + skipped];
        if (skipped < gap_count && gaps[skipped] == i + 1U) {
            skipped++;
        }
    }
    return FDB_EDGE_OK;
}

static int copy_payload(cobs_reader_t reader, uint8_t *out, size_t len)
{
    cobs_reader_t first = reader;
    uint8_t byte;
    uintptr_t out_addr = (uintptr_t)out;
    uintptr_t src_addr = (uintptr_t)reader.src;

    if (out_addr > src_addr && out_addr - src_addr < reader.len &&
        cobs_next(&first, &byte) == 1 && out_addr > (uintptr_t)(first.src + first.pos - 1U)) {
        return copy_shifted_payload(reader, out, len);
    }
    return cobs_read(&reader, out, len);
}

int fdb_wal_decode_frame(
    const uint8_t *wire_no_delim,
    size_t wire_len,
    fdb_wal_frame_hdr_t *out_hdr,
    uint8_t *out_payload,
    size_t payload_cap)
{
    cobs_reader_t reader = {wire_no_delim, wire_len, 0U, 0U, false};
    cobs_reader_t payload_start;
    uint8_t scratch[64];
    fdb_wal_frame_hdr_t hdr;
    size_t remaining;
    uint32_t crc;
    uint8_t extra;

    if (!wire_no_delim || !out_hdr) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (wire_len >= FDB_EDGE_MAX_FRAME_BYTES ||
        cobs_read(&reader, scratch, WAL_HDR_SIZE) != FDB_EDGE_OK ||
        decode_header(scratch, &hdr) != FDB_EDGE_OK) {
        return FDB_EDGE_ERR_CORRUPT;
    }
    payload_start = reader;
    crc = fdb_edge_crc32c_update(fdb_edge_crc32c_init(), scratch, WAL_HDR_SIZE);
    remaining = hdr.payload_len;
    while (remaining > 0U) {
        size_t len = remaining < sizeof(scratch) ? remaining : sizeof(scratch);
        if (cobs_read(&reader, scratch, len) != FDB_EDGE_OK) {
            return FDB_EDGE_ERR_CORRUPT;
        }
        crc = fdb_edge_crc32c_update(crc, scratch, len);
        remaining -= len;
    }
    if (cobs_read(&reader, scratch, WAL_CRC_SIZE) != FDB_EDGE_OK ||
        cobs_next(&reader, &extra) != 0 || get_u32_le(scratch) != fdb_edge_crc32c_final(crc)) {
        return FDB_EDGE_ERR_CORRUPT;
    }
    if (hdr.payload_len > payload_cap) {
        return FDB_EDGE_ERR_BOUNDS;
    }
    if (hdr.payload_len > 0U && out_payload &&
        copy_payload(payload_start, out_payload, hdr.payload_len) != FDB_EDGE_OK) {
        return FDB_EDGE_ERR_CORRUPT;
    }
    *out_hdr = hdr;
    return FDB_EDGE_OK;
}

typedef struct {
    uint32_t device_id;
    uint8_t *out_stream;
    size_t out_cap;
    size_t written;
    uint32_t frames;
    int err;
} wal_export_ctx_t;

static bool wal_export_cb(
    const uint8_t *payload,
    size_t payload_len,
    uint64_t min_mono_seq,
    uint64_t max_mono_seq,
    int64_t min_wall_ms,
    int64_t max_wall_ms,
    uint32_t sample_count,
    void *user_ctx)
{
    wal_export_ctx_t *ctx = (wal_export_ctx_t *)user_ctx;
    fdb_wal_frame_hdr_t hdr;
    size_t frame_len = 0U;
    int rc;

    (void)min_wall_ms;
    (void)max_wall_ms;
    (void)sample_count;

    if (payload_len > UINT16_MAX) {
        ctx->err = FDB_EDGE_ERR_BOUNDS;
        return false;
    }
    hdr.type = FDB_WAL_MSG_DATA;
    hdr.device_id = ctx->device_id;
    hdr.from_seq = min_mono_seq;
    hdr.to_seq = max_mono_seq;
    hdr.flags = 0U;
    hdr.payload_len = (uint16_t)payload_len;

    rc = fdb_wal_encode_frame(
        &hdr,
        payload,
        ctx->out_stream + ctx->written,
        ctx->out_cap - ctx->written,
        &frame_len);
    if (rc != FDB_EDGE_OK) {
        ctx->err = rc;
        return false;
    }
    ctx->written += frame_len;
    ctx->frames++;
    return true;
}

int fdb_wal_export_after(
    fdb_edge_tsdb_t *src_db,
    uint32_t device_id,
    uint64_t acked_seq,
    uint8_t *out_stream,
    size_t out_cap,
    size_t *out_stream_len,
    uint32_t *out_frame_count)
{
    wal_export_ctx_t ctx;
    int rc;

    if (!src_db || !out_stream || !out_stream_len) {
        return FDB_EDGE_ERR_INVALID;
    }
    *out_stream_len = 0U;
    if (out_frame_count) {
        *out_frame_count = 0U;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.device_id = device_id;
    ctx.out_stream = out_stream;
    ctx.out_cap = out_cap;
    ctx.err = FDB_EDGE_OK;

    rc = fdb_edge_tsdb_iter_raw_blocks_after(src_db, acked_seq, wal_export_cb, &ctx);
    if (rc != FDB_EDGE_OK) {
        return rc;
    }
    *out_stream_len = ctx.written;
    if (out_frame_count) {
        *out_frame_count = ctx.frames;
    }
    return ctx.err;
}

void fdb_wal_receiver_init(
    fdb_wal_receiver_t *rx,
    fdb_edge_tsdb_t *dest_db,
    uint32_t expected_device_id)
{
    if (!rx) {
        return;
    }
    memset(rx, 0, sizeof(*rx));
    rx->dest_db = dest_db;
    rx->expected_device_id = expected_device_id;
    if (dest_db) {
        rx->peer_last_seq = dest_db->hlc.monotonic_seq;
    }
}

static void emit_reply(
    uint8_t *out_reply_wire,
    size_t reply_cap,
    size_t *out_reply_len,
    fdb_wal_msg_type_t type,
    uint32_t dev_id,
    uint64_t seq)
{
    fdb_wal_frame_hdr_t reply;
    size_t flen = 0U;

    if (!out_reply_wire || !out_reply_len || reply_cap == 0U || *out_reply_len >= reply_cap) {
        return;
    }
    reply.type = type;
    reply.device_id = dev_id;
    reply.from_seq = seq;
    reply.to_seq = seq;
    reply.flags = 0U;
    reply.payload_len = 0U;
    if (fdb_wal_encode_frame(
            &reply,
            NULL,
            out_reply_wire + *out_reply_len,
            reply_cap - *out_reply_len,
            &flen) == FDB_EDGE_OK) {
        *out_reply_len += flen;
    }
}

int fdb_wal_receiver_feed(
    fdb_wal_receiver_t *rx,
    const uint8_t *bytes,
    size_t len,
    uint8_t *out_reply_wire,
    size_t reply_cap,
    size_t *out_reply_len)
{
    size_t i;

    if (!rx || !rx->dest_db || (!bytes && len > 0U)) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (out_reply_len) {
        *out_reply_len = 0U;
    }

    for (i = 0; i < len; i++) {
        uint8_t b = bytes[i];
        if (b == 0x00U) {
            fdb_wal_frame_hdr_t hdr;
            size_t raw_len = 0U;
            uint64_t next_seq = rx->peer_last_seq;
            if (next_seq != UINT64_MAX) {
                next_seq++;
            }
            if (!rx->overflowed && rx->rx_len == 0U) {
                continue;
            }
            /* A decoded prefix is never longer than its COBS prefix. */
            if (rx->overflowed ||
                cobs_decode(rx->rx_cobs, rx->rx_len, rx->rx_cobs, sizeof(rx->rx_cobs), &raw_len) != FDB_EDGE_OK ||
                decode_raw_frame(rx->rx_cobs, raw_len, &hdr) != FDB_EDGE_OK) {
                rx->frames_corrupt++;
                rx->overflowed = false;
                rx->rx_len = 0U;
                emit_reply(
                    out_reply_wire, reply_cap, out_reply_len,
                    FDB_WAL_MSG_NACK, rx->expected_device_id, next_seq);
                continue;
            }
            rx->rx_len = 0U;

            if (hdr.device_id != rx->expected_device_id || hdr.type != FDB_WAL_MSG_DATA) {
                continue;
            }
            if (hdr.from_seq == 0ULL || hdr.from_seq > hdr.to_seq || hdr.payload_len == 0U) {
                rx->frames_corrupt++;
                emit_reply(
                    out_reply_wire, reply_cap, out_reply_len,
                    FDB_WAL_MSG_NACK, rx->expected_device_id, next_seq);
                continue;
            }
            if (hdr.to_seq <= rx->peer_last_seq) {
                rx->frames_duplicate++;
                emit_reply(
                    out_reply_wire, reply_cap, out_reply_len,
                    FDB_WAL_MSG_ACK, rx->expected_device_id, rx->peer_last_seq);
                continue;
            }
            if (rx->peer_last_seq > 0ULL && hdr.from_seq != next_seq) {
                rx->frames_out_of_order++;
                emit_reply(
                    out_reply_wire, reply_cap, out_reply_len,
                    FDB_WAL_MSG_NACK, rx->expected_device_id, next_seq);
                continue;
            }
            if (fdb_edge_tsdb_append_raw_block_range(
                    rx->dest_db, rx->rx_cobs + WAL_HDR_SIZE, hdr.payload_len,
                    hdr.from_seq, hdr.to_seq) != FDB_EDGE_OK) {
                rx->frames_corrupt++;
                emit_reply(
                    out_reply_wire, reply_cap, out_reply_len,
                    FDB_WAL_MSG_NACK, rx->expected_device_id, next_seq);
                continue;
            }

            rx->peer_last_seq = hdr.to_seq;
            rx->frames_accepted++;
            emit_reply(
                out_reply_wire, reply_cap, out_reply_len,
                FDB_WAL_MSG_ACK, rx->expected_device_id, rx->peer_last_seq);
        } else {
            if (!rx->overflowed) {
                if (rx->rx_len < sizeof(rx->rx_cobs) - 1U) {
                    rx->rx_cobs[rx->rx_len++] = b;
                } else {
                    rx->overflowed = true;
                }
            }
        }
    }
    return FDB_EDGE_OK;
}
