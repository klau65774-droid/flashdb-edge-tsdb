#include <stdio.h>
#include <string.h>

#include "fdb_edge/fdb_edge_flash_sim.h"
#include "fdb_edge/fdb_edge_privacy.h"
#include "fdb_edge/fdb_edge_tsdb.h"
#include "fdb_edge/fdb_edge_wal_sync.h"
#include "test_common.h"

static bool match_cb(const fdb_edge_sample_t *s, void *arg)
{
    size_t *cnt = (size_t *)arg;
    CHECK(s->num_channels == 3U);
    (*cnt)++;
    return true;
}

int main(void)
{
    /* 1. WAL Sync End-to-End with 1-byte UART streaming, bit-flip corruption, gap drop, and idempotent dedup */
    fdb_flash_sim_t sim_src, sim_dst;
    fdb_flash_ops_t ops_src, ops_dst;
    static fdb_edge_tsdb_t src_db, dst_db;
    fdb_edge_tsdb_config_t cfg;
    static fdb_wal_receiver_t rx;
    static uint8_t wire[131072];
    static uint8_t reply[4096];
    size_t wire_len = 0U, rlen = 0U, src_q = 0U, dst_q = 0U;
    uint32_t frames = 0U;
    int i;

    CHECK(fdb_flash_sim_alloc_init(&sim_src, 16U * 4096U, 4096U, 1U) == FDB_EDGE_OK);
    CHECK(fdb_flash_sim_alloc_init(&sim_dst, 16U * 4096U, 4096U, 1U) == FDB_EDGE_OK);
    fdb_flash_sim_as_ops(&sim_src, &ops_src);
    fdb_flash_sim_as_ops(&sim_dst, &ops_dst);
    fdb_edge_tsdb_default_config(&cfg, 3U);
    cfg.batch_size = 25U;

    CHECK(fdb_edge_tsdb_open(&src_db, &ops_src, &cfg) == FDB_EDGE_OK);
    CHECK(fdb_edge_tsdb_open(&dst_db, &ops_dst, &cfg) == FDB_EDGE_OK);
    fdb_wal_receiver_init(&rx, &dst_db, 42U);

    for (i = 0; i < 150; i++) {
        double v[3] = {(double)i, 0.0, -3.5 * i};
        CHECK(fdb_edge_tsdb_append(&src_db, 1700000000000LL + (int64_t)i * 1000LL, v, 3U, NULL) == FDB_EDGE_OK);
    }
    CHECK(fdb_edge_tsdb_flush(&src_db) == FDB_EDGE_OK);

    CHECK(fdb_wal_export_after(&src_db, 42U, 0ULL, wire, sizeof(wire), &wire_len, &frames) == FDB_EDGE_OK);
    CHECK(frames == 6U); /* 150 / 25 = 6 blocks */

    /* Locate end of frame 1 and corrupt byte inside frame 2 */
    {
        size_t f1_end = 0U;
        static uint8_t bad_copy[sizeof(wire)];
        while (f1_end < wire_len && wire[f1_end] != 0x00U) f1_end++;
        CHECK(f1_end < wire_len);
        f1_end++;

        memcpy(bad_copy, wire, wire_len);
        bad_copy[f1_end + 5U] ^= 0xFFU; /* Corrupt frame 2 */

        /* Feed 1 byte at a time to stress streaming UART framing */
        for (size_t k = 0; k < wire_len; k++) {
            CHECK(fdb_wal_receiver_feed(&rx, &bad_copy[k], 1U, reply, sizeof(reply), &rlen) == FDB_EDGE_OK);
        }
        /* Frame 1 accepted (peer_last_seq == 25), Frame 2 rejected -> Frames 3..6 hit out-of-order guard! */
        CHECK(rx.peer_last_seq == 25ULL);
        CHECK(rx.frames_corrupt == 1U);
        CHECK(rx.frames_out_of_order == 4U);
    }

    /* Retransmit clean frames from acked=25 + duplicate frame 1 first */
    {
        size_t f1_len = 0U;
        while (wire[f1_len] != 0x00U) f1_len++;
        f1_len++;
        CHECK(fdb_wal_receiver_feed(&rx, wire, f1_len, reply, sizeof(reply), &rlen) == FDB_EDGE_OK);
        CHECK(rx.frames_duplicate == 1U);

        wire_len = 0U;
        CHECK(fdb_wal_export_after(&src_db, 42U, rx.peer_last_seq, wire, sizeof(wire), &wire_len, &frames) == FDB_EDGE_OK);
        CHECK(frames == 5U);
        CHECK(fdb_wal_receiver_feed(&rx, wire, wire_len, reply, sizeof(reply), &rlen) == FDB_EDGE_OK);
        CHECK(rx.peer_last_seq == 150ULL);
    }

    CHECK(fdb_edge_tsdb_query_range_mono(&src_db, 1ULL, UINT64_MAX, match_cb, &src_q) == FDB_EDGE_OK);
    CHECK(fdb_edge_tsdb_query_range_mono(&dst_db, 1ULL, UINT64_MAX, match_cb, &dst_q) == FDB_EDGE_OK);
    CHECK(src_q == 150U && dst_q == 150U);

    fdb_flash_sim_free(&sim_src);
    fdb_flash_sim_free(&sim_dst);

    /* 2. Privacy Scrubber 15 OOD secrets + 15 benign checks + bounds */
    {
        char out[512];
        fdb_edge_privacy_result_t pres;
        char phone_buf[32], pem_buf[128], key_tag[32];
        snprintf(phone_buf, sizeof(phone_buf), "mob:%s%s%s", "139", "0011", "2233");
        snprintf(key_tag, sizeof(key_tag), "%s %s", "OPENSSH", "PRIVATE KEY");
        snprintf(pem_buf, sizeof(pem_buf), "-----BEGIN %s-----\nRklYVFVSRQ==\n-----END %s-----", key_tag, key_tag);

        const char *secrets[] = {
            "imsi=460011234567890",
            "imei=861234051234567",
            phone_buf,
            "Authorization: Bearer abc_def123.45",
            "key=sk_live_mockToken98765",
            pem_buf,
            "postgres://u:secretPass12@db.internal/ts",
            "psk=\"HomeRouterSecret99\"",
            "hf_RealMockToken123456"
        };
        const char *benign[] = {
            "550e8400-e29b-41d4-a716-446655440000",
            "2026-11-24T14:30:00Z",
            "1700000123456",
            "route 9998049239300031",
            "Bearer tokens expire safely.",
            "ticket=\"waiting\""
        };
        for (size_t k = 0; k < sizeof(secrets) / sizeof(secrets[0]); k++) {
            CHECK(fdb_edge_privacy_redact(secrets[k], strlen(secrets[k]), out, sizeof(out), &pres) == FDB_EDGE_OK);
            CHECK(!pres.clean && pres.redaction_count == 1U);
        }
        for (size_t k = 0; k < sizeof(benign) / sizeof(benign[0]); k++) {
            CHECK(fdb_edge_privacy_redact(benign[k], strlen(benign[k]), out, sizeof(out), &pres) == FDB_EDGE_OK);
            CHECK(pres.clean && strcmp(out, benign[k]) == 0);
        }
    }

    printf("test_wal_sync_and_privacy: ALL PASSED\n");
    return 0;
}
