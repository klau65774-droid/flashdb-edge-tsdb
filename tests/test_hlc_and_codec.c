#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fdb_edge/fdb_edge_codec.h"
#include "fdb_edge/fdb_edge_crc32c.h"
#include "fdb_edge/fdb_edge_hlc.h"
#include "test_common.h"

static uint64_t xorshift(uint64_t *st)
{
    uint64_t x = *st;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *st = x;
    return x;
}

static void assert_roundtrip(const fdb_edge_sample_t *in, size_t n)
{
    static uint8_t enc[8192];
    static fdb_edge_sample_t dec[FDB_EDGE_MAX_BLOCK_SAMPLES];
    size_t enc_len = 0U;
    size_t dec_cnt = 0U;
    size_t i;
    uint8_t ch;

    CHECK(fdb_edge_codec_encode(in, n, enc, sizeof(enc), &enc_len) == FDB_EDGE_OK);
    CHECK(enc_len > 0U);
    CHECK(fdb_edge_codec_decode(enc, enc_len, dec, FDB_EDGE_MAX_BLOCK_SAMPLES, &dec_cnt) == FDB_EDGE_OK);
    CHECK(dec_cnt == n);

    for (i = 0; i < n; i++) {
        CHECK(dec[i].wall_ts_ms == in[i].wall_ts_ms);
        CHECK(dec[i].mono_seq == in[i].mono_seq);
        CHECK(dec[i].epoch == in[i].epoch);
        CHECK(dec[i].logical_sub == in[i].logical_sub);
        CHECK(dec[i].num_channels == in[i].num_channels);
        for (ch = 0U; ch < in[i].num_channels; ch++) {
            uint64_t a = 0ULL, b = 0ULL;
            memcpy(&a, &in[i].values[ch], sizeof(a));
            memcpy(&b, &dec[i].values[ch], sizeof(b));
            CHECK(a == b); /* Bit-for-bit lossless check including -0.0 / subnormals */
        }
    }

    /* Corrupted truncated frame must fail safely without UB */
    if (enc_len > 4U) {
        CHECK(fdb_edge_codec_decode(enc, enc_len - 2U, dec, FDB_EDGE_MAX_BLOCK_SAMPLES, &dec_cnt) != FDB_EDGE_OK);
    }
}

static void test_codec_timestamp_and_ieee_bits(void)
{
    static const int64_t timestamps[] = {
        INT64_MIN, 0, INT64_MAX, INT64_MIN, INT64_MAX, -1, -1, 0
    };
    static const uint64_t ieee_bits[] = {
        0ULL, 0x8000000000000000ULL, 0x0000000000000001ULL, 0x800fffffffffffffULL,
        0x7ff0000000000000ULL, 0xfff0000000000000ULL,
        0x7ff8000000000042ULL, 0xfff0000000000042ULL
    };
    fdb_edge_sample_t samples[sizeof(timestamps) / sizeof(timestamps[0])] = {0};
    size_t i;

    for (i = 0U; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        samples[i].wall_ts_ms = timestamps[i];
        samples[i].mono_seq = (i == 7U) ? UINT64_MAX : (uint64_t)i;
        samples[i].epoch = (i % 2U == 0U) ? 0U : UINT32_MAX;
        samples[i].logical_sub = (i % 2U == 0U) ? UINT32_MAX : 0U;
        samples[i].num_channels = 1U;
        memcpy(&samples[i].values[0], &ieee_bits[i], sizeof(ieee_bits[i]));
    }
    assert_roundtrip(samples, sizeof(samples) / sizeof(samples[0]));

    /* UINT64_MAX - 1 is a valid stored delta when the first sequence is zero. */
    samples[1].mono_seq = UINT64_MAX;
    assert_roundtrip(samples, 2U);
}

/* Make a two-sample, one-channel C1 fixture with a zero timestamp delta. */
static void check_invalid_deltas(const uint8_t *seq, size_t seq_len,
                                 const uint8_t *epoch, size_t epoch_len,
                                 uint32_t first_epoch)
{
    uint8_t buf[64] = {'C', '1', 2U, 1U, 0U};
    fdb_edge_sample_t decoded[2];
    size_t count = 0U;
    size_t pos = 30U;
    size_t i;

    memcpy(buf + pos, seq, seq_len);
    pos += seq_len;
    if (epoch_len != 0U) {
        buf[4] = 1U;
        for (i = 0U; i < 4U; ++i) {
            buf[21U + i] = (uint8_t)(first_epoch >> (8U * i));
        }
        memcpy(buf + pos, epoch, epoch_len);
        pos += epoch_len;
    }
    pos += 9U; /* Base binary64 zero and a single unchanged-value bit. */
    CHECK(fdb_edge_codec_decode(buf, pos, decoded, 2U, &count) == FDB_EDGE_ERR_CORRUPT);
}

static void test_codec_malformed_input(void)
{
    static const uint8_t zero[] = {0U};
    static const uint8_t plus_one[] = {2U};
    static const uint8_t minus_one[] = {1U};
    static const uint8_t overlong_zero[] = {0x80U, 0U};
    static const uint8_t max_uvarint[] = {
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x01U
    };
    uint8_t extreme_delta[sizeof(max_uvarint)];
    uint8_t buf[41] = {'C', '1', 2U, 1U, 0U};
    fdb_edge_sample_t decoded[3];
    size_t count = 0U;

    check_invalid_deltas(max_uvarint, sizeof(max_uvarint), NULL, 0U, 0U);
    check_invalid_deltas(overlong_zero, sizeof(overlong_zero), NULL, 0U, 0U);
    check_invalid_deltas(zero, sizeof(zero), plus_one, sizeof(plus_one), UINT32_MAX);
    check_invalid_deltas(zero, sizeof(zero), minus_one, sizeof(minus_one), 0U);
    check_invalid_deltas(zero, sizeof(zero), max_uvarint, sizeof(max_uvarint), 0U);

    memcpy(extreme_delta, max_uvarint, sizeof(extreme_delta));
    extreme_delta[0] = 0xfeU; /* ZigZag for INT64_MAX. */
    check_invalid_deltas(zero, sizeof(zero), extreme_delta, sizeof(extreme_delta), 1U);
    extreme_delta[9] = 0x02U; /* Bits beyond the unsigned 64-bit range. */
    check_invalid_deltas(extreme_delta, sizeof(extreme_delta), NULL, 0U, 0U);
    extreme_delta[9] = 0x80U; /* A continuation after the last representable byte. */
    check_invalid_deltas(extreme_delta, sizeof(extreme_delta), NULL, 0U, 0U);

    CHECK(fdb_edge_codec_decode(buf, 40U, decoded, 3U, &count) == FDB_EDGE_OK);
    buf[39] = 1U; /* The low seven bits are padding after an unchanged value. */
    CHECK(fdb_edge_codec_decode(buf, 40U, decoded, 3U, &count) == FDB_EDGE_ERR_CORRUPT);
    buf[39] = 0x80U; /* Reuse without a preceding XOR window. */
    CHECK(fdb_edge_codec_decode(buf, 40U, decoded, 3U, &count) == FDB_EDGE_ERR_CORRUPT);
    buf[39] = 0xc0U; /* New XOR window with a zero XOR. */
    CHECK(fdb_edge_codec_decode(buf, sizeof(buf), decoded, 3U, &count) == FDB_EDGE_ERR_CORRUPT);
    buf[39] = 0xffU; /* Leading bits + meaningful bits are greater than 64. */
    buf[40] = 0xf8U;
    CHECK(fdb_edge_codec_decode(buf, sizeof(buf), decoded, 3U, &count) == FDB_EDGE_ERR_CORRUPT);
}

static void test_codec_wire_compatibility(void)
{
    /* C1 bytes captured from the original bit-at-a-time encoder. */
    static const uint8_t legacy[] = {
        0x43U, 0x31U, 0x06U, 0x02U, 0x00U, 0x64U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x06U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xf0U, 0x3fU, 0xc1U,
        0xfcU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x0cU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U,
        0x00U, 0x80U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x60U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x08U, 0x0fU, 0xf0U, 0x00U, 0x00U, 0x00U, 0x00U, 0x60U,
        0x00U, 0x00U, 0x00U, 0x46U, 0x00U, 0x7aU, 0x00U
    };
    static const uint64_t ieee_bits[6][2] = {
        {0ULL,                  0x3ff0000000000000ULL},
        {0x8000000000000001ULL, 0x3ff0000000000001ULL},
        {0x8000000000000101ULL, 0x3ff0000000000009ULL},
        {0x8000000000000101ULL, 0xfff0000000000009ULL},
        {0x8000000000000100ULL, 0x7ff0000000000009ULL},
        {0x8000000000000180ULL, 0x7ff0000000000009ULL}
    };
    fdb_edge_sample_t samples[6] = {0};
    fdb_edge_sample_t decoded[6];
    uint8_t buf[sizeof(legacy)];
    size_t len = 0U;
    size_t count = 0U;
    size_t i, ch;

    for (i = 0U; i < 6U; ++i) {
        samples[i].wall_ts_ms = 100 + 3 * (int64_t)i;
        samples[i].mono_seq = 1U + (uint64_t)i;
        samples[i].num_channels = 2U;
        for (ch = 0U; ch < 2U; ++ch) {
            memcpy(&samples[i].values[ch], &ieee_bits[i][ch], sizeof(ieee_bits[i][ch]));
        }
    }
    CHECK(fdb_edge_codec_encode(samples, 6U, buf, sizeof(buf), &len) == FDB_EDGE_OK);
    CHECK(len == sizeof(legacy));
    CHECK(memcmp(buf, legacy, sizeof(legacy)) == 0);
    assert_roundtrip(samples, 6U);

    for (i = 0U; i < sizeof(legacy); ++i) {
        CHECK(fdb_edge_codec_decode(legacy, i, decoded, 6U, &count) == FDB_EDGE_ERR_CORRUPT);
        memset(buf, 0xa5, sizeof(buf));
        CHECK(fdb_edge_codec_encode(samples, 6U, buf, i, &len) == FDB_EDGE_ERR_BOUNDS);
        CHECK(buf[i] == 0xa5U);
    }
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        if (strcmp(argv[1], "test_codec_timestamp_and_ieee_bits") == 0) {
            test_codec_timestamp_and_ieee_bits();
        } else if (strcmp(argv[1], "test_codec_malformed_input") == 0) {
            test_codec_malformed_input();
        } else if (strcmp(argv[1], "test_codec_wire_compatibility") == 0) {
            test_codec_wire_compatibility();
        } else {
            CHECK(0);
        }
        return 0;
    }
    CHECK(argc == 1);

    /* 1. Standard Castagnoli CRC32C check vectors */
    CHECK(fdb_edge_crc32c("123456789", 9U) == 0xE3069283U);
    {
        uint8_t zeros[32] = {0};
        CHECK(fdb_edge_crc32c(zeros, sizeof(zeros)) == 0x8A9136AAU);
    }

    /* 2. HLC state machine across rollbacks + bursts + forward ticks */
    {
        fdb_edge_hlc_t hlc;
        fdb_edge_stamp_t st;
        fdb_edge_hlc_init(&hlc, 0, 0ULL, 0U);

        CHECK(fdb_edge_hlc_next(&hlc, 1000LL, &st) == FDB_EDGE_OK);
        CHECK(st.mono_seq == 1ULL && st.epoch == 0U && !st.was_rollback);
        CHECK(fdb_edge_hlc_next(&hlc, 1000LL, &st) == FDB_EDGE_OK);
        CHECK(st.mono_seq == 2ULL && st.logical_sub == 1U);
        CHECK(fdb_edge_hlc_next(&hlc, 500LL, &st) == FDB_EDGE_OK);
        CHECK(st.mono_seq == 3ULL && st.epoch == 1U && st.was_rollback);
        CHECK(fdb_edge_hlc_next(&hlc, 750LL, &st) == FDB_EDGE_OK);
        CHECK(st.mono_seq == 4ULL && st.epoch == 1U && !st.was_rollback);
        CHECK(hlc.rollback_count == 1U);
    }

    /* 3. Codec exactness tests across 1-sample, steady, multi-channel noisy, and extreme IEEE-754 patterns */
    {
        static fdb_edge_sample_t s[128];
        size_t i;
        uint8_t ch;
        uint64_t rng = 0x123456789ABCDEF0ULL;

        /* Single sample block */
        memset(&s[0], 0, sizeof(s[0]));
        s[0].wall_ts_ms = -500000LL;
        s[0].mono_seq = 1ULL;
        s[0].num_channels = 2U;
        s[0].values[0] = -0.0;
        s[0].values[1] = DBL_MIN;
        assert_roundtrip(s, 1U);

        /* 128 samples x 8 channels with rollback in middle + noisy jitter + identical repeats */
        memset(s, 0, sizeof(s));
        for (i = 0; i < 128U; i++) {
            s[i].wall_ts_ms = (i < 64U) ? (1700000000000LL + (int64_t)i * 1000LL)
                                        : (1699996400000LL + (int64_t)i * 1000LL);
            s[i].mono_seq = (uint64_t)(i + 1U);
            s[i].epoch = (i < 64U) ? 0U : 1U;
            s[i].logical_sub = (i == 10U) ? 2U : 0U;
            s[i].num_channels = 8U;
            for (ch = 0U; ch < 8U; ch++) {
                if (i > 0U && (i % 5U == 0U)) {
                    s[i].values[ch] = s[i - 1U].values[ch];
                } else {
                    uint64_t r = xorshift(&rng);
                    s[i].values[ch] = 25.0 + ((double)(int64_t)(r & 0xFFFFU) - 32768.0) / 1000.0;
                }
            }
        }
        assert_roundtrip(s, 128U);
    }

    test_codec_timestamp_and_ieee_bits();
    test_codec_malformed_input();
    test_codec_wire_compatibility();
    printf("test_hlc_and_codec: ALL PASSED\n");
    return 0;
}
