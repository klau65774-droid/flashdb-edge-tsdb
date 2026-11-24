#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fdb_edge/fdb_edge_flash_sim.h"
#include "fdb_edge/fdb_edge_tsdb.h"
#include "test_common.h"

typedef struct {
    size_t count;
    uint64_t last_seq;
    bool seq_error;
} collect_ctx_t;

static bool collect_cb(const fdb_edge_sample_t *s, void *arg)
{
    collect_ctx_t *ctx = (collect_ctx_t *)arg;
    if (s->mono_seq <= ctx->last_seq) {
        ctx->seq_error = true;
    }
    ctx->last_seq = s->mono_seq;
    ctx->count++;
    return true;
}

static uint64_t prng(uint64_t *st)
{
    uint64_t x = *st;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *st = x;
    return x;
}

static void append_samples(fdb_edge_tsdb_t *db, unsigned count)
{
    unsigned i;
    for (i = 0U; i < count; i++) {
        double value = 1.0;
        CHECK(fdb_edge_tsdb_append(db, (int64_t)(db->hlc.monotonic_seq + 1ULL), &value, 1U, NULL) == FDB_EDGE_OK);
    }
}

static void check_samples(fdb_edge_tsdb_t *db, size_t count, uint64_t last)
{
    collect_ctx_t ctx = {0};
    CHECK(fdb_edge_tsdb_query_range_mono(db, 1ULL, UINT64_MAX, collect_cb, &ctx) == FDB_EDGE_OK);
    CHECK(ctx.count == count);
    CHECK(ctx.last_seq == last);
    CHECK(!ctx.seq_error);
}

typedef struct {
    fdb_flash_ops_t underlying;
    bool fail_cut_read_once;
    bool partial_erase_once;
    bool preserve_header;
} fault_flash_t;

static int fault_read(void *ctx, uint32_t addr, uint8_t *data, size_t size)
{
    fault_flash_t *fault = ctx;
    fdb_flash_sim_t *sim = fault->underlying.ctx;
    if (fault->fail_cut_read_once && sim->power_cut_tripped) {
        fault->fail_cut_read_once = false;
        return FDB_EDGE_ERR_IO;
    }
    return fault->underlying.read(fault->underlying.ctx, addr, data, size);
}

static int fault_write(void *ctx, uint32_t addr, const uint8_t *data, size_t size)
{
    fault_flash_t *fault = ctx;
    return fault->underlying.write(fault->underlying.ctx, addr, data, size);
}

static int fault_erase(void *ctx, uint32_t addr, size_t size)
{
    fault_flash_t *fault = ctx;
    if (fault->partial_erase_once) {
        fdb_flash_sim_t *sim = fault->underlying.ctx;
        fault->partial_erase_once = false;
        if (fault->preserve_header) {
            fault->preserve_header = false;
            memset(sim->mem + addr + FDB_EDGE_SECTOR_HDR_BYTES, 0xFF, size - FDB_EDGE_SECTOR_HDR_BYTES);
        } else {
            memset(sim->mem + addr, 0xFF, FDB_EDGE_SECTOR_HDR_BYTES);
        }
        return FDB_EDGE_ERR_IO;
    }
    return fault->underlying.erase(fault->underlying.ctx, addr, size);
}

static void bind_fault_flash(fdb_flash_sim_t *sim, fault_flash_t *fault, fdb_flash_ops_t *ops)
{
    memset(fault, 0, sizeof(*fault));
    fdb_flash_sim_as_ops(sim, &fault->underlying);
    *ops = fault->underlying;
    ops->ctx = fault;
    ops->read = fault_read;
    ops->write = fault_write;
    ops->erase = fault_erase;
}

static void test_split_flush_retry(void)
{
    static fdb_edge_tsdb_t db;
    const uint32_t cut_offsets[] = {32U + 64U + 64U + 32U + 64U + 32U + 64U + 1U,
                                   32U + 64U + 64U + 8U, 32U + 64U + 64U + 8U};
    size_t scenario;

    for (scenario = 0U; scenario < sizeof(cut_offsets) / sizeof(cut_offsets[0]); scenario++) {
        fdb_flash_sim_t sim;
        fault_flash_t fault;
        fdb_flash_ops_t ops;
        fdb_edge_tsdb_config_t cfg;
        fdb_flash_sim_stats_t stats;
        double value = 1.0;

        CHECK(fdb_flash_sim_alloc_init(&sim, 4U * 256U, 256U, 32U) == FDB_EDGE_OK);
        bind_fault_flash(&sim, &fault, &ops);
        fdb_edge_tsdb_default_config(&cfg, 1U);
        cfg.batch_size = 4U;
        cfg.max_block_bytes = 40U;
        CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);
        append_samples(&db, 3U);
        fault.fail_cut_read_once = scenario == 2U;
        fdb_flash_sim_arm_power_cut(&sim, cut_offsets[scenario], true);
        CHECK(fdb_edge_tsdb_append(&db, 4, &value, 1U, NULL) == FDB_EDGE_ERR_IO);
        CHECK(db.commit_readback_pending == (scenario == 2U));
        fdb_flash_sim_disarm_power_cut(&sim);
        check_samples(&db, 4U, 4ULL);
        CHECK(db.flushed_samples == 2ULL);
        CHECK(db.stage_len == 2U && db.stage[0].mono_seq == 3ULL && db.stage[1].mono_seq == 4ULL);
        CHECK(fdb_edge_tsdb_flush(&db) == FDB_EDGE_OK);
        CHECK(db.flushed_samples == 4ULL && db.stage_len == 0U);
        CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);
        check_samples(&db, 4U, 4ULL);
        fdb_flash_sim_get_stats(&sim, &stats);
        CHECK(stats.nor_bit_violations == 0ULL);
        fdb_flash_sim_free(&sim);
    }
}

static void test_split_flush_full(void)
{
    static fdb_edge_tsdb_t db;
    fdb_flash_sim_t sim;
    fdb_flash_ops_t ops;
    fdb_edge_tsdb_config_t cfg;
    double value = 1.0;
    unsigned i;

    CHECK(fdb_flash_sim_alloc_init(&sim, 2U * 256U, 256U, 32U) == FDB_EDGE_OK);
    fdb_flash_sim_as_ops(&sim, &ops);
    fdb_edge_tsdb_default_config(&cfg, 1U);
    cfg.batch_size = 4U;
    cfg.max_block_bytes = 40U;
    cfg.rollover = false;
    CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);
    append_samples(&db, 1U);
    CHECK(fdb_edge_tsdb_flush(&db) == FDB_EDGE_OK);
    append_samples(&db, 3U);
    CHECK(fdb_edge_tsdb_append(&db, 5, &value, 1U, NULL) == FDB_EDGE_ERR_FULL);
    CHECK(db.flushed_samples == 3ULL);
    CHECK(db.stage_len == 2U && db.stage[0].mono_seq == 4ULL && db.stage[1].mono_seq == 5ULL);
    check_samples(&db, 5U, 5ULL);
    CHECK(fdb_edge_tsdb_flush(&db) == FDB_EDGE_ERR_FULL);
    append_samples(&db, 1U);
    CHECK(fdb_edge_tsdb_append(&db, 7, &value, 1U, NULL) == FDB_EDGE_ERR_FULL);
    for (i = 0U; i < 5U; i++) {
        fdb_edge_stamp_t unaccepted = {0};
        unaccepted.mono_seq = 99ULL;
        CHECK(fdb_edge_tsdb_append(&db, 8, &value, 1U, &unaccepted) == FDB_EDGE_ERR_FULL);
        CHECK(unaccepted.mono_seq == 0ULL);
    }
    CHECK(db.stage_len == 4U && db.hlc.monotonic_seq == 7ULL);
    check_samples(&db, 7U, 7ULL);
    fdb_flash_sim_free(&sim);
}

static void test_nor_rollover_recovery(void)
{
    static fdb_edge_tsdb_t db;
    fdb_flash_sim_t sim;
    fault_flash_t fault;
    fdb_flash_ops_t ops;
    fdb_edge_tsdb_config_t cfg;
    fdb_flash_sim_stats_t stats;
    uint32_t victim;
    uint32_t torn_seal;
    uint32_t torn_tail;
    double value = 1.0;

    CHECK(fdb_flash_sim_alloc_init(&sim, 6U * 256U, 256U, 32U) == FDB_EDGE_OK);
    bind_fault_flash(&sim, &fault, &ops);
    fdb_edge_tsdb_default_config(&cfg, 1U);
    cfg.batch_size = 1U;
    CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);
    append_samples(&db, 18U);
    victim = ((uint32_t)db.cur_sec_idx + 1U) % db.sector_count;
    CHECK(db.sectors[victim].erase_count == 2U);
    fdb_flash_sim_arm_power_cut(&sim, 64U + 256U + 3U, true);
    CHECK(fdb_edge_tsdb_append(&db, 19, &value, 1U, NULL) == FDB_EDGE_ERR_IO);
    fdb_flash_sim_disarm_power_cut(&sim);
    CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);
    CHECK(db.sectors[victim].state == FDB_EDGE_SEC_TORN_HDR);
    CHECK(db.sectors[victim].erase_count == sim.erase_counts[victim]);
    check_samples(&db, 5U, 18ULL);
    append_samples(&db, 1U);
    CHECK(db.sectors[victim].erase_count == sim.erase_counts[victim]);

    torn_seal = (uint32_t)db.cur_sec_idx;
    fdb_flash_sim_arm_power_cut(&sim, 4U, true);
    CHECK(fdb_edge_tsdb_append(&db, 20, &value, 1U, NULL) == FDB_EDGE_ERR_IO);
    fdb_flash_sim_disarm_power_cut(&sim);
    CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);
    CHECK(db.sectors[torn_seal].state == FDB_EDGE_SEC_TORN_SEAL);

    fdb_flash_sim_arm_power_cut(&sim, 256U + 32U + 64U + 3U, true);
    CHECK(fdb_edge_tsdb_append(&db, 20, &value, 1U, NULL) == FDB_EDGE_ERR_IO);
    torn_tail = (uint32_t)db.cur_sec_idx;
    fdb_flash_sim_disarm_power_cut(&sim);
    CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);
    CHECK(db.sectors[torn_tail].has_torn_tail);
    CHECK(db.sectors[torn_seal].state == FDB_EDGE_SEC_TORN_SEAL);
    check_samples(&db, 5U, 19ULL);

    fault.partial_erase_once = true;
    fault.preserve_header = true;
    CHECK(fdb_edge_tsdb_append(&db, 20, &value, 1U, NULL) == FDB_EDGE_ERR_IO);
    CHECK(db.sectors[torn_tail].has_torn_tail);
    fdb_flash_sim_arm_power_cut(&sim, 256U + 3U, true);
    CHECK(fdb_edge_tsdb_flush(&db) == FDB_EDGE_ERR_IO);
    fdb_flash_sim_disarm_power_cut(&sim);
    CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);
    CHECK(db.sectors[torn_tail].state == FDB_EDGE_SEC_TORN_HDR);
    check_samples(&db, 5U, 19ULL);
    append_samples(&db, 1U);
    CHECK((uint32_t)db.cur_sec_idx == torn_tail);

    fault.partial_erase_once = true;
    CHECK(fdb_edge_tsdb_append(&db, 21, &value, 1U, NULL) == FDB_EDGE_ERR_IO);
    check_samples(&db, 6U, 21ULL);
    CHECK(fdb_edge_tsdb_flush(&db) == FDB_EDGE_OK);
    CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);
    check_samples(&db, 6U, 21ULL);
    fdb_flash_sim_get_stats(&sim, &stats);
    CHECK(stats.nor_bit_violations == 0ULL);
    fdb_flash_sim_free(&sim);
}

static void test_raw_hlc_recovery(void)
{
    static fdb_edge_tsdb_t db;
    fdb_flash_sim_t sim;
    fdb_flash_ops_t ops;
    fdb_edge_tsdb_config_t cfg;
    fdb_edge_tsdb_stats_t stats;
    fdb_edge_sample_t samples[3] = {0};
    fdb_edge_stamp_t stamp;
    uint8_t payload[256];
    size_t len = 0U;
    double value = 1.0;
    size_t i;

    CHECK(fdb_flash_sim_alloc_init(&sim, 3U * 512U, 512U, 8U) == FDB_EDGE_OK);
    fdb_flash_sim_as_ops(&sim, &ops);
    fdb_edge_tsdb_default_config(&cfg, 1U);
    CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);
    CHECK(fdb_edge_tsdb_append(&db, 6000, &value, 1U, NULL) == FDB_EDGE_OK);
    for (i = 0U; i < 3U; i++) {
        samples[i].mono_seq = 2ULL + (uint64_t)i;
        samples[i].wall_ts_ms = i == 0U ? 7000 : -1000;
        samples[i].epoch = i == 0U ? 0U : 1U;
        samples[i].logical_sub = i == 2U ? 1U : 0U;
        samples[i].num_channels = 1U;
        samples[i].values[0] = value;
    }
    CHECK(fdb_edge_codec_encode(samples, 3U, payload, sizeof(payload), &len) == FDB_EDGE_OK);
    CHECK(fdb_edge_tsdb_append_raw_block_range(&db, payload, len, 2ULL, 3ULL) == FDB_EDGE_ERR_CORRUPT);
    CHECK(db.stage_len == 1U && db.flushed_samples == 0ULL);
    CHECK(fdb_edge_tsdb_append_raw_block_range(&db, payload, len, 2ULL, 4ULL) == FDB_EDGE_OK);
    CHECK(fdb_edge_tsdb_append_raw_block(&db, payload, len) == FDB_EDGE_ERR_ROLLBACK);
    CHECK(db.hlc.logical_sub == 1U);
    fdb_edge_tsdb_get_stats(&db, &stats);
    CHECK(stats.current_epoch == 1U && stats.rollback_events == 1ULL && stats.last_wall_ms == -1000);
    check_samples(&db, 4U, 4ULL);
    CHECK(fdb_edge_tsdb_open(&db, &db.flash, &db.cfg) == FDB_EDGE_OK);
    CHECK(db.hlc.logical_sub == 1U && db.hlc.rollback_count == 1U);
    samples[0].mono_seq = 5ULL;
    samples[0].wall_ts_ms = -1100;
    samples[0].epoch = 1U;
    CHECK(fdb_edge_codec_encode(samples, 1U, payload, sizeof(payload), &len) == FDB_EDGE_OK);
    CHECK(fdb_edge_tsdb_append_raw_block(&db, payload, len) == FDB_EDGE_ERR_ROLLBACK);
    CHECK(fdb_edge_tsdb_append(&db, -1000, &value, 1U, &stamp) == FDB_EDGE_OK);
    CHECK(stamp.mono_seq == 5ULL && stamp.epoch == 1U && stamp.logical_sub == 2U && !stamp.was_rollback);
    CHECK(fdb_edge_tsdb_append(&db, -1001, &value, 1U, &stamp) == FDB_EDGE_OK);
    CHECK(stamp.mono_seq == 6ULL && stamp.epoch == 2U && stamp.logical_sub == 0U && stamp.was_rollback);
    CHECK(db.hlc.rollback_count == 2U);
    fdb_flash_sim_free(&sim);
}

static void test_flash_geometry_bounds(void)
{
    static fdb_edge_tsdb_t db;
    fdb_flash_sim_t sim;
    fdb_flash_ops_t ops;
    fdb_edge_tsdb_config_t cfg;
    fdb_flash_sim_stats_t stats;
    collect_ctx_t ctx = {0};

    CHECK(fdb_flash_sim_alloc_init(&sim, 2U * 256U, 256U, 32U) == FDB_EDGE_OK);
    fdb_flash_sim_as_ops(&sim, &ops);
    fdb_edge_tsdb_default_config(&cfg, 1U);
    ops.write_gran = 3U;
    CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_ERR_INVALID);
    ops.write_gran = 32U;
    ops.sector_size = UINT32_C(0x80000000);
    ops.total_size = ops.sector_size;
    CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_ERR_INVALID);
    fdb_flash_sim_as_ops(&sim, &ops);
    cfg.max_block_bytes = UINT32_MAX;
    CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_ERR_CAPACITY);
    cfg.max_block_bytes = 65U;
    CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_ERR_CAPACITY);
    cfg.max_block_bytes = 64U;
    cfg.batch_size = 1U;
    sim.mem[FDB_EDGE_SECTOR_HDR_BYTES + 4U] = 0U;
    CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);
    append_samples(&db, 1U);
    CHECK(db.cur_sec_idx == 1);
    append_samples(&db, 1U);
    CHECK(db.cur_sec_idx == 0);
    fdb_flash_sim_get_stats(&sim, &stats);
    CHECK(stats.nor_bit_violations == 0ULL && stats.total_erases == 1U);
    sim.mem[FDB_EDGE_SECTOR_HDR_BYTES + 4U] ^= 1U;
    CHECK(fdb_edge_tsdb_query_range_mono(&db, 2ULL, 2ULL, collect_cb, &ctx) == FDB_EDGE_ERR_CORRUPT);
    CHECK(ctx.count == 0U);
    fdb_flash_sim_free(&sim);
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        if (strcmp(argv[1], "split_flush_retry") == 0) test_split_flush_retry();
        else if (strcmp(argv[1], "split_flush_full") == 0) test_split_flush_full();
        else if (strcmp(argv[1], "nor_rollover_recovery") == 0) test_nor_rollover_recovery();
        else if (strcmp(argv[1], "raw_hlc_recovery") == 0) test_raw_hlc_recovery();
        else if (strcmp(argv[1], "flash_geometry_bounds") == 0) test_flash_geometry_bounds();
        else { fprintf(stderr, "Unknown TSDB test case: %s\n", argv[1]); return 1; }
        printf("%s: PASSED\n", argv[1]);
        return 0;
    }

    /* 1. Write granularity tests: 1B, 4B, 8B, AND 32B (STM32H7 flash-word) with 0 NOR bit violations */
    const uint32_t grans[4] = {1U, 4U, 8U, 32U};
    size_t g;
    for (g = 0; g < 4U; g++) {
        fdb_flash_sim_t sim;
        fdb_flash_ops_t ops;
        static fdb_edge_tsdb_t db;
        fdb_edge_tsdb_config_t cfg;
        fdb_flash_sim_stats_t fs;
        collect_ctx_t cctx;
        int i;

        CHECK(fdb_flash_sim_alloc_init(&sim, 8U * 4096U, 4096U, grans[g]) == FDB_EDGE_OK);
        fdb_flash_sim_as_ops(&sim, &ops);
        fdb_edge_tsdb_default_config(&cfg, 4U);
        cfg.batch_size = 32U;
        CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);

        for (i = 0; i < 600; i++) {
            double v[4] = {20.0 + i * 0.1, -5.0, sin(i * 0.05), 100.0 + (i % 7)};
            int64_t ts = (i == 300) ? (1700000000000LL - 3600000LL) : (1700000000000LL + (int64_t)i * 1000LL);
            CHECK(fdb_edge_tsdb_append(&db, ts, v, 4U, NULL) == FDB_EDGE_OK);
        }
        CHECK(fdb_edge_tsdb_flush(&db) == FDB_EDGE_OK);

        /* Cold reboot reopen + write 20 more samples into same unsealed active sector */
        memset(&db, 0, sizeof(db));
        CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);
        for (i = 0; i < 20; i++) {
            double v[4] = {1.0, 2.0, 3.0, 4.0};
            CHECK(fdb_edge_tsdb_append(&db, 1700001000000LL + (int64_t)i * 1000LL, v, 4U, NULL) == FDB_EDGE_OK);
        }
        CHECK(fdb_edge_tsdb_flush(&db) == FDB_EDGE_OK);

        memset(&cctx, 0, sizeof(cctx));
        CHECK(fdb_edge_tsdb_query_range_mono(&db, 1ULL, UINT64_MAX, collect_cb, &cctx) == FDB_EDGE_OK);
        CHECK(!cctx.seq_error);
        CHECK(cctx.count == 620U);

        fdb_flash_sim_get_stats(&sim, &fs);
        CHECK(fs.nor_bit_violations == 0ULL);
        fdb_flash_sim_free(&sim);
    }

    /* 2. High-entropy incompressible 16-channel batch split-flush test (exceeds single block -> splits cleanly) */
    {
        fdb_flash_sim_t sim;
        fdb_flash_ops_t ops;
        static fdb_edge_tsdb_t db;
        fdb_edge_tsdb_config_t cfg;
        collect_ctx_t cctx;
        uint64_t rng = 0x9988776655443322ULL;
        int i;

        CHECK(fdb_flash_sim_alloc_init(&sim, 16U * 4096U, 4096U, 8U) == FDB_EDGE_OK);
        fdb_flash_sim_as_ops(&sim, &ops);
        fdb_edge_tsdb_default_config(&cfg, 16U);
        cfg.batch_size = 128U; /* 128 * 16 * 8 = 16KB uncompressible > 4KB sector! */
        CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);

        for (i = 0; i < 128; i++) {
            double vals[16];
            int c;
            for (c = 0; c < 16; c++) {
                uint64_t r = prng(&rng);
                memcpy(&vals[c], &r, sizeof(double));
                if (!isfinite(vals[c])) vals[c] = (double)r;
            }
            CHECK(fdb_edge_tsdb_append(&db, 1700000000000LL + (int64_t)i * 100LL, vals, 16U, NULL) == FDB_EDGE_OK);
        }
        CHECK(fdb_edge_tsdb_flush(&db) == FDB_EDGE_OK);

        memset(&cctx, 0, sizeof(cctx));
        CHECK(fdb_edge_tsdb_query_range_mono(&db, 1ULL, UINT64_MAX, collect_cb, &cctx) == FDB_EDGE_OK);
        CHECK(cctx.count == 128U);
        fdb_flash_sim_free(&sim);
    }

    /* 3. Ring rollover & uniform wear leveling check (max - min sector erase <= 1) */
    {
        fdb_flash_sim_t sim;
        fdb_flash_ops_t ops;
        static fdb_edge_tsdb_t db;
        fdb_edge_tsdb_config_t cfg;
        fdb_flash_sim_stats_t fs;
        collect_ctx_t cctx;
        int i;

        CHECK(fdb_flash_sim_alloc_init(&sim, 6U * 1024U, 1024U, 4U) == FDB_EDGE_OK);
        fdb_flash_sim_as_ops(&sim, &ops);
        fdb_edge_tsdb_default_config(&cfg, 2U);
        cfg.batch_size = 16U;
        CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);

        for (i = 0; i < 2400; i++) {
            double v[2] = {(double)i, 42.5 + sin((double)i)};
            CHECK(fdb_edge_tsdb_append(&db, 1700000000000LL + (int64_t)i * 1000LL, v, 2U, NULL) == FDB_EDGE_OK);
        }
        CHECK(fdb_edge_tsdb_flush(&db) == FDB_EDGE_OK);

        fdb_flash_sim_get_stats(&sim, &fs);
        CHECK(fs.nor_bit_violations == 0ULL);
        CHECK(fs.max_sector_erases >= 3U);
        CHECK((fs.max_sector_erases - fs.min_sector_erases) <= 1U);

        /* Wall range query including uncommitted RAM sample test */
        {
            double v[2] = {999.0, 1.0};
            CHECK(fdb_edge_tsdb_append(&db, 1700009000000LL, v, 2U, NULL) == FDB_EDGE_OK);
            memset(&cctx, 0, sizeof(cctx));
            CHECK(fdb_edge_tsdb_query_range_wall(&db, 1700008999000LL, 1700009001000LL, collect_cb, &cctx) == FDB_EDGE_OK);
            CHECK(cctx.count == 1U);
        }
        fdb_flash_sim_free(&sim);
    }

    /* 4. 200 deterministic brownout torn-write power cuts */
    {
        uint64_t seed = 1337ULL;
        uint32_t t;
        for (t = 0; t < 200U; t++) {
            fdb_flash_sim_t sim;
            fdb_flash_ops_t ops;
            static fdb_edge_tsdb_t db;
            fdb_edge_tsdb_config_t cfg;
            fdb_flash_sim_stats_t fs;
            collect_ctx_t cctx;
            uint64_t pre_flushed;
            uint32_t cut_at = 2U + (uint32_t)(prng(&seed) % 90U);

            CHECK(fdb_flash_sim_alloc_init(&sim, 6U * 4096U, 4096U, (t % 2U) ? 8U : 1U) == FDB_EDGE_OK);
            fdb_flash_sim_as_ops(&sim, &ops);
            fdb_edge_tsdb_default_config(&cfg, 1U);
            cfg.batch_size = 16U;
            CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);

            for (int i = 0; i < 48; i++) {
                double v = (double)i;
                CHECK(fdb_edge_tsdb_append(&db, 1700000000000LL + (int64_t)i * 1000LL, &v, 1U, NULL) == FDB_EDGE_OK);
            }
            CHECK(fdb_edge_tsdb_flush(&db) == FDB_EDGE_OK);
            pre_flushed = db.flushed_samples;
            CHECK(pre_flushed == 48ULL);

            fdb_flash_sim_arm_power_cut(&sim, cut_at, true);
            for (int i = 0; i < 16; i++) {
                double v = 9999.0;
                if (fdb_edge_tsdb_append(&db, 1700001000000LL + (int64_t)i * 1000LL, &v, 1U, NULL) != FDB_EDGE_OK) {
                    break;
                }
            }
            fdb_flash_sim_disarm_power_cut(&sim);

            memset(&db, 0, sizeof(db));
            CHECK(fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_OK);
            memset(&cctx, 0, sizeof(cctx));
            CHECK(fdb_edge_tsdb_query_range_mono(&db, 1ULL, UINT64_MAX, collect_cb, &cctx) == FDB_EDGE_OK);
            CHECK(cctx.count >= pre_flushed);

            /* Appending after crash recovery must not violate NOR 0->1 rules */
            {
                double v = 500.0;
                CHECK(fdb_edge_tsdb_append(&db, 1700002000000LL, &v, 1U, NULL) == FDB_EDGE_OK);
                CHECK(fdb_edge_tsdb_flush(&db) == FDB_EDGE_OK);
            }
            fdb_flash_sim_get_stats(&sim, &fs);
            CHECK(fs.nor_bit_violations == 0ULL);
            fdb_flash_sim_free(&sim);
        }
    }

    printf("test_edge_tsdb_and_faults: ALL PASSED\n");
    return 0;
}
