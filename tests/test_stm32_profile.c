#include <stdio.h>
#include <string.h>

#include <fal.h>
#include "fdb_edge/fdb_edge_stm32.h"
#include "fdb_edge/fdb_edge_wal_sync.h"
#include "test_common.h"

_Static_assert(FDB_EDGE_MAX_TRACKED_SECTORS == 128U, "STM32 sector count must be configurable");
_Static_assert(FDB_EDGE_MAX_BLOCK_SAMPLES == 64U, "STM32 sample count must be configurable");
_Static_assert(FDB_EDGE_MAX_CHANNELS == 8U, "STM32 channel count must be configurable");
_Static_assert(FDB_EDGE_MAX_WORK_BUF_BYTES == 4096U, "STM32 workspace must be configurable");

static struct fal_partition part = {0U, "data", "nor", 0L, 2U * 4096U, 0U};
static struct fal_flash_dev dev = {"nor", 0U, 1024U * 1024U, 4096U, {0}, 8U};
static uint8_t storage[2U * 4096U];
static bool short_transfer;

int fal_init(void)
{
    return 1;
}

const struct fal_flash_dev *fal_flash_device_find(const char *name)
{
    return strcmp(name, "nor") == 0 ? &dev : NULL;
}

const struct fal_partition *fal_partition_find(const char *name)
{
    return strcmp(name, "data") == 0 ? &part : NULL;
}

int fal_partition_read(const struct fal_partition *partition, uint32_t addr, uint8_t *buf, size_t size)
{
    CHECK(partition == &part && size <= sizeof(storage) && addr <= sizeof(storage) - size);
    if (short_transfer) {
        return (int)size - 1;
    }
    memcpy(buf, storage + addr, size);
    return (int)size;
}

int fal_partition_write(const struct fal_partition *partition, uint32_t addr, const uint8_t *buf, size_t size)
{
    CHECK(partition == &part && size <= sizeof(storage) && addr <= sizeof(storage) - size);
    if (short_transfer) {
        return (int)size - 1;
    }
    memcpy(storage + addr, buf, size);
    return (int)size;
}

int fal_partition_erase(const struct fal_partition *partition, uint32_t addr, size_t size)
{
    CHECK(partition == &part && size <= sizeof(storage) && addr <= sizeof(storage) - size);
    if (short_transfer) {
        return (int)size - 1;
    }
    memset(storage + addr, 0xFF, size);
    return (int)size;
}

static int record_tx(const uint8_t *buf, size_t len, void *user_ctx)
{
    size_t *sent = user_ctx;
    CHECK(len > 0U && buf[len - 1U] == 0U);
    *sent = len;
    return FDB_EDGE_OK;
}

int main(void)
{
    static const uint32_t granularities[] = {1U, 4U, 8U, 32U};
    static fdb_edge_tsdb_t db;
    stm32_fal_ctx_t ctx;
    fdb_flash_ops_t ops;
    fdb_edge_tsdb_config_t cfg;
    uint8_t word[32] = {0};
    size_t i;

    CHECK(fdb_stm32_bind_fal_partition("missing", &ctx, &ops, 1U) == FDB_EDGE_ERR_IO);
    CHECK(fdb_stm32_bind_fal_partition("data", &ctx, &ops, 2U) == FDB_EDGE_ERR_INVALID);
    part.offset = 1L;
    CHECK(fdb_stm32_bind_fal_partition("data", &ctx, &ops, 1U) == FDB_EDGE_ERR_INVALID);
    part.offset = 0L;
    part.len = (FDB_EDGE_MAX_TRACKED_SECTORS + 1U) * dev.blk_size;
    CHECK(fdb_stm32_bind_fal_partition("data", &ctx, &ops, 1U) == FDB_EDGE_ERR_INVALID);
    part.len = sizeof(storage);

    fdb_edge_tsdb_default_config(&cfg, 1U);
    for (i = 0U; i < sizeof(granularities) / sizeof(granularities[0]); i++) {
        const double value = 5.0;
        uint32_t gran = granularities[i];
        CHECK(fdb_stm32_bind_fal_partition("data", &ctx, &ops, gran) == FDB_EDGE_OK);
        short_transfer = true;
        CHECK(ops.read(ops.ctx, 0U, word, gran) == FDB_EDGE_ERR_IO);
        CHECK(ops.write(ops.ctx, 0U, word, gran) == FDB_EDGE_ERR_IO);
        CHECK(ops.erase(ops.ctx, 0U, ops.sector_size) == FDB_EDGE_ERR_IO);
        CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_ERR_IO);
        short_transfer = false;
        CHECK(ops.erase(ops.ctx, 0U, sizeof(storage)) == FDB_EDGE_OK);
        CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);
        CHECK(fdb_edge_tsdb_append(&db, 123, &value, 1U, NULL) == FDB_EDGE_OK);
        CHECK(fdb_edge_tsdb_flush(&db) == FDB_EDGE_OK);
        CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);
        CHECK(db.hlc.monotonic_seq == 1U && db.hlc.last_wall_ms == 123);
    }

    {
        const double value = 6.0;
        uint8_t wire[512];
        size_t wire_len = 0U, first_frame = 0U, sent = 0U;
        uint32_t frames = 0U;

        CHECK(fdb_edge_tsdb_append(&db, 124, &value, 1U, NULL) == FDB_EDGE_OK);
        CHECK(fdb_edge_tsdb_flush(&db) == FDB_EDGE_OK);
        CHECK(fdb_wal_export_after(&db, 1U, 0U, wire, sizeof(wire), &wire_len, &frames) == FDB_EDGE_OK);
        CHECK(wire_len > 1U && frames == 2U);
        while (first_frame < wire_len && wire[first_frame] != 0U) {
            first_frame++;
        }
        first_frame++;
        CHECK(first_frame < wire_len);
        CHECK(fdb_stm32_flush_wal_over_uart(&db, 1U, 0U, wire, first_frame, record_tx, &sent) == FDB_EDGE_OK);
        CHECK(sent == first_frame);
        CHECK(fdb_stm32_flush_wal_over_uart(&db, 1U, 1U, wire, sizeof(wire), record_tx, &sent) == FDB_EDGE_OK);
        CHECK(sent == wire_len - first_frame);
    }

    printf("STM32 profile on host ABI: TSDB=%zu, sector_meta=%zu, sample=%zu, WAL receiver=%zu\n",
           sizeof(db), sizeof(db.sectors[0]), sizeof(db.stage[0]), sizeof(fdb_wal_receiver_t));
    return 0;
}
