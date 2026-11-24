/*
 * STM32 FAL Flash & UART DMA WAL Sync Port Template
 *
 * Compile inside your STM32CubeIDE / arm-none-eabi-gcc firmware target with:
 *   -DFDB_EDGE_STM32_HAL -include ports/stm32/fdb_cfg.h -Iinclude
 *
 * Zero dynamic heap allocations: uses caller-owned static fdb_edge_tsdb_t.
 */

#include "fdb_edge/fdb_edge_stm32.h"
#include "fdb_edge/fdb_edge_wal_sync.h"

#ifdef FDB_EDGE_STM32_HAL
#include <fal.h>

static int checked_transfer(int rc, size_t requested)
{
    if (rc < 0) {
        return rc;
    }
    return (size_t)rc == requested ? FDB_EDGE_OK : FDB_EDGE_ERR_IO;
}

static int stm32_fal_read(void *ctx, uint32_t addr, uint8_t *buf, size_t size)
{
    const stm32_fal_ctx_t *f = (const stm32_fal_ctx_t *)ctx;
    if (!f || !f->part) {
        return FDB_EDGE_ERR_INVALID;
    }
    return checked_transfer(fal_partition_read(f->part, addr, buf, size), size);
}

static int stm32_fal_write(void *ctx, uint32_t addr, const uint8_t *buf, size_t size)
{
    const stm32_fal_ctx_t *f = (const stm32_fal_ctx_t *)ctx;
    if (!f || !f->part) {
        return FDB_EDGE_ERR_INVALID;
    }
    return checked_transfer(fal_partition_write(f->part, addr, buf, size), size);
}

static int stm32_fal_erase(void *ctx, uint32_t addr, size_t size)
{
    const stm32_fal_ctx_t *f = (const stm32_fal_ctx_t *)ctx;
    if (!f || !f->part) {
        return FDB_EDGE_ERR_INVALID;
    }
    return checked_transfer(fal_partition_erase(f->part, addr, size), size);
}

int fdb_stm32_bind_fal_partition(
    const char *part_name,
    stm32_fal_ctx_t *out_ctx,
    fdb_flash_ops_t *out_ops,
    uint32_t write_gran_bytes)
{
    const struct fal_partition *part;
    const struct fal_flash_dev *dev;
    uint64_t partition_size;
    uint64_t flash_size;
    uint64_t block_size;
    uint64_t offset;

    if (!part_name || !out_ctx || !out_ops ||
        (write_gran_bytes != 1U && write_gran_bytes != 4U &&
         write_gran_bytes != 8U && write_gran_bytes != 32U) ||
        write_gran_bytes > FDB_EDGE_MAX_WRITE_GRAN) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (fal_init() < 0) {
        return FDB_EDGE_ERR_IO;
    }
    part = fal_partition_find(part_name);
    if (!part) {
        return FDB_EDGE_ERR_IO;
    }
    dev = fal_flash_device_find(part->flash_name);
    if (!dev) {
        return FDB_EDGE_ERR_IO;
    }

    partition_size = part->len;
    flash_size = dev->len;
    block_size = dev->blk_size;
    if (part->offset < 0 || partition_size > UINT32_MAX ||
        block_size < 256U || block_size > UINT32_MAX ||
        (block_size & (block_size - 1U)) != 0U) {
        return FDB_EDGE_ERR_INVALID;
    }
    offset = (uint64_t)part->offset;
    if (offset > flash_size || partition_size > flash_size - offset ||
        offset % block_size != 0U || partition_size % block_size != 0U ||
        partition_size / block_size < 2U ||
        partition_size / block_size > FDB_EDGE_MAX_TRACKED_SECTORS) {
        return FDB_EDGE_ERR_INVALID;
    }
    out_ctx->part = part;
    out_ops->ctx = out_ctx;
    out_ops->total_size = (uint32_t)partition_size;
    out_ops->sector_size = (uint32_t)block_size;
    out_ops->write_gran = write_gran_bytes;
    out_ops->read = stm32_fal_read;
    out_ops->write = stm32_fal_write;
    out_ops->erase = stm32_fal_erase;
    return FDB_EDGE_OK;
}

int fdb_stm32_flush_wal_over_uart(
    fdb_edge_tsdb_t *db,
    uint32_t device_id,
    uint64_t acked_seq,
    uint8_t *tx_ring_buf,
    size_t tx_ring_cap,
    stm32_uart_tx_fn tx_fn,
    void *uart_ctx)
{
    size_t stream_len = 0U;
    uint32_t frames = 0U;
    int rc;

    if (!db || !tx_ring_buf || !tx_fn) {
        return FDB_EDGE_ERR_INVALID;
    }
    rc = fdb_wal_export_after(db, device_id, acked_seq, tx_ring_buf, tx_ring_cap, &stream_len, &frames);
    if (rc != FDB_EDGE_OK && (rc != FDB_EDGE_ERR_BOUNDS || frames == 0U)) {
        return rc;
    }
    if (stream_len == 0U || frames == 0U) {
        return FDB_EDGE_OK;
    }
    return tx_fn(tx_ring_buf, stream_len, uart_ctx);
}
#else
/* Host syntax verification stub when STM32 Cube/FAL headers are absent. */
int fdb_stm32_port_stub_version(void)
{
    return 1;
}
#endif
