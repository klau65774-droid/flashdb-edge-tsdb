#include "fal.h"

#include <string.h>

static fdb_flash_sim_t *g_bound_sim = NULL;
static struct fal_flash_dev g_flash_dev = {
    "nor_sim0",
    0U,
    0U,
    4096U,
    {NULL, NULL, NULL, NULL},
    8U
};
static struct fal_partition g_part = {
    0x46414C30U,
    "tsdb_sim",
    "nor_sim0",
    0L,
    0U,
    0U
};

void fdb_upstream_bind_sim(fdb_flash_sim_t *sim)
{
    g_bound_sim = sim;
    if (sim != NULL) {
        g_flash_dev.len = (size_t)sim->total_size;
        g_flash_dev.blk_size = (size_t)sim->sector_size;
        g_flash_dev.write_gran = (size_t)(sim->write_gran * 8U);
        g_part.len = (size_t)sim->total_size;
    } else {
        g_flash_dev.len = 0U;
        g_flash_dev.blk_size = 4096U;
        g_part.len = 0U;
    }
}

int fal_init(void)
{
    return (g_bound_sim != NULL) ? 1 : -1;
}

const struct fal_flash_dev *fal_flash_device_find(const char *name)
{
    if (!name || strcmp(name, g_flash_dev.name) != 0 || g_bound_sim == NULL) {
        return NULL;
    }
    return &g_flash_dev;
}

const struct fal_partition *fal_partition_find(const char *name)
{
    if (!name || strcmp(name, g_part.name) != 0 || g_bound_sim == NULL) {
        return NULL;
    }
    return &g_part;
}

int fal_partition_read(const struct fal_partition *part, uint32_t addr, uint8_t *buf, size_t size)
{
    if (!part || !g_bound_sim) {
        return -1;
    }
    return fdb_flash_sim_read(g_bound_sim, addr, buf, size);
}

int fal_partition_write(const struct fal_partition *part, uint32_t addr, const uint8_t *buf, size_t size)
{
    if (!part || !g_bound_sim) {
        return -1;
    }
    return fdb_flash_sim_write(g_bound_sim, addr, buf, size);
}

int fal_partition_erase(const struct fal_partition *part, uint32_t addr, size_t size)
{
    if (!part || !g_bound_sim) {
        return -1;
    }
    return fdb_flash_sim_erase(g_bound_sim, addr, size);
}
