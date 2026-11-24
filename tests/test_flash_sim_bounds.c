#include "fdb_edge/fdb_edge_flash_sim.h"
#include "test_common.h"

int main(void)
{
    fdb_flash_sim_t sim = {0};
    uint8_t image[512];
    uint32_t erases[2];
    uint8_t byte = 0U;

    CHECK(fdb_flash_sim_erase(&sim, 0U, 256U) == FDB_EDGE_ERR_INVALID);
    CHECK(fdb_flash_sim_init_static(&sim, image, erases, 0U, UINT32_C(0x80000000), 1U) ==
          FDB_EDGE_ERR_INVALID);
    CHECK(fdb_flash_sim_alloc_init(&sim, 0U, UINT32_C(0x80000000), 1U) == FDB_EDGE_ERR_INVALID);
    CHECK(fdb_flash_sim_init_static(&sim, image, erases, sizeof(image), 256U, 1U) == FDB_EDGE_OK);
    CHECK(fdb_flash_sim_write(&sim, sizeof(image) - 1U, &byte, 1U) == 1);
    CHECK(fdb_flash_sim_read(&sim, sizeof(image) - 1U, &byte, 1U) == 1 && byte == 0U);
    CHECK(fdb_flash_sim_write(&sim, sizeof(image), &byte, 1U) == FDB_EDGE_ERR_BOUNDS);
    CHECK(fdb_flash_sim_read(&sim, 0U, &byte, SIZE_MAX) == FDB_EDGE_ERR_BOUNDS);
    CHECK(fdb_flash_sim_erase(&sim, sizeof(image), 256U) == FDB_EDGE_ERR_BOUNDS);
    return 0;
}
