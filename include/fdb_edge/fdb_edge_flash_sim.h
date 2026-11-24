#ifndef FDB_EDGE_FLASH_SIM_H
#define FDB_EDGE_FLASH_SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fdb_edge/fdb_edge_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t total_bytes_written;
    uint64_t total_bytes_read;
    uint64_t write_calls;
    uint64_t read_calls;
    uint64_t total_erases;
    uint64_t nor_bit_violations;
    uint64_t power_cuts_triggered;
    uint32_t min_sector_erases;
    uint32_t max_sector_erases;
} fdb_flash_sim_stats_t;

typedef struct {
    uint8_t *mem;
    uint32_t *erase_counts;
    uint32_t total_size;
    uint32_t sector_size;
    uint32_t sector_count;
    uint32_t write_gran; /* Bytes per flash program word: 1, 4, 8, or 32 */
    bool owns_memory;
    bool strict_alignment;
    bool power_cut_armed;
    bool power_cut_tripped;
    bool tear_partial_byte;
    uint32_t power_cut_bytes_left;
    fdb_flash_sim_stats_t stats;
} fdb_flash_sim_t;

int fdb_flash_sim_init_static(
    fdb_flash_sim_t *sim,
    uint8_t *mem_buf,
    uint32_t *erase_count_buf,
    uint32_t total_size,
    uint32_t sector_size,
    uint32_t write_gran);

int fdb_flash_sim_alloc_init(
    fdb_flash_sim_t *sim,
    uint32_t total_size,
    uint32_t sector_size,
    uint32_t write_gran);

void fdb_flash_sim_free(fdb_flash_sim_t *sim);

void fdb_flash_sim_reset_stats(fdb_flash_sim_t *sim);

void fdb_flash_sim_arm_power_cut(
    fdb_flash_sim_t *sim,
    uint32_t after_bytes,
    bool tear_partial_byte);

void fdb_flash_sim_disarm_power_cut(fdb_flash_sim_t *sim);

int fdb_flash_sim_read(void *ctx, uint32_t addr, uint8_t *buf, size_t size);
int fdb_flash_sim_write(void *ctx, uint32_t addr, const uint8_t *buf, size_t size);
int fdb_flash_sim_erase(void *ctx, uint32_t addr, size_t size);

void fdb_flash_sim_as_ops(fdb_flash_sim_t *sim, fdb_flash_ops_t *out_ops);

void fdb_flash_sim_get_stats(const fdb_flash_sim_t *sim, fdb_flash_sim_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* FDB_EDGE_FLASH_SIM_H */
