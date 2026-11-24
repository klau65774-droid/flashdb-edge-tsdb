#include "fdb_edge/fdb_edge_flash_sim.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool is_valid_gran(uint32_t write_gran)
{
    return write_gran == 1U || write_gran == 4U || write_gran == 8U || write_gran == 32U;
}

static bool is_power_of_two(uint32_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

int fdb_flash_sim_init_static(
    fdb_flash_sim_t *sim,
    uint8_t *mem_buf,
    uint32_t *erase_count_buf,
    uint32_t total_size,
    uint32_t sector_size,
    uint32_t write_gran)
{
    if (!sim || !mem_buf || !erase_count_buf) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (!is_power_of_two(sector_size) || sector_size < 256U || !is_valid_gran(write_gran)) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (total_size / sector_size < 2U || (total_size % sector_size) != 0U) {
        return FDB_EDGE_ERR_INVALID;
    }

    memset(sim, 0, sizeof(*sim));
    sim->mem = mem_buf;
    sim->erase_counts = erase_count_buf;
    sim->total_size = total_size;
    sim->sector_size = sector_size;
    sim->sector_count = total_size / sector_size;
    sim->write_gran = write_gran;
    sim->owns_memory = false;
    sim->strict_alignment = (write_gran > 1U);

    memset(sim->mem, 0xFF, total_size);
    memset(sim->erase_counts, 0, (size_t)sim->sector_count * sizeof(uint32_t));
    return FDB_EDGE_OK;
}

int fdb_flash_sim_alloc_init(
    fdb_flash_sim_t *sim,
    uint32_t total_size,
    uint32_t sector_size,
    uint32_t write_gran)
{
    uint8_t *mem_buf = NULL;
    uint32_t *erase_buf = NULL;
    uint32_t sector_count;
    int rc;

    if (!sim || !is_power_of_two(sector_size) || sector_size < 256U || !is_valid_gran(write_gran)) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (total_size / sector_size < 2U || (total_size % sector_size) != 0U) {
        return FDB_EDGE_ERR_INVALID;
    }

    sector_count = total_size / sector_size;
    mem_buf = (uint8_t *)malloc(total_size);
    erase_buf = (uint32_t *)calloc(sector_count, sizeof(uint32_t));
    if (!mem_buf || !erase_buf) {
        free(mem_buf);
        free(erase_buf);
        return FDB_EDGE_ERR_IO;
    }

    rc = fdb_flash_sim_init_static(sim, mem_buf, erase_buf, total_size, sector_size, write_gran);
    if (rc != FDB_EDGE_OK) {
        free(mem_buf);
        free(erase_buf);
        return rc;
    }
    sim->owns_memory = true;
    return FDB_EDGE_OK;
}

void fdb_flash_sim_free(fdb_flash_sim_t *sim)
{
    if (!sim) {
        return;
    }
    if (sim->owns_memory) {
        free(sim->mem);
        free(sim->erase_counts);
    }
    memset(sim, 0, sizeof(*sim));
}

void fdb_flash_sim_reset_stats(fdb_flash_sim_t *sim)
{
    if (!sim) {
        return;
    }
    memset(&sim->stats, 0, sizeof(sim->stats));
    if (sim->erase_counts && sim->sector_count > 0U) {
        memset(sim->erase_counts, 0, (size_t)sim->sector_count * sizeof(uint32_t));
    }
}

void fdb_flash_sim_arm_power_cut(
    fdb_flash_sim_t *sim,
    uint32_t after_bytes,
    bool tear_partial_byte)
{
    if (!sim) {
        return;
    }
    sim->power_cut_armed = true;
    sim->power_cut_tripped = false;
    sim->tear_partial_byte = tear_partial_byte;
    sim->power_cut_bytes_left = after_bytes;
}

void fdb_flash_sim_disarm_power_cut(fdb_flash_sim_t *sim)
{
    if (!sim) {
        return;
    }
    sim->power_cut_armed = false;
    sim->power_cut_tripped = false;
    sim->tear_partial_byte = false;
    sim->power_cut_bytes_left = 0U;
}

static bool transfer_fits(const fdb_flash_sim_t *sim, uint32_t addr, size_t size)
{
    return addr <= sim->total_size && size <= sim->total_size - addr && size <= INT_MAX;
}

int fdb_flash_sim_read(void *ctx, uint32_t addr, uint8_t *buf, size_t size)
{
    fdb_flash_sim_t *sim = (fdb_flash_sim_t *)ctx;
    if (!sim || !sim->mem || !buf) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (!transfer_fits(sim, addr, size)) {
        return FDB_EDGE_ERR_BOUNDS;
    }
    if (size > 0U) {
        memcpy(buf, sim->mem + addr, size);
    }
    sim->stats.read_calls++;
    sim->stats.total_bytes_read += (uint64_t)size;
    return (int)size;
}

int fdb_flash_sim_write(void *ctx, uint32_t addr, const uint8_t *buf, size_t size)
{
    fdb_flash_sim_t *sim = (fdb_flash_sim_t *)ctx;
    size_t i;

    if (!sim || !sim->mem || (!buf && size > 0U)) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (!transfer_fits(sim, addr, size)) {
        return FDB_EDGE_ERR_BOUNDS;
    }
    if (sim->power_cut_armed && sim->power_cut_tripped) {
        return FDB_EDGE_ERR_POWER_CUT;
    }
    if (sim->strict_alignment && sim->write_gran > 1U) {
        if ((addr % sim->write_gran) != 0U || (size % (size_t)sim->write_gran) != 0U) {
            return FDB_EDGE_ERR_INVALID;
        }
    }

    sim->stats.write_calls++;
    for (i = 0; i < size; i++) {
        uint8_t cur;
        uint8_t target;

        if (sim->power_cut_armed && sim->power_cut_bytes_left == 0U) {
            if (sim->tear_partial_byte) {
                cur = sim->mem[addr + i];
                target = buf[i];
                /* Simulate brownout flipping only a subset of target 0-bits in the boundary byte. */
                sim->mem[addr + i] = (uint8_t)(cur & (uint8_t)(target | 0x55U));
            }
            sim->power_cut_tripped = true;
            sim->stats.power_cuts_triggered++;
            return FDB_EDGE_ERR_POWER_CUT;
        }

        cur = sim->mem[addr + i];
        target = buf[i];
        if ((uint8_t)(cur & target) != target) {
            sim->stats.nor_bit_violations++;
        }
        sim->mem[addr + i] = (uint8_t)(cur & target);
        sim->stats.total_bytes_written++;

        if (sim->power_cut_armed && sim->power_cut_bytes_left > 0U) {
            sim->power_cut_bytes_left--;
        }
    }
    return (int)size;
}

int fdb_flash_sim_erase(void *ctx, uint32_t addr, size_t size)
{
    fdb_flash_sim_t *sim = (fdb_flash_sim_t *)ctx;
    uint32_t first_sec;
    uint32_t sec_count;
    uint32_t s;

    if (!sim || !sim->mem || !sim->erase_counts || sim->sector_size == 0U || size == 0U) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (!transfer_fits(sim, addr, size)) {
        return FDB_EDGE_ERR_BOUNDS;
    }
    if ((addr % sim->sector_size) != 0U || (size % (size_t)sim->sector_size) != 0U) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (sim->power_cut_armed && sim->power_cut_tripped) {
        return FDB_EDGE_ERR_POWER_CUT;
    }
    if (sim->power_cut_armed) {
        if (sim->power_cut_bytes_left < (uint32_t)size) {
            sim->power_cut_tripped = true;
            sim->stats.power_cuts_triggered++;
            return FDB_EDGE_ERR_POWER_CUT;
        }
        sim->power_cut_bytes_left -= (uint32_t)size;
    }

    memset(sim->mem + addr, 0xFF, size);
    first_sec = addr / sim->sector_size;
    sec_count = (uint32_t)(size / (size_t)sim->sector_size);
    for (s = 0; s < sec_count; s++) {
        sim->erase_counts[first_sec + s]++;
        sim->stats.total_erases++;
    }
    return (int)size;
}

void fdb_flash_sim_as_ops(fdb_flash_sim_t *sim, fdb_flash_ops_t *out_ops)
{
    if (!sim || !out_ops) {
        return;
    }
    out_ops->ctx = sim;
    out_ops->total_size = sim->total_size;
    out_ops->sector_size = sim->sector_size;
    out_ops->write_gran = sim->write_gran;
    out_ops->read = fdb_flash_sim_read;
    out_ops->write = fdb_flash_sim_write;
    out_ops->erase = fdb_flash_sim_erase;
}

void fdb_flash_sim_get_stats(const fdb_flash_sim_t *sim, fdb_flash_sim_stats_t *out_stats)
{
    uint32_t i;
    uint32_t min_e = UINT32_MAX;
    uint32_t max_e = 0U;

    if (!sim || !out_stats) {
        return;
    }
    *out_stats = sim->stats;
    if (sim->erase_counts && sim->sector_count > 0U) {
        for (i = 0; i < sim->sector_count; i++) {
            if (sim->erase_counts[i] < min_e) {
                min_e = sim->erase_counts[i];
            }
            if (sim->erase_counts[i] > max_e) {
                max_e = sim->erase_counts[i];
            }
        }
        out_stats->min_sector_erases = (min_e == UINT32_MAX) ? 0U : min_e;
        out_stats->max_sector_erases = max_e;
    }
}
