#ifndef FDB_EDGE_CRC32C_H
#define FDB_EDGE_CRC32C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t fdb_edge_crc32c_init(void);
uint32_t fdb_edge_crc32c_update(uint32_t crc, const void *data, size_t len);
uint32_t fdb_edge_crc32c_final(uint32_t crc);
uint32_t fdb_edge_crc32c(const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* FDB_EDGE_CRC32C_H */
