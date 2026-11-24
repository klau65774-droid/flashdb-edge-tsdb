#include <stdint.h>
#include <string.h>

#include "fdb_edge/fdb_edge_crc32c.h"
#include "fdb_edge/fdb_edge_flash_sim.h"
#include "fdb_edge/fdb_edge_wal_sync.h"
#include "test_common.h"

static void check_reply(const uint8_t *wire, size_t wire_len, fdb_wal_msg_type_t type, uint64_t seq)
{
    fdb_wal_frame_hdr_t hdr;

    CHECK(wire_len == 34U && wire[wire_len - 1U] == 0U);
    CHECK(fdb_wal_decode_frame(wire, wire_len - 1U, &hdr, NULL, 0U) == FDB_EDGE_OK);
    CHECK(hdr.type == type && hdr.from_seq == seq && hdr.to_seq == seq && hdr.payload_len == 0U);
}

static void test_cobs_boundaries(void)
{
    fdb_wal_frame_hdr_t hdr = {FDB_WAL_MSG_DATA, 42U, 1ULL, 1ULL, 0U, 250U};
    fdb_wal_frame_hdr_t decoded;
    uint8_t payload[250];
    uint8_t output[286];
    uint8_t roundtrip[250];
    uint8_t invalid_wire[33] = {33U, 'W', 'L', 1U, FDB_WAL_MSG_ACK};
    uint32_t invalid_crc;
    size_t wire_len = 123U;
    size_t i;

    memset(payload, 0xA5, sizeof(payload));
    memset(output, 0xCC, sizeof(output));
    CHECK(fdb_wal_encode_frame(&hdr, payload, output, 0U, &wire_len) == FDB_EDGE_ERR_BOUNDS);
    CHECK(wire_len == 123U && output[0] == 0xCCU);

    /* The payload and its CRC form one terminal run of exactly 254 nonzero bytes. */
    CHECK(fdb_wal_encode_frame(&hdr, payload, output, 284U, &wire_len) == FDB_EDGE_OK);
    CHECK(wire_len == 284U && output[28] == 0xFFU && output[283] == 0U && output[284] == 0xCCU);
    CHECK(fdb_wal_decode_frame(output, wire_len - 1U, &decoded, roundtrip, sizeof(roundtrip)) == FDB_EDGE_OK);
    CHECK(decoded.payload_len == sizeof(payload) && memcmp(payload, roundtrip, sizeof(payload)) == 0);
    output[283] = 1U; /* Version 1 writers also emitted a redundant terminal code. */
    output[284] = 0U;
    CHECK(fdb_wal_decode_frame(output, 284U, &decoded, roundtrip, sizeof(roundtrip)) == FDB_EDGE_OK);
    CHECK(memcmp(payload, roundtrip, sizeof(payload)) == 0);

    memcpy(output + 2U, payload, sizeof(payload));
    CHECK(fdb_wal_encode_frame(&hdr, output + 2U, output, 284U, &wire_len) == FDB_EDGE_OK);
    CHECK(fdb_wal_decode_frame(output, wire_len - 1U, &decoded, roundtrip, sizeof(roundtrip)) == FDB_EDGE_OK);
    CHECK(memcmp(payload, roundtrip, sizeof(payload)) == 0);

    hdr.type = FDB_WAL_MSG_ACK;
    hdr.device_id = 549U; /* This fixture's CRC ends with a zero byte. */
    hdr.payload_len = 0U;
    CHECK(fdb_wal_encode_frame(&hdr, NULL, output, 34U, &wire_len) == FDB_EDGE_OK);
    CHECK(output[32] == 1U);
    check_reply(output, wire_len, FDB_WAL_MSG_ACK, 1ULL);
    hdr.type = FDB_WAL_MSG_NACK;
    CHECK(fdb_wal_encode_frame(&hdr, NULL, output, 34U, &wire_len) == FDB_EDGE_OK);
    check_reply(output, wire_len, FDB_WAL_MSG_NACK, 1ULL);
    hdr.type = FDB_WAL_MSG_HEARTBEAT;
    CHECK(fdb_wal_encode_frame(&hdr, NULL, output, 34U, &wire_len) == FDB_EDGE_OK);
    check_reply(output, wire_len, FDB_WAL_MSG_HEARTBEAT, 1ULL);

    /* A zero inside a run is invalid even when the raw bytes have a valid CRC. */
    invalid_crc = fdb_edge_crc32c(invalid_wire + 1U, 28U);
    for (i = 0U; i < 4U; i++) {
        invalid_wire[29U + i] = (uint8_t)(invalid_crc >> (i * 8U));
    }
    CHECK(fdb_wal_decode_frame(invalid_wire, sizeof(invalid_wire), &decoded, NULL, 0U) == FDB_EDGE_ERR_CORRUPT);
}

static void test_frame_capacity(void)
{
    static uint8_t payload[FDB_EDGE_MAX_FRAME_BYTES];
    static uint8_t wire[FDB_EDGE_MAX_FRAME_BYTES + 1U];
    static uint8_t roundtrip[FDB_EDGE_MAX_FRAME_BYTES];
    fdb_wal_frame_hdr_t hdr = {FDB_WAL_MSG_DATA, 42U, 1ULL, 1ULL, 0U, 0U};
    fdb_wal_frame_hdr_t decoded;
    size_t wire_len = 0U;

    hdr.payload_len = (uint16_t)(FDB_EDGE_MAX_FRAME_BYTES - 34U);
    wire[FDB_EDGE_MAX_FRAME_BYTES] = 0xCCU;
    CHECK(fdb_wal_encode_frame(&hdr, payload, wire, FDB_EDGE_MAX_FRAME_BYTES, &wire_len) == FDB_EDGE_OK);
    CHECK(wire_len == FDB_EDGE_MAX_FRAME_BYTES && wire[FDB_EDGE_MAX_FRAME_BYTES] == 0xCCU);
    CHECK(fdb_wal_decode_frame(wire, wire_len - 1U, &decoded, roundtrip, hdr.payload_len) == FDB_EDGE_OK);
    CHECK(decoded.payload_len == hdr.payload_len && memcmp(payload, roundtrip, hdr.payload_len) == 0);
    hdr.payload_len++;
    CHECK(fdb_wal_encode_frame(&hdr, payload, wire, sizeof(wire), &wire_len) == FDB_EDGE_ERR_BOUNDS);
    CHECK(wire[FDB_EDGE_MAX_FRAME_BYTES] == 0xCCU);
}

static void test_payload_overlap(void)
{
    static const size_t shifts[] = {0U, 28U, 29U, 30U, 31U, 32U, 33U, 45U, 700U};
    uint8_t payload[800];
    uint8_t golden[900];
    uint8_t aliased[1600];
    fdb_wal_frame_hdr_t hdr = {FDB_WAL_MSG_DATA, 42U, 1ULL, 1ULL, 0U, sizeof(payload)};
    fdb_wal_frame_hdr_t decoded;
    size_t wire_len;
    size_t i;

    memset(payload, 0xA5, sizeof(payload));
    payload[254] = 0U;
    payload[511] = 0U;
    payload[799] = 0U;
    CHECK(fdb_wal_encode_frame(&hdr, payload, golden, sizeof(golden), &wire_len) == FDB_EDGE_OK);
    for (i = 0U; i < sizeof(shifts) / sizeof(shifts[0]); i++) {
        memcpy(aliased, golden, wire_len);
        CHECK(fdb_wal_decode_frame(aliased, wire_len - 1U, &decoded,
                                  aliased + shifts[i], sizeof(payload)) == FDB_EDGE_OK);
        CHECK(memcmp(aliased + shifts[i], payload, sizeof(payload)) == 0);
    }
}

static void setup_receiver(fdb_flash_sim_t *sim, fdb_edge_tsdb_t *db, fdb_wal_receiver_t *rx)
{
    fdb_flash_ops_t ops;
    fdb_edge_tsdb_config_t cfg;

    CHECK(fdb_flash_sim_alloc_init(sim, 4U * 4096U, 4096U, 1U) == FDB_EDGE_OK);
    fdb_flash_sim_as_ops(sim, &ops);
    fdb_edge_tsdb_default_config(&cfg, 1U);
    CHECK(fdb_edge_tsdb_open(db, &ops, &cfg) == FDB_EDGE_OK);
    fdb_wal_receiver_init(rx, db, 42U);
}

static size_t data_frame(uint8_t *wire, size_t cap, uint64_t payload_first, uint64_t payload_last,
                         uint64_t header_first, uint64_t header_last)
{
    fdb_edge_sample_t samples[2] = {0};
    fdb_wal_frame_hdr_t hdr = {FDB_WAL_MSG_DATA, 42U, header_first, header_last, 0U, 0U};
    uint8_t payload[128];
    size_t payload_len = 0U;
    size_t wire_len = 0U;

    samples[0].num_channels = 1U;
    samples[0].mono_seq = payload_first;
    samples[1].num_channels = 1U;
    samples[1].mono_seq = payload_last;
    CHECK(fdb_edge_codec_encode(samples, payload_first == payload_last ? 1U : 2U,
                                payload, sizeof(payload), &payload_len) == FDB_EDGE_OK);
    hdr.payload_len = (uint16_t)payload_len;
    CHECK(fdb_wal_encode_frame(&hdr, payload, wire, cap, &wire_len) == FDB_EDGE_OK);
    return wire_len;
}

static void test_receiver_resync(void)
{
    fdb_flash_sim_t sim;
    static fdb_edge_tsdb_t db;
    static fdb_wal_receiver_t rx;
    uint8_t part[32];
    uint8_t following[256] = {0U, 0U, 1U, 0U};
    uint8_t replies[128];
    size_t wire_len;
    size_t reply_len = 0U;
    size_t remaining = FDB_EDGE_MAX_FRAME_BYTES + 17U;

    setup_receiver(&sim, &db, &rx);
    memset(part, 0x7F, sizeof(part));
    while (remaining > 0U) {
        size_t size = remaining < sizeof(part) ? remaining : sizeof(part);
        CHECK(fdb_wal_receiver_feed(&rx, part, size, replies, sizeof(replies), &reply_len) == FDB_EDGE_OK);
        CHECK(reply_len == 0U);
        remaining -= size;
    }
    CHECK(rx.overflowed);
    wire_len = data_frame(following + 4U, sizeof(following) - 4U, 1ULL, 2ULL, 1ULL, 2ULL);
    CHECK(fdb_wal_receiver_feed(&rx, following, wire_len + 4U,
                                replies, sizeof(replies), &reply_len) == FDB_EDGE_OK);
    CHECK(!rx.overflowed && rx.rx_len == 0U && rx.frames_corrupt == 2ULL);
    CHECK(rx.frames_accepted == 1ULL && rx.peer_last_seq == 2ULL && reply_len == 102U);
    check_reply(replies, 34U, FDB_WAL_MSG_NACK, 1ULL);
    check_reply(replies + 34U, 34U, FDB_WAL_MSG_NACK, 1ULL);
    check_reply(replies + 68U, 34U, FDB_WAL_MSG_ACK, 2ULL);
    fdb_flash_sim_free(&sim);
}

static void test_receiver_sequences(void)
{
    fdb_flash_sim_t sim;
    static fdb_edge_tsdb_t db;
    static fdb_wal_receiver_t rx;
    uint8_t wire[256];
    uint8_t reply[64];
    size_t wire_len;
    size_t reply_len;

    setup_receiver(&sim, &db, &rx);
    wire_len = data_frame(wire, sizeof(wire), 1ULL, 2ULL, 1ULL, 3ULL);
    CHECK(fdb_wal_receiver_feed(&rx, wire, wire_len, reply, sizeof(reply), &reply_len) == FDB_EDGE_OK);
    CHECK(rx.frames_corrupt == 1ULL && rx.peer_last_seq == 0ULL && db.hlc.monotonic_seq == 0ULL);
    check_reply(reply, reply_len, FDB_WAL_MSG_NACK, 1ULL);

    wire_len = data_frame(wire, sizeof(wire), 1ULL, 2ULL, 1ULL, 2ULL);
    CHECK(fdb_wal_receiver_feed(&rx, wire, wire_len, reply, sizeof(reply), &reply_len) == FDB_EDGE_OK);
    check_reply(reply, reply_len, FDB_WAL_MSG_ACK, 2ULL);

    wire_len = data_frame(wire, sizeof(wire), 2ULL, 3ULL, 2ULL, 3ULL);
    CHECK(fdb_wal_receiver_feed(&rx, wire, wire_len, reply, sizeof(reply), &reply_len) == FDB_EDGE_OK);
    CHECK(rx.frames_out_of_order == 1ULL && rx.peer_last_seq == 2ULL && db.hlc.monotonic_seq == 2ULL);
    check_reply(reply, reply_len, FDB_WAL_MSG_NACK, 3ULL);

    wire_len = data_frame(wire, sizeof(wire), 3ULL, 3ULL, 3ULL, 3ULL);
    CHECK(fdb_wal_receiver_feed(&rx, wire, wire_len, reply, sizeof(reply), &reply_len) == FDB_EDGE_OK);
    CHECK(rx.frames_accepted == 2ULL && db.hlc.monotonic_seq == 3ULL);
    check_reply(reply, reply_len, FDB_WAL_MSG_ACK, 3ULL);
    fdb_flash_sim_free(&sim);

    setup_receiver(&sim, &db, &rx);
    wire_len = data_frame(wire, sizeof(wire), UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX);
    CHECK(fdb_wal_receiver_feed(&rx, wire, wire_len, reply, sizeof(reply), &reply_len) == FDB_EDGE_OK);
    check_reply(reply, reply_len, FDB_WAL_MSG_ACK, UINT64_MAX);
    wire[0] = 1U;
    wire[1] = 0U;
    CHECK(fdb_wal_receiver_feed(&rx, wire, 2U, reply, sizeof(reply), &reply_len) == FDB_EDGE_OK);
    check_reply(reply, reply_len, FDB_WAL_MSG_NACK, UINT64_MAX);
    fdb_flash_sim_free(&sim);
}

static void test_export_prefix(void)
{
    fdb_flash_sim_t sim;
    static fdb_edge_tsdb_t db;
    static fdb_wal_receiver_t rx;
    uint8_t full[256];
    uint8_t partial[256];
    const double value = 1.25;
    size_t full_len;
    size_t partial_len;
    size_t first_len;
    const uint8_t *first_delim;
    uint32_t count;

    setup_receiver(&sim, &db, &rx);
    CHECK(fdb_edge_tsdb_append(&db, 1000, &value, 1U, NULL) == FDB_EDGE_OK);
    CHECK(fdb_edge_tsdb_flush(&db) == FDB_EDGE_OK);
    CHECK(fdb_edge_tsdb_append(&db, 2000, &value, 1U, NULL) == FDB_EDGE_OK);
    CHECK(fdb_edge_tsdb_flush(&db) == FDB_EDGE_OK);
    CHECK(fdb_wal_export_after(&db, 42U, 0ULL, full, sizeof(full), &full_len, &count) == FDB_EDGE_OK);
    CHECK(count == 2U);
    first_delim = memchr(full, 0, full_len);
    CHECK(first_delim);
    first_len = (size_t)(first_delim - full) + 1U;

    CHECK(fdb_wal_export_after(&db, 42U, 0ULL, partial, first_len, &partial_len, &count) == FDB_EDGE_ERR_BOUNDS);
    CHECK(count == 1U && partial_len == first_len && memcmp(full, partial, first_len) == 0);
    CHECK(fdb_wal_export_after(&db, 42U, 0ULL, partial, 0U, &partial_len, &count) == FDB_EDGE_ERR_BOUNDS);
    CHECK(count == 0U && partial_len == 0U);
    CHECK(fdb_wal_export_after(&db, 42U, 1ULL, partial, sizeof(partial), &partial_len, &count) == FDB_EDGE_OK);
    CHECK(count == 1U && partial_len == full_len - first_len &&
          memcmp(full + first_len, partial, partial_len) == 0);
    fdb_flash_sim_free(&sim);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (strcmp(argv[1], "cobs") == 0) {
        test_cobs_boundaries();
        test_frame_capacity();
        test_payload_overlap();
    } else if (strcmp(argv[1], "resync") == 0) {
        test_receiver_resync();
    } else if (strcmp(argv[1], "export") == 0) {
        test_export_prefix();
    } else {
        CHECK(strcmp(argv[1], "sequences") == 0);
        test_receiver_sequences();
    }
    return 0;
}
