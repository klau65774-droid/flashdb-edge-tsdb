#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fdb_edge/fdb_edge_flash_sim.h"
#include "fdb_edge/fdb_edge_wal_sync.h"
#include "test_common.h"

#define TEST_SECTOR_SIZE 4096U
#define TEST_SECTORS 4U
#define TEST_SAMPLES 80U
#define TEST_SHORT_FRAMES 90U
#define TEST_DEVICE_ID 42U

static void prepare_fixture(void)
{
    const uint8_t short_frame[] = {1U, 0U};
    fdb_flash_sim_t sim;
    fdb_flash_ops_t flash;
    fdb_edge_tsdb_config_t cfg;
    static fdb_edge_tsdb_t db;
    static uint8_t wire[8192];
    size_t wire_len;
    uint32_t frames;
    uint32_t seq;
    unsigned i;
    const uint8_t *first_delim;
    FILE *wal;

    CHECK(fdb_flash_sim_alloc_init(&sim, TEST_SECTORS * TEST_SECTOR_SIZE,
                                  TEST_SECTOR_SIZE, 1U) == FDB_EDGE_OK);
    fdb_flash_sim_as_ops(&sim, &flash);
    fdb_edge_tsdb_default_config(&cfg, 1U);
    cfg.batch_size = 1U;
    CHECK(fdb_edge_tsdb_open(&db, &flash, &cfg) == FDB_EDGE_OK);
    for (seq = 1U; seq <= TEST_SAMPLES; seq++) {
        double value = (double)seq / 2.0;
        CHECK(fdb_edge_tsdb_append(&db, (int64_t)seq * 11, &value, 1U, NULL) == FDB_EDGE_OK);
    }
    CHECK(fdb_edge_tsdb_flush(&db) == FDB_EDGE_OK);
    CHECK(fdb_wal_export_after(&db, TEST_DEVICE_ID, 0ULL, wire, sizeof(wire), &wire_len, &frames) == FDB_EDGE_OK);
    CHECK(frames == TEST_SAMPLES && wire_len > 1024U);

    wal = fopen("input.wal", "wb");
    CHECK(wal);
    /* More replies than the former 2048-byte reply buffer could hold per read. */
    for (i = 0U; i < TEST_SHORT_FRAMES; i++) {
        CHECK(fwrite(short_frame, 1U, sizeof(short_frame), wal) == sizeof(short_frame));
    }
    CHECK(fwrite(wire, 1U, wire_len, wal) == wire_len);
    CHECK(fclose(wal) == 0);

    first_delim = memchr(wire, 0, wire_len);
    CHECK(first_delim && first_delim > wire);
    wal = fopen("incomplete.wal", "wb");
    CHECK(wal);
    {
        size_t prefix_len = (size_t)(first_delim - wire);
        CHECK(fwrite(wire, 1U, prefix_len, wal) == prefix_len);
    }
    CHECK(fclose(wal) == 0);
    fdb_flash_sim_free(&sim);
}

static int read_image(void *ctx, uint32_t addr, uint8_t *buf, size_t size)
{
    FILE *fp = (FILE *)ctx;

    CHECK(addr <= TEST_SECTORS * TEST_SECTOR_SIZE && size <= TEST_SECTORS * TEST_SECTOR_SIZE - addr);
    CHECK(fseek(fp, (long)addr, SEEK_SET) == 0);
    CHECK(fread(buf, 1U, size, fp) == size);
    return (int)size;
}

static int readonly_write(void *ctx, uint32_t addr, const uint8_t *buf, size_t size)
{
    (void)ctx;
    (void)addr;
    (void)buf;
    (void)size;
    return FDB_EDGE_ERR_IO;
}

static int readonly_erase(void *ctx, uint32_t addr, size_t size)
{
    (void)ctx;
    (void)addr;
    (void)size;
    return FDB_EDGE_ERR_IO;
}

static bool verify_sample(const fdb_edge_sample_t *sample, void *ctx)
{
    uint64_t *count = (uint64_t *)ctx;

    (*count)++;
    CHECK(sample->mono_seq == *count && sample->wall_ts_ms == (int64_t)*count * 11);
    CHECK(sample->num_channels == 1U && sample->values[0] == (double)*count / 2.0);
    return true;
}

static void verify_fixture(const char *ack_path, bool replay)
{
    FILE *image = fopen("collector.bin", "rb");
    FILE *ack = fopen(ack_path, "rb");
    fdb_flash_ops_t flash;
    fdb_edge_tsdb_config_t cfg;
    static fdb_edge_tsdb_t db;
    fdb_edge_tsdb_stats_t stats;
    size_t i;
    uint64_t count = 0ULL;

    CHECK(image && ack);
    flash.ctx = image;
    flash.total_size = TEST_SECTORS * TEST_SECTOR_SIZE;
    flash.sector_size = TEST_SECTOR_SIZE;
    flash.write_gran = 1U;
    flash.read = read_image;
    flash.write = readonly_write;
    flash.erase = readonly_erase;
    fdb_edge_tsdb_default_config(&cfg, 1U);
    CHECK(fdb_edge_tsdb_open(&db, &flash, &cfg) == FDB_EDGE_OK);
    CHECK(fdb_edge_tsdb_query_range_mono(&db, 1ULL, UINT64_MAX, verify_sample, &count) == FDB_EDGE_OK);
    fdb_edge_tsdb_get_stats(&db, &stats);
    CHECK(count == TEST_SAMPLES && stats.last_mono_seq == TEST_SAMPLES);
    CHECK(stats.committed_blocks == TEST_SAMPLES && stats.total_samples == TEST_SAMPLES);
    CHECK(fclose(image) == 0);

    for (i = 0U; i < TEST_SHORT_FRAMES + TEST_SAMPLES; i++) {
        uint8_t reply[34];
        fdb_wal_frame_hdr_t reply_hdr;
        bool is_nack = i < TEST_SHORT_FRAMES;
        uint64_t expected_seq = is_nack ? (replay ? TEST_SAMPLES + 1U : 1U) :
            (replay ? TEST_SAMPLES : (uint64_t)(i - TEST_SHORT_FRAMES + 1U));

        CHECK(fread(reply, 1U, sizeof(reply), ack) == sizeof(reply));
        CHECK(reply[sizeof(reply) - 1U] == 0U);
        CHECK(fdb_wal_decode_frame(reply, sizeof(reply) - 1U, &reply_hdr, NULL, 0U) == FDB_EDGE_OK);
        CHECK(reply_hdr.type == (is_nack ? FDB_WAL_MSG_NACK : FDB_WAL_MSG_ACK));
        CHECK(reply_hdr.device_id == TEST_DEVICE_ID && reply_hdr.payload_len == 0U);
        CHECK(reply_hdr.from_seq == expected_seq && reply_hdr.to_seq == expected_seq);
    }
    CHECK(fgetc(ack) == EOF && !ferror(ack));
    CHECK(fclose(ack) == 0);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (strcmp(argv[1], "prepare") == 0) {
        prepare_fixture();
    } else if (strcmp(argv[1], "verify_ingest") == 0) {
        verify_fixture("ingest.ack", false);
    } else {
        CHECK(strcmp(argv[1], "verify_replay") == 0);
        verify_fixture("replay.ack", true);
    }
    return 0;
}
