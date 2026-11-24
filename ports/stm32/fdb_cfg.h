#ifndef FDB_EDGE_STM32_CFG_H
#define FDB_EDGE_STM32_CFG_H

/*
 * STM32F405 / STM32H743 static zero-malloc profile:
 * Force-include this configuration across the library and its callers.
 * See stm32_toolchain_notes.md for measured object sizes and stack usage.
 */
#ifndef FDB_EDGE_MAX_CHANNELS
#define FDB_EDGE_MAX_CHANNELS         8U
#endif

#ifndef FDB_EDGE_MAX_BLOCK_SAMPLES
#define FDB_EDGE_MAX_BLOCK_SAMPLES    64U
#endif

#ifndef FDB_EDGE_MAX_PAYLOAD_BYTES
#define FDB_EDGE_MAX_PAYLOAD_BYTES    3584U
#endif

#ifndef FDB_EDGE_MAX_WORK_BUF_BYTES
#define FDB_EDGE_MAX_WORK_BUF_BYTES   4096U
#endif

#ifndef FDB_EDGE_MAX_TRACKED_SECTORS
#define FDB_EDGE_MAX_TRACKED_SECTORS  128U
#endif

/* Match STM32 internal flash / external SPI NOR flash granularity in bits.
 * Example: 8 for SPI NOR / STM32F4 byte program, 64 for STM32L4/G4, 256 for STM32H7. */
#ifndef FDB_WRITE_GRAN
#define FDB_WRITE_GRAN                8
#endif

#define FDB_USING_TSDB
#define FDB_USING_TIMESTAMP_64BIT
#define FDB_USING_FAL_MODE

#endif /* FDB_EDGE_STM32_CFG_H */
