#ifndef FDB_EDGE_CONFIG_H
#define FDB_EDGE_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FDB_EDGE_VERSION_STR          "0.1.0"
#define FDB_EDGE_UPSTREAM_VERSION_STR "2.2.99"

#ifndef FDB_EDGE_MAX_CHANNELS
#define FDB_EDGE_MAX_CHANNELS         16U
#endif

#ifndef FDB_EDGE_MAX_BLOCK_SAMPLES
#define FDB_EDGE_MAX_BLOCK_SAMPLES    128U
#endif

#ifndef FDB_EDGE_MAX_PAYLOAD_BYTES
#define FDB_EDGE_MAX_PAYLOAD_BYTES    3584U
#endif

#ifndef FDB_EDGE_MAX_FRAME_BYTES
#define FDB_EDGE_MAX_FRAME_BYTES      4096U
#endif

#ifndef FDB_EDGE_MAX_WORK_BUF_BYTES
#define FDB_EDGE_MAX_WORK_BUF_BYTES   12288U
#endif

#ifndef FDB_EDGE_MAX_TRACKED_SECTORS
#define FDB_EDGE_MAX_TRACKED_SECTORS  1024U
#endif

#ifndef FDB_EDGE_DEFAULT_SECTOR_SIZE
#define FDB_EDGE_DEFAULT_SECTOR_SIZE  4096U
#endif

#ifndef FDB_EDGE_MAX_WRITE_GRAN
#define FDB_EDGE_MAX_WRITE_GRAN       32U
#endif

#ifndef FDB_EDGE_MAX_PRIVACY_BYTES
#define FDB_EDGE_MAX_PRIVACY_BYTES    32768U
#endif

typedef enum {
    FDB_EDGE_OK = 0,
    FDB_EDGE_ERR_INVALID = -1,
    FDB_EDGE_ERR_IO = -2,
    FDB_EDGE_ERR_CORRUPT = -3,
    FDB_EDGE_ERR_FULL = -4,
    FDB_EDGE_ERR_CAPACITY = -5,
    FDB_EDGE_ERR_ROLLBACK = -6,
    FDB_EDGE_ERR_BOUNDS = -7,
    FDB_EDGE_ERR_NOR_VIOLATION = -8,
    FDB_EDGE_ERR_POWER_CUT = -9
} fdb_edge_err_t;

typedef struct {
    void *ctx;
    uint32_t total_size;
    uint32_t sector_size;
    uint32_t write_gran; /* Write alignment in bytes (1, 4, 8, or 32). */
    int (*read)(void *ctx, uint32_t addr, uint8_t *buf, size_t size);
    int (*write)(void *ctx, uint32_t addr, const uint8_t *buf, size_t size);
    int (*erase)(void *ctx, uint32_t addr, size_t size);
} fdb_flash_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* FDB_EDGE_CONFIG_H */
