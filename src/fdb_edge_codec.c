#include "fdb_edge/fdb_edge_codec.h"

#include <float.h>
#include <stdbool.h>
#include <string.h>

_Static_assert(sizeof(double) == sizeof(uint64_t) && FLT_RADIX == 2 &&
               DBL_MANT_DIG == 53 && DBL_MIN_EXP == -1021 && DBL_MAX_EXP == 1024,
               "C1 codec requires IEEE-754 binary64 double");
_Static_assert(FDB_EDGE_MAX_BLOCK_SAMPLES > 0U && FDB_EDGE_MAX_BLOCK_SAMPLES <= UINT8_MAX,
               "C1 sample count must fit in one byte");
_Static_assert(FDB_EDGE_MAX_CHANNELS > 0U && FDB_EDGE_MAX_CHANNELS <= UINT8_MAX,
               "C1 channel count must fit in one byte");

static void put_u32_le(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFFU);
    dst[1] = (uint8_t)((v >> 8) & 0xFFU);
    dst[2] = (uint8_t)((v >> 16) & 0xFFU);
    dst[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint32_t get_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0]
        | ((uint32_t)src[1] << 8)
        | ((uint32_t)src[2] << 16)
        | ((uint32_t)src[3] << 24);
}

static void put_u64_le(uint8_t *dst, uint64_t v)
{
    size_t i;
    for (i = 0; i < 8U; i++) {
        dst[i] = (uint8_t)((v >> (i * 8U)) & 0xFFULL);
    }
}

static uint64_t get_u64_le(const uint8_t *src)
{
    uint64_t v = 0U;
    size_t i;
    for (i = 0; i < 8U; i++) {
        v |= ((uint64_t)src[i]) << (i * 8U);
    }
    return v;
}

static int64_t signed_from_bits(uint64_t bits)
{
    if (bits <= (uint64_t)INT64_MAX) {
        return (int64_t)bits;
    }
    return -1 - (int64_t)(UINT64_MAX - bits);
}

/* Use unsigned bit patterns so full-span timestamp deltas wrap modulo 2^64. */
static uint64_t zigzag_enc(uint64_t bits)
{
    return (bits << 1) ^ (0ULL - (bits >> 63));
}

static uint64_t zigzag_dec(uint64_t z)
{
    return (z >> 1) ^ (0ULL - (z & 1ULL));
}

static int write_uvarint(uint8_t *buf, size_t cap, size_t *pos, uint64_t v)
{
    while (v >= 0x80ULL) {
        if (*pos >= cap) {
            return FDB_EDGE_ERR_BOUNDS;
        }
        buf[(*pos)++] = (uint8_t)((v & 0x7FULL) | 0x80ULL);
        v >>= 7;
    }
    if (*pos >= cap) {
        return FDB_EDGE_ERR_BOUNDS;
    }
    buf[(*pos)++] = (uint8_t)(v & 0x7FULL);
    return FDB_EDGE_OK;
}

static int read_uvarint(const uint8_t *buf, size_t len, size_t *pos, uint64_t *out)
{
    uint64_t result = 0U;
    uint32_t shift = 0U;

    while (*pos < len) {
        uint8_t b = buf[(*pos)++];
        if (shift >= 64U || (shift == 63U && b > 1U)) {
            return FDB_EDGE_ERR_CORRUPT;
        }
        result |= ((uint64_t)(b & 0x7FU)) << shift;
        if ((b & 0x80U) == 0U) {
            if (shift != 0U && b == 0U) {
                return FDB_EDGE_ERR_CORRUPT;
            }
            *out = result;
            return FDB_EDGE_OK;
        }
        shift += 7U;
    }
    return FDB_EDGE_ERR_CORRUPT;
}

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t byte_pos;
    uint8_t bit_pos; /* 0..7 bits used in current byte */
} bit_writer_t;

static void bw_init(bit_writer_t *bw, uint8_t *buf, size_t cap, size_t start_pos)
{
    bw->buf = buf;
    bw->cap = cap;
    bw->byte_pos = start_pos;
    bw->bit_pos = 0U;
}

static int bw_put_bit(bit_writer_t *bw, uint32_t bit)
{
    if (bw->byte_pos >= bw->cap) {
        return FDB_EDGE_ERR_BOUNDS;
    }
    if (bw->bit_pos == 0U) {
        bw->buf[bw->byte_pos] = 0U;
    }
    if (bit & 1U) {
        bw->buf[bw->byte_pos] |= (uint8_t)(1U << (7U - bw->bit_pos));
    }
    bw->bit_pos++;
    if (bw->bit_pos == 8U) {
        bw->bit_pos = 0U;
        bw->byte_pos++;
    }
    return FDB_EDGE_OK;
}

static int bw_put_bits(bit_writer_t *bw, uint64_t val, uint32_t nbits)
{
    while (nbits != 0U) {
        uint32_t free_bits = 8U - bw->bit_pos;
        uint32_t chunk = nbits < free_bits ? nbits : free_bits;
        uint32_t mask = (1U << chunk) - 1U;
        uint8_t bits;

        if (bw->byte_pos >= bw->cap) {
            return FDB_EDGE_ERR_BOUNDS;
        }
        nbits -= chunk;
        bits = (uint8_t)(((val >> nbits) & mask) << (free_bits - chunk));
        if (bw->bit_pos == 0U) {
            bw->buf[bw->byte_pos] = bits;
        } else {
            bw->buf[bw->byte_pos] |= bits;
        }
        bw->bit_pos = (uint8_t)(bw->bit_pos + chunk);
        if (bw->bit_pos == 8U) {
            bw->bit_pos = 0U;
            bw->byte_pos++;
        }
    }
    return FDB_EDGE_OK;
}

static size_t bw_finish_len(const bit_writer_t *bw)
{
    return bw->byte_pos + (bw->bit_pos > 0U ? 1U : 0U);
}

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t byte_pos;
    uint8_t bit_pos;
} bit_reader_t;

static void br_init(bit_reader_t *br, const uint8_t *buf, size_t len, size_t start_pos)
{
    br->buf = buf;
    br->len = len;
    br->byte_pos = start_pos;
    br->bit_pos = 0U;
}

static int br_get_bit(bit_reader_t *br, uint32_t *out_bit)
{
    if (br->byte_pos >= br->len) {
        return FDB_EDGE_ERR_CORRUPT;
    }
    *out_bit = (uint32_t)((br->buf[br->byte_pos] >> (7U - br->bit_pos)) & 1U);
    br->bit_pos++;
    if (br->bit_pos == 8U) {
        br->bit_pos = 0U;
        br->byte_pos++;
    }
    return FDB_EDGE_OK;
}

static int br_get_bits(bit_reader_t *br, uint32_t nbits, uint64_t *out_val)
{
    uint64_t acc = 0U;

    while (nbits != 0U) {
        uint32_t available = 8U - br->bit_pos;
        uint32_t chunk = nbits < available ? nbits : available;
        uint32_t mask = (1U << chunk) - 1U;
        uint32_t bits;

        if (br->byte_pos >= br->len) {
            return FDB_EDGE_ERR_CORRUPT;
        }
        bits = ((uint32_t)br->buf[br->byte_pos] >> (available - chunk)) & mask;
        acc = (acc << chunk) | bits;
        nbits -= chunk;
        br->bit_pos = (uint8_t)(br->bit_pos + chunk);
        if (br->bit_pos == 8U) {
            br->bit_pos = 0U;
            br->byte_pos++;
        }
    }
    *out_val = acc;
    return FDB_EDGE_OK;
}

static size_t br_bytes_consumed(const bit_reader_t *br)
{
    return br->byte_pos + (br->bit_pos > 0U ? 1U : 0U);
}

static uint32_t clz64_u(uint64_t v)
{
    uint32_t n = 0U;
    if (v == 0ULL) {
        return 64U;
    }
    while ((v & (1ULL << 63)) == 0ULL) {
        n++;
        v <<= 1;
    }
    return n;
}

static uint32_t ctz64_u(uint64_t v)
{
    uint32_t n = 0U;
    if (v == 0ULL) {
        return 64U;
    }
    while ((v & 1ULL) == 0ULL) {
        n++;
        v >>= 1;
    }
    return n;
}

int fdb_edge_codec_encode(
    const fdb_edge_sample_t *samples,
    size_t sample_count,
    uint8_t *out_buf,
    size_t out_cap,
    size_t *out_len)
{
    size_t pos = 0U;
    size_t i;
    uint8_t ch;
    uint8_t nch;
    bool has_epoch_changes = false;
    bool has_sub_nonzero = false;
    uint64_t prev_ts_u;
    uint64_t prev_delta = 0U;
    bit_writer_t bw;

    if (!samples || !out_buf || !out_len) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (sample_count == 0U || sample_count > FDB_EDGE_MAX_BLOCK_SAMPLES) {
        return FDB_EDGE_ERR_INVALID;
    }
    nch = samples[0].num_channels;
    if (nch == 0U || nch > FDB_EDGE_MAX_CHANNELS) {
        return FDB_EDGE_ERR_INVALID;
    }
    for (i = 1; i < sample_count; i++) {
        if (samples[i].num_channels != nch) {
            return FDB_EDGE_ERR_INVALID;
        }
        if (samples[i].mono_seq <= samples[i - 1U].mono_seq) {
            return FDB_EDGE_ERR_ROLLBACK;
        }
        if (samples[i].epoch != samples[0].epoch) {
            has_epoch_changes = true;
        }
        if (samples[i].logical_sub != 0U) {
            has_sub_nonzero = true;
        }
    }

    if (out_cap < 29U) {
        return FDB_EDGE_ERR_BOUNDS;
    }

    out_buf[pos++] = 'C';
    out_buf[pos++] = '1';
    out_buf[pos++] = (uint8_t)sample_count;
    out_buf[pos++] = nch;
    out_buf[pos++] = (uint8_t)((has_epoch_changes ? 1U : 0U) | (has_sub_nonzero ? 2U : 0U));

    put_u64_le(out_buf + pos, (uint64_t)samples[0].wall_ts_ms);
    pos += 8U;
    put_u64_le(out_buf + pos, samples[0].mono_seq);
    pos += 8U;
    put_u32_le(out_buf + pos, samples[0].epoch);
    pos += 4U;
    put_u32_le(out_buf + pos, samples[0].logical_sub);
    pos += 4U;

    /* Encode timestamps (first delta + subsequent delta-of-delta). */
    prev_ts_u = (uint64_t)samples[0].wall_ts_ms;
    for (i = 1; i < sample_count; i++) {
        uint64_t cur_u = (uint64_t)samples[i].wall_ts_ms;
        uint64_t delta = cur_u - prev_ts_u;
        uint64_t dod = delta - prev_delta;
        int rc = write_uvarint(out_buf, out_cap, &pos, zigzag_enc(dod));
        if (rc != FDB_EDGE_OK) {
            return rc;
        }
        prev_ts_u = cur_u;
        prev_delta = delta;
    }

    /* Encode strictly increasing monotonic sequence deltas (stored as delta - 1). */
    for (i = 1; i < sample_count; i++) {
        uint64_t delta_m1 = samples[i].mono_seq - samples[i - 1U].mono_seq - 1ULL;
        int rc = write_uvarint(out_buf, out_cap, &pos, delta_m1);
        if (rc != FDB_EDGE_OK) {
            return rc;
        }
    }

    if (has_epoch_changes) {
        for (i = 1; i < sample_count; i++) {
            int64_t diff = (int64_t)samples[i].epoch - (int64_t)samples[i - 1U].epoch;
            int rc = write_uvarint(out_buf, out_cap, &pos, zigzag_enc((uint64_t)diff));
            if (rc != FDB_EDGE_OK) {
                return rc;
            }
        }
    }

    if (has_sub_nonzero) {
        for (i = 1; i < sample_count; i++) {
            int rc = write_uvarint(out_buf, out_cap, &pos, (uint64_t)samples[i].logical_sub);
            if (rc != FDB_EDGE_OK) {
                return rc;
            }
        }
    }

    /* Store base channel double values (8B IEEE-754 LE each). */
    if (pos + (size_t)nch * 8U > out_cap) {
        return FDB_EDGE_ERR_BOUNDS;
    }
    for (ch = 0U; ch < nch; ch++) {
        uint64_t bits = 0U;
        memcpy(&bits, &samples[0].values[ch], sizeof(bits));
        put_u64_le(out_buf + pos, bits);
        pos += 8U;
    }

    /* Gorilla XOR bitstream for all remaining channel samples. */
    bw_init(&bw, out_buf, out_cap, pos);
    for (ch = 0U; ch < nch; ch++) {
        uint64_t prev_bits = 0U;
        uint32_t prev_lead = 0xFFU;
        uint32_t prev_trail = 0U;
        uint32_t prev_len = 0U;

        memcpy(&prev_bits, &samples[0].values[ch], sizeof(prev_bits));
        for (i = 1; i < sample_count; i++) {
            uint64_t cur_bits = 0U;
            uint64_t xor_val;

            memcpy(&cur_bits, &samples[i].values[ch], sizeof(cur_bits));
            xor_val = cur_bits ^ prev_bits;
            if (xor_val == 0ULL) {
                if (bw_put_bit(&bw, 0U) != FDB_EDGE_OK) {
                    return FDB_EDGE_ERR_BOUNDS;
                }
            } else {
                uint32_t lead = clz64_u(xor_val);
                uint32_t trail = ctz64_u(xor_val);
                if (prev_lead != 0xFFU && lead >= prev_lead && trail >= prev_trail) {
                    if (bw_put_bits(&bw, 2ULL, 2U) != FDB_EDGE_OK ||
                        bw_put_bits(&bw, xor_val >> prev_trail, prev_len) != FDB_EDGE_OK) {
                        return FDB_EDGE_ERR_BOUNDS;
                    }
                } else {
                    uint32_t clamped_lead = (lead > 31U) ? 31U : lead;
                    uint32_t meaning_len = 64U - clamped_lead - trail;
                    if (bw_put_bits(&bw, 3ULL, 2U) != FDB_EDGE_OK ||
                        bw_put_bits(&bw, clamped_lead, 5U) != FDB_EDGE_OK ||
                        bw_put_bits(&bw, meaning_len - 1U, 6U) != FDB_EDGE_OK ||
                        bw_put_bits(&bw, xor_val >> trail, meaning_len) != FDB_EDGE_OK) {
                        return FDB_EDGE_ERR_BOUNDS;
                    }
                    prev_lead = clamped_lead;
                    prev_trail = trail;
                    prev_len = meaning_len;
                }
            }
            prev_bits = cur_bits;
        }
    }

    *out_len = bw_finish_len(&bw);
    return FDB_EDGE_OK;
}

int fdb_edge_codec_decode(
    const uint8_t *buf,
    size_t buf_len,
    fdb_edge_sample_t *out_samples,
    size_t out_sample_cap,
    size_t *out_sample_count)
{
    size_t pos = 0U;
    size_t count;
    size_t i;
    uint8_t nch;
    uint8_t flags;
    bool has_epoch_changes;
    bool has_sub_nonzero;
    uint64_t prev_ts_u;
    uint64_t prev_delta = 0U;
    uint8_t ch;
    bit_reader_t br;

    if (!buf || !out_samples || !out_sample_count) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (buf_len < 29U) {
        return FDB_EDGE_ERR_CORRUPT;
    }
    if (buf[0] != 'C' || buf[1] != '1') {
        return FDB_EDGE_ERR_CORRUPT;
    }
    count = (size_t)buf[2];
    nch = buf[3];
    flags = buf[4];
    pos = 5U;

    if (count == 0U || count > FDB_EDGE_MAX_BLOCK_SAMPLES || nch == 0U || nch > FDB_EDGE_MAX_CHANNELS) {
        return FDB_EDGE_ERR_CORRUPT;
    }
    if (count > out_sample_cap) {
        return FDB_EDGE_ERR_BOUNDS;
    }
    if (flags > 0x03U) {
        return FDB_EDGE_ERR_CORRUPT;
    }
    has_epoch_changes = (flags & 1U) != 0U;
    has_sub_nonzero = (flags & 2U) != 0U;

    memset(out_samples, 0, count * sizeof(out_samples[0]));
    prev_ts_u = get_u64_le(buf + pos);
    out_samples[0].wall_ts_ms = signed_from_bits(prev_ts_u);
    pos += 8U;
    out_samples[0].mono_seq = get_u64_le(buf + pos);
    pos += 8U;
    out_samples[0].epoch = get_u32_le(buf + pos);
    pos += 4U;
    out_samples[0].logical_sub = get_u32_le(buf + pos);
    pos += 4U;
    out_samples[0].num_channels = nch;

    for (i = 1; i < count; i++) {
        uint64_t z = 0U;
        if (read_uvarint(buf, buf_len, &pos, &z) != FDB_EDGE_OK) {
            return FDB_EDGE_ERR_CORRUPT;
        }
        prev_delta += zigzag_dec(z);
        prev_ts_u += prev_delta;
        out_samples[i].wall_ts_ms = signed_from_bits(prev_ts_u);
        out_samples[i].num_channels = nch;
    }

    for (i = 1; i < count; i++) {
        uint64_t dm1 = 0U;
        if (read_uvarint(buf, buf_len, &pos, &dm1) != FDB_EDGE_OK) {
            return FDB_EDGE_ERR_CORRUPT;
        }
        if (dm1 >= UINT64_MAX - out_samples[i - 1U].mono_seq) {
            return FDB_EDGE_ERR_CORRUPT;
        }
        out_samples[i].mono_seq = out_samples[i - 1U].mono_seq + 1ULL + dm1;
    }

    for (i = 1; i < count; i++) {
        if (has_epoch_changes) {
            uint64_t z = 0U;
            uint32_t prev_epoch = out_samples[i - 1U].epoch;
            int64_t diff;
            if (read_uvarint(buf, buf_len, &pos, &z) != FDB_EDGE_OK) {
                return FDB_EDGE_ERR_CORRUPT;
            }
            diff = signed_from_bits(zigzag_dec(z));
            if (diff < -(int64_t)prev_epoch || diff > (int64_t)(UINT32_MAX - prev_epoch)) {
                return FDB_EDGE_ERR_CORRUPT;
            }
            out_samples[i].epoch = (uint32_t)((int64_t)prev_epoch + diff);
        } else {
            out_samples[i].epoch = out_samples[0].epoch;
        }
    }

    for (i = 1; i < count; i++) {
        if (has_sub_nonzero) {
            uint64_t sub = 0U;
            if (read_uvarint(buf, buf_len, &pos, &sub) != FDB_EDGE_OK || sub > UINT32_MAX) {
                return FDB_EDGE_ERR_CORRUPT;
            }
            out_samples[i].logical_sub = (uint32_t)sub;
        } else {
            out_samples[i].logical_sub = 0U;
        }
    }

    if (pos + (size_t)nch * 8U > buf_len) {
        return FDB_EDGE_ERR_CORRUPT;
    }
    for (ch = 0U; ch < nch; ch++) {
        uint64_t bits = get_u64_le(buf + pos);
        pos += 8U;
        memcpy(&out_samples[0].values[ch], &bits, sizeof(bits));
    }

    br_init(&br, buf, buf_len, pos);
    for (ch = 0U; ch < nch; ch++) {
        uint64_t prev_bits = 0U;
        uint32_t prev_lead = 0xFFU;
        uint32_t prev_trail = 0U;
        uint32_t prev_len = 0U;

        memcpy(&prev_bits, &out_samples[0].values[ch], sizeof(prev_bits));
        for (i = 1; i < count; i++) {
            uint32_t b0 = 0U;
            uint64_t cur_bits = prev_bits;

            if (br_get_bit(&br, &b0) != FDB_EDGE_OK) {
                return FDB_EDGE_ERR_CORRUPT;
            }
            if (b0 != 0U) {
                uint32_t b1 = 0U;
                if (br_get_bit(&br, &b1) != FDB_EDGE_OK) {
                    return FDB_EDGE_ERR_CORRUPT;
                }
                if (b1 == 0U) {
                    uint64_t raw = 0U;
                    uint64_t xor_val;
                    if (prev_lead == 0xFFU || prev_len == 0U || prev_trail >= 64U) {
                        return FDB_EDGE_ERR_CORRUPT;
                    }
                    if (br_get_bits(&br, prev_len, &raw) != FDB_EDGE_OK || raw == 0U) {
                        return FDB_EDGE_ERR_CORRUPT;
                    }
                    xor_val = raw << prev_trail;
                    cur_bits = prev_bits ^ xor_val;
                } else {
                    uint64_t lead_v = 0U;
                    uint64_t len_m1 = 0U;
                    uint64_t raw = 0U;
                    uint32_t lead_u;
                    uint32_t len_u;
                    uint32_t trail_u;

                    if (br_get_bits(&br, 5U, &lead_v) != FDB_EDGE_OK ||
                        br_get_bits(&br, 6U, &len_m1) != FDB_EDGE_OK) {
                        return FDB_EDGE_ERR_CORRUPT;
                    }
                    lead_u = (uint32_t)lead_v;
                    len_u = (uint32_t)len_m1 + 1U;
                    if (lead_u + len_u > 64U) {
                        return FDB_EDGE_ERR_CORRUPT;
                    }
                    trail_u = 64U - lead_u - len_u;
                    if (br_get_bits(&br, len_u, &raw) != FDB_EDGE_OK || raw == 0U) {
                        return FDB_EDGE_ERR_CORRUPT;
                    }
                    cur_bits = prev_bits ^ (raw << trail_u);
                    prev_lead = lead_u;
                    prev_trail = trail_u;
                    prev_len = len_u;
                }
            }
            memcpy(&out_samples[i].values[ch], &cur_bits, sizeof(cur_bits));
            prev_bits = cur_bits;
        }
    }

    if (br_bytes_consumed(&br) != buf_len) {
        return FDB_EDGE_ERR_CORRUPT;
    }
    if (br.bit_pos != 0U &&
        (br.buf[br.byte_pos] & ((1U << (8U - br.bit_pos)) - 1U)) != 0U) {
        return FDB_EDGE_ERR_CORRUPT;
    }
    *out_sample_count = count;
    return FDB_EDGE_OK;
}
