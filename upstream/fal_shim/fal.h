#ifndef _FAL_H_
#define _FAL_H_

#include <stddef.h>
#include <stdint.h>

#include "fal_cfg.h"
#include "fdb_edge/fdb_edge_flash_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

struct fal_flash_dev {
    char name[FAL_DEV_NAME_MAX];
    uint32_t addr;
    size_t len;
    size_t blk_size;
    struct {
        int (*init)(void);
        int (*read)(long offset, uint8_t *buf, size_t size);
        int (*write)(long offset, const uint8_t *buf, size_t size);
        int (*erase)(long offset, size_t size);
    } ops;
    size_t write_gran;
};

struct fal_partition {
    uint32_t magic_word;
    char name[FAL_DEV_NAME_MAX];
    char flash_name[FAL_DEV_NAME_MAX];
    long offset;
    size_t len;
    uint32_t reserved;
};

int fal_init(void);
const struct fal_flash_dev *fal_flash_device_find(const char *name);
const struct fal_partition *fal_partition_find(const char *name);
int fal_partition_read(const struct fal_partition *part, uint32_t addr, uint8_t *buf, size_t size);
int fal_partition_write(const struct fal_partition *part, uint32_t addr, const uint8_t *buf, size_t size);
int fal_partition_erase(const struct fal_partition *part, uint32_t addr, size_t size);

void fdb_upstream_bind_sim(fdb_flash_sim_t *sim);

#ifdef __cplusplus
}
#endif

#endif /* _FAL_H_ */
