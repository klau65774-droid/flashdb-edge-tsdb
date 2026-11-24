#ifndef FDB_EDGE_RPI_CFG_H
#define FDB_EDGE_RPI_CFG_H

/*
 * Raspberry Pi (ARM64 / ARMv7 Linux) gateway & collector configuration.
 */
#define FDB_USING_TSDB
#define FDB_USING_TIMESTAMP_64BIT
#define FDB_USING_FILE_POSIX_MODE
#define FDB_WRITE_GRAN 8

#ifndef FDB_EDGE_MAX_CHANNELS
#define FDB_EDGE_MAX_CHANNELS       16U
#endif

#ifndef FDB_EDGE_MAX_BLOCK_SAMPLES
#define FDB_EDGE_MAX_BLOCK_SAMPLES  128U
#endif

#ifndef FDB_EDGE_MAX_WORK_BUF_BYTES
#define FDB_EDGE_MAX_WORK_BUF_BYTES 12288U
#endif

#endif /* FDB_EDGE_RPI_CFG_H */
