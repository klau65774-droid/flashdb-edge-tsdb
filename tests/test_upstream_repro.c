#include <stdio.h>
#include <string.h>

#include <flashdb.h>
#include <fal.h>

#include "fdb_edge/fdb_edge_flash_sim.h"
#include "fdb_edge/fdb_edge_tsdb.h"
#include "test_common.h"

static fdb_time_t g_seq_t = 1700000000LL;
static fdb_time_t dummy_time(void) { return ++g_seq_t; }

static bool count_edge_cb(const fdb_edge_sample_t *s, void *arg)
{
    size_t *cnt = (size_t *)arg;
    (void)s;
    (*cnt)++;
    return true;
}

static bool count_up_cb(fdb_tsl_t tsl, void *arg)
{
    size_t *cnt = (size_t *)arg;
    (void)tsl;
    (*cnt)++;
    return false;
}

int main(void)
{
    /* 1. Verify upstream Issue #16 clock-rollback drop vs fdb_edge HLC preservation */
    fdb_flash_sim_t sim_up, sim_edge;
    fdb_flash_ops_t ops_edge;
    struct fdb_tsdb up_db;
    static fdb_edge_tsdb_t edge_db;
    fdb_edge_tsdb_config_t cfg;
    struct fdb_blob blob;
    int i;
    int up_ok = 0, up_drop = 0;
    size_t edge_queried = 0U;

    CHECK(fdb_flash_sim_alloc_init(&sim_up, 8U * 4096U, 4096U, 1U) == FDB_EDGE_OK);
    memset(&up_db, 0, sizeof(up_db));
    fdb_upstream_bind_sim(&sim_up);
    CHECK(fdb_tsdb_init(&up_db, "repro_16", "tsdb_sim", dummy_time, 128U, NULL) == FDB_NO_ERR);

    for (i = 0; i < 6; i++) {
        double val = 10.0 + i;
        fdb_blob_make(&blob, &val, sizeof(val));
        CHECK(fdb_tsl_append_with_ts(&up_db, &blob, 1700000000LL + i * 10LL) == FDB_NO_ERR);
        up_ok++;
    }
    /* Same timestamp + 1-hour backward step */
    {
        double val = 99.0;
        fdb_blob_make(&blob, &val, sizeof(val));
        if (fdb_tsl_append_with_ts(&up_db, &blob, 1700000050LL) != FDB_NO_ERR) up_drop++;
        if (fdb_tsl_append_with_ts(&up_db, &blob, 1700000050LL - 3600LL) != FDB_NO_ERR) up_drop++;
    }
    CHECK(up_ok == 6);
    CHECK(up_drop == 2); /* Upstream drops both! */
    fdb_tsdb_deinit(&up_db);
    fdb_upstream_bind_sim(NULL);
    fdb_flash_sim_free(&sim_up);

    CHECK(fdb_flash_sim_alloc_init(&sim_edge, 8U * 4096U, 4096U, 1U) == FDB_EDGE_OK);
    fdb_flash_sim_as_ops(&sim_edge, &ops_edge);
    fdb_edge_tsdb_default_config(&cfg, 1U);
    cfg.batch_size = 4U;
    CHECK(fdb_edge_tsdb_open(&edge_db, &ops_edge, &cfg) == FDB_EDGE_OK);

    for (i = 0; i < 6; i++) {
        double val = 10.0 + i;
        CHECK(fdb_edge_tsdb_append(&edge_db, 1700000000000LL + (int64_t)i * 10000LL, &val, 1U, NULL) == FDB_EDGE_OK);
    }
    {
        double v_same = 77.0, v_back = 88.0, v_next = 89.0;
        fdb_edge_stamp_t st;
        CHECK(fdb_edge_tsdb_append(&edge_db, 1700000050000LL, &v_same, 1U, &st) == FDB_EDGE_OK);
        CHECK(st.logical_sub == 1U);
        CHECK(!st.was_rollback);
        CHECK(fdb_edge_tsdb_append(&edge_db, 1700000050000LL - 3600000LL, &v_back, 1U, &st) == FDB_EDGE_OK);
        CHECK(st.was_rollback);
        CHECK(st.epoch == 1U);
        CHECK(fdb_edge_tsdb_append(&edge_db, 1700000050000LL - 3599000LL, &v_next, 1U, &st) == FDB_EDGE_OK);
        CHECK(!st.was_rollback);
        CHECK(st.epoch == 1U);
    }
    CHECK(fdb_edge_tsdb_flush(&edge_db) == FDB_EDGE_OK);
    CHECK(fdb_edge_tsdb_query_range_mono(&edge_db, 1ULL, UINT64_MAX, count_edge_cb, &edge_queried) == FDB_EDGE_OK);
    CHECK(edge_queried == 9U);
    fdb_flash_sim_free(&sim_edge);

    /* 2. Verify Issue #249: Upstream remain underflow on max_len=4050 vs 4096 sector; Edge rejects oversized, passes 10K/16K */
    {
        fdb_flash_sim_t s_up_cap, s_edge_oversize, s_edge_16k;
        fdb_flash_ops_t o_over, o_16k;
        static uint8_t big_payload[4050];
        size_t up_cnt = 0U, q16 = 0U;

        CHECK(fdb_flash_sim_alloc_init(&s_up_cap, 4U * 4096U, 4096U, 1U) == FDB_EDGE_OK);
        memset(&up_db, 0, sizeof(up_db));
        fdb_upstream_bind_sim(&s_up_cap);
        CHECK(fdb_tsdb_init(&up_db, "cap_repro", "tsdb_sim", dummy_time, 4050U, NULL) == FDB_NO_ERR);
        memset(big_payload, 0xCD, sizeof(big_payload));
        fdb_blob_make(&blob, big_payload, sizeof(big_payload));
        CHECK(fdb_tsl_append_with_ts(&up_db, &blob, 1700001000LL) == FDB_NO_ERR);
        /* Verify upstream sector->remain unsigned wraparound (> 4096!) */
        CHECK(up_db.cur_sec.remain > 4096U);
        fdb_tsl_iter(&up_db, count_up_cb, &up_cnt);
        fdb_tsdb_deinit(&up_db);
        fdb_upstream_bind_sim(NULL);
        fdb_flash_sim_free(&s_up_cap);

        CHECK(fdb_flash_sim_alloc_init(&s_edge_oversize, 4U * 4096U, 4096U, 1U) == FDB_EDGE_OK);
        fdb_flash_sim_as_ops(&s_edge_oversize, &o_over);
        fdb_edge_tsdb_default_config(&cfg, 1U);
        cfg.max_block_bytes = 4050U;
        CHECK(fdb_edge_tsdb_open(&edge_db, &o_over, &cfg) == FDB_EDGE_ERR_CAPACITY);
        fdb_flash_sim_free(&s_edge_oversize);

        CHECK(fdb_flash_sim_alloc_init(&s_edge_16k, 4U * 16384U, 16384U, 1U) == FDB_EDGE_OK);
        fdb_flash_sim_as_ops(&s_edge_16k, &o_16k);
        fdb_edge_tsdb_default_config(&cfg, 1U);
        cfg.max_block_bytes = 10240U;
        cfg.batch_size = 64U;
        CHECK(fdb_edge_tsdb_open(&edge_db, &o_16k, &cfg) == FDB_EDGE_OK);
        for (i = 0; i < 120; i++) {
            double v = 3.14159 + (double)i * 0.1;
            CHECK(fdb_edge_tsdb_append(&edge_db, 1700000000000LL + (int64_t)i * 1000LL, &v, 1U, NULL) == FDB_EDGE_OK);
        }
        CHECK(fdb_edge_tsdb_flush(&edge_db) == FDB_EDGE_OK);
        CHECK(fdb_edge_tsdb_query_range_mono(&edge_db, 1ULL, UINT64_MAX, count_edge_cb, &q16) == FDB_EDGE_OK);
        CHECK(q16 == 120U);
        fdb_flash_sim_free(&s_edge_16k);
    }

    printf("test_upstream_repro: ALL PASSED\n");
    return 0;
}
