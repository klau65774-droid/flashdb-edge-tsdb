#ifndef FDB_EDGE_STM32_H
#define FDB_EDGE_STM32_H

#include "fdb_edge/fdb_edge_tsdb.h"

#ifdef __cplusplus
extern "C" {
#endif

struct fal_partition;

typedef struct {
    const struct fal_partition *part;
} stm32_fal_ctx_t;

typedef int (*stm32_uart_tx_fn)(const uint8_t *buf, size_t len, void *uart_ctx);

int fdb_stm32_bind_fal_partition(
    const char *part_name, stm32_fal_ctx_t *out_ctx, fdb_flash_ops_t *out_ops, uint32_t write_gran_bytes);

/* Sends the complete frames that fit. Continue from the peer's ACK cursor. */
int fdb_stm32_flush_wal_over_uart(
    fdb_edge_tsdb_t *db, uint32_t device_id, uint64_t acked_seq,
    uint8_t *tx_ring_buf, size_t tx_ring_cap, stm32_uart_tx_fn tx_fn, void *uart_ctx);

#ifdef __cplusplus
}
#endif

#endif /* FDB_EDGE_STM32_H */
