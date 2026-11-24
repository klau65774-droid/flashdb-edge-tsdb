#include "fdb_edge/fdb_edge_tsdb.h"

#include <limits.h>
#include <string.h>

#include "fdb_edge/fdb_edge_crc32c.h"

#define EDGE_SEC_MAGIC  0x46444253U /* "FDBS" */
#define EDGE_SEAL_MAGIC 0x5345414CU /* "SEAL" */
#define EDGE_BLK_MAGIC  0x46444242U /* "FDBB" */
#define EDGE_CMT_MAGIC  0x434D5452U /* "CMTR" */
#define EDGE_WEAR_MAGIC 0x57454152U /* "WEAR" */
#define EDGE_HDR_VER    1U
#define EDGE_COMMIT_BUF_BYTES ((FDB_EDGE_MAX_WRITE_GRAN < 8U) ? 8U : FDB_EDGE_MAX_WRITE_GRAN)

_Static_assert(FDB_EDGE_MAX_BLOCK_SAMPLES > 0U && FDB_EDGE_MAX_BLOCK_SAMPLES <= UINT8_MAX,
    "The on-flash sample count occupies one byte");

static uint32_t align_up_u32(uint32_t v, uint32_t align)
{
    if (align <= 1U) {
        return v;
    }
    return ((v + align - 1U) / align) * align;
}

static void put_u16_le(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFFU);
    dst[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static uint16_t get_u16_le(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

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
    uint64_t v = 0ULL;
    size_t i;
    for (i = 0; i < 8U; i++) {
        v |= ((uint64_t)src[i]) << (i * 8U);
    }
    return v;
}

static int64_t get_i64_le(const uint8_t *src)
{
    uint64_t value = get_u64_le(src);
    return value <= (uint64_t)INT64_MAX ? (int64_t)value : -1 - (int64_t)(UINT64_MAX - value);
}

static bool is_all_ff(const uint8_t *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (buf[i] != 0xFFU) {
            return false;
        }
    }
    return true;
}

static uint32_t min_usable_overhead(uint32_t write_gran)
{
    uint32_t cmt = align_up_u32(8U, write_gran);
    return FDB_EDGE_SECTOR_HDR_BYTES + FDB_EDGE_BLOCK_HDR_BYTES + cmt;
}

void fdb_edge_tsdb_default_config(fdb_edge_tsdb_config_t *cfg, uint8_t num_channels)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->num_channels = num_channels;
    cfg->batch_size = FDB_EDGE_MAX_BLOCK_SAMPLES < 64U ? FDB_EDGE_MAX_BLOCK_SAMPLES : 64U;
    cfg->max_block_bytes = 0U;
    cfg->rollover = true;
    cfg->allow_torn_recovery = true;
}

static bool has_sector_header(const fdb_edge_sec_meta_t *meta)
{
    return meta->state == FDB_EDGE_SEC_ACTIVE_UNSEALED ||
        meta->state == FDB_EDGE_SEC_SEALED || meta->state == FDB_EDGE_SEC_TORN_SEAL;
}

static int32_t next_ordered_sector(const fdb_edge_tsdb_t *db, int32_t previous)
{
    uint64_t previous_seq = previous < 0 ? 0ULL : db->sectors[previous].sector_seq;
    int32_t chosen = -1;
    uint32_t i;

    /* The normal ring has consecutive sequences in adjacent physical sectors. */
    if (previous >= 0 && previous_seq < UINT64_MAX) {
        i = ((uint32_t)previous + 1U) % db->sector_count;
        if (has_sector_header(&db->sectors[i]) && db->sectors[i].sector_seq == previous_seq + 1ULL) {
            return (int32_t)i;
        }
    }

    /* Recovery may leave gaps in physical order. Find the next surviving generation. */
    for (i = 0; i < db->sector_count; i++) {
        const fdb_edge_sec_meta_t *meta = &db->sectors[i];
        if (has_sector_header(meta) && meta->sector_seq > previous_seq &&
            (chosen < 0 || meta->sector_seq < db->sectors[chosen].sector_seq)) {
            chosen = (int32_t)i;
        }
    }
    return chosen;
}

static bool valid_block_header(const fdb_edge_tsdb_t *db, const uint8_t *hdr, uint32_t remaining)
{
    uint32_t payload_len = get_u16_le(hdr + 4U);
    uint32_t padded_len = align_up_u32(payload_len, db->flash.write_gran);

    return get_u32_le(hdr) == EDGE_BLK_MAGIC &&
        fdb_edge_crc32c(hdr, 60U) == get_u32_le(hdr + 60U) &&
        payload_len > 0U && payload_len <= db->effective_max_block_bytes &&
        hdr[6] > 0U && hdr[6] <= FDB_EDGE_MAX_BLOCK_SAMPLES && hdr[7] == db->cfg.num_channels &&
        get_u64_le(hdr + 8U) > 0ULL && get_u64_le(hdr + 8U) <= get_u64_le(hdr + 16U) &&
        padded_len == get_u32_le(hdr + 56U) &&
        FDB_EDGE_BLOCK_HDR_BYTES + padded_len + db->commit_aligned_bytes <= remaining;
}

static int scan_sector_blocks(
    fdb_edge_tsdb_t *db,
    uint32_t sec_idx,
    fdb_edge_stamp_t *last)
{
    uint32_t sec_base = sec_idx * db->flash.sector_size;
    fdb_edge_sec_meta_t *meta = &db->sectors[sec_idx];
    uint32_t pos = FDB_EDGE_SECTOR_HDR_BYTES;
    uint8_t blk_hdr[FDB_EDGE_BLOCK_HDR_BYTES];
    uint8_t cmt_buf[EDGE_COMMIT_BUF_BYTES];

    meta->block_count = 0U;
    meta->sample_count = 0U;
    meta->has_torn_tail = false;
    meta->write_offset = pos;
    meta->min_mono_seq = 0ULL;
    meta->max_mono_seq = 0ULL;
    meta->min_wall_ms = 0;
    meta->max_wall_ms = 0;

    while (pos + FDB_EDGE_BLOCK_HDR_BYTES <= db->flash.sector_size) {
        uint16_t payload_len;
        uint8_t sample_cnt;
        uint64_t b_min_seq;
        uint64_t b_max_seq;
        int64_t b_min_wall;
        int64_t b_max_wall;
        uint32_t payload_crc;
        uint32_t padded_len;
        uint32_t next_pos;
        uint32_t cmt_magic;
        uint32_t cmt_crc;

        if (db->flash.read(db->flash.ctx, sec_base + pos, blk_hdr, sizeof(blk_hdr)) < 0) {
            return FDB_EDGE_ERR_IO;
        }
        if (is_all_ff(blk_hdr, sizeof(blk_hdr))) {
            break;
        }

        if (!valid_block_header(db, blk_hdr, db->flash.sector_size - pos)) {
            meta->has_torn_tail = true;
            db->torn_blocks_recovered++;
            break;
        }

        payload_len = get_u16_le(blk_hdr + 4U);
        sample_cnt = blk_hdr[6];
        b_min_seq = get_u64_le(blk_hdr + 8U);
        b_max_seq = get_u64_le(blk_hdr + 16U);
        b_min_wall = get_i64_le(blk_hdr + 24U);
        b_max_wall = get_i64_le(blk_hdr + 32U);
        payload_crc = get_u32_le(blk_hdr + 48U);

        padded_len = get_u32_le(blk_hdr + 56U);
        next_pos = pos + FDB_EDGE_BLOCK_HDR_BYTES + padded_len + db->commit_aligned_bytes;

        /* Read commit trailer BEFORE trusting payload. */
        if (db->flash.read(
                db->flash.ctx,
                sec_base + pos + FDB_EDGE_BLOCK_HDR_BYTES + padded_len,
                cmt_buf,
                db->commit_aligned_bytes) < 0) {
            return FDB_EDGE_ERR_IO;
        }
        cmt_magic = get_u32_le(cmt_buf);
        cmt_crc = get_u32_le(cmt_buf + 4U);
        if (cmt_magic != EDGE_CMT_MAGIC || cmt_crc != (payload_crc ^ 0xA5A55A5AU)) {
            meta->has_torn_tail = true;
            db->torn_blocks_recovered++;
            break;
        }

        if (db->flash.read(
                db->flash.ctx,
                sec_base + pos + FDB_EDGE_BLOCK_HDR_BYTES,
                db->work_buf,
                payload_len) < 0) {
            return FDB_EDGE_ERR_IO;
        }
        if (fdb_edge_crc32c(db->work_buf, payload_len) != payload_crc) {
            meta->has_torn_tail = true;
            db->torn_blocks_recovered++;
            break;
        }

        if (b_max_seq > last->mono_seq) {
            size_t dec_cnt = 0U;
            const fdb_edge_sample_t *sample = &db->decode_buf[sample_cnt - 1U];
            if (fdb_edge_codec_decode(db->work_buf, payload_len, db->decode_buf,
                    FDB_EDGE_MAX_BLOCK_SAMPLES, &dec_cnt) != FDB_EDGE_OK ||
                dec_cnt != sample_cnt || db->decode_buf[0].mono_seq != b_min_seq ||
                sample->mono_seq != b_max_seq || sample->epoch != get_u32_le(blk_hdr + 44U)) {
                meta->has_torn_tail = true;
                db->torn_blocks_recovered++;
                break;
            }
            last->wall_ts_ms = sample->wall_ts_ms;
            last->mono_seq = sample->mono_seq;
            last->epoch = sample->epoch;
            last->logical_sub = sample->logical_sub;
        }

        if (meta->block_count == 0U) {
            meta->min_mono_seq = b_min_seq;
            meta->max_mono_seq = b_max_seq;
            meta->min_wall_ms = b_min_wall;
            meta->max_wall_ms = b_max_wall;
        } else {
            if (b_min_seq < meta->min_mono_seq) {
                meta->min_mono_seq = b_min_seq;
            }
            if (b_max_seq > meta->max_mono_seq) {
                meta->max_mono_seq = b_max_seq;
            }
            if (b_min_wall < meta->min_wall_ms) {
                meta->min_wall_ms = b_min_wall;
            }
            if (b_max_wall > meta->max_wall_ms) {
                meta->max_wall_ms = b_max_wall;
            }
        }

        meta->block_count++;
        meta->sample_count += sample_cnt;
        db->committed_blocks++;
        db->flushed_samples += sample_cnt;

        pos = next_pos;
        meta->write_offset = pos;
    }

    if (meta->has_torn_tail && !db->cfg.allow_torn_recovery) {
        return FDB_EDGE_ERR_CORRUPT;
    }
    return FDB_EDGE_OK;
}

static void restore_hlc(fdb_edge_tsdb_t *db, const fdb_edge_stamp_t *last)
{
    fdb_edge_hlc_init(&db->hlc, last->wall_ts_ms, last->mono_seq, last->epoch);
    db->hlc.logical_sub = last->logical_sub;
    db->hlc.rollback_count = last->epoch;
}

static void consume_stage_prefix(fdb_edge_tsdb_t *db, size_t count)
{
    if (count == 0U) {
        return;
    }
    db->stage_len -= count;
    if (db->stage_len > 0U) {
        memmove(db->stage, db->stage + count, db->stage_len * sizeof(db->stage[0]));
    }
}

static int refresh_pending_commit(fdb_edge_tsdb_t *db)
{
    fdb_edge_sec_meta_t *meta;
    fdb_edge_sec_meta_t old_meta;
    fdb_edge_stamp_t last = {0};
    uint64_t old_blocks;
    uint64_t old_samples;
    uint64_t old_torn;
    size_t committed = 0U;
    int rc;

    if (!db->commit_readback_pending) {
        return FDB_EDGE_OK;
    }
    meta = &db->sectors[db->cur_sec_idx];
    old_meta = *meta;
    old_blocks = db->committed_blocks;
    old_samples = db->flushed_samples;
    old_torn = db->torn_blocks_recovered;
    db->committed_blocks -= meta->block_count;
    db->flushed_samples -= meta->sample_count;
    rc = scan_sector_blocks(db, (uint32_t)db->cur_sec_idx, &last);
    if (rc != FDB_EDGE_OK && !(rc == FDB_EDGE_ERR_CORRUPT && meta->has_torn_tail)) {
        *meta = old_meta;
        db->committed_blocks = old_blocks;
        db->flushed_samples = old_samples;
        db->torn_blocks_recovered = old_torn;
        return rc;
    }
    db->commit_readback_pending = false;
    /* Fence the entire tail after the driver reported a failed program. */
    meta->has_torn_tail = true;
    while (committed < db->stage_len && db->stage[committed].mono_seq <= last.mono_seq) {
        committed++;
    }
    consume_stage_prefix(db, committed);
    if (last.mono_seq > db->hlc.monotonic_seq) {
        restore_hlc(db, &last);
    }
    return FDB_EDGE_OK;
}

int fdb_edge_tsdb_open(
    fdb_edge_tsdb_t *db,
    const fdb_flash_ops_t *flash,
    const fdb_edge_tsdb_config_t *cfg)
{
    fdb_flash_ops_t supplied_flash;
    fdb_edge_tsdb_config_t supplied_cfg;
    uint32_t overhead;
    uint32_t max_usable;
    uint32_t i;
    fdb_edge_stamp_t last = {0};

    if (!db || !flash || !cfg || !flash->read || !flash->write || !flash->erase) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (cfg->num_channels == 0U || cfg->num_channels > FDB_EDGE_MAX_CHANNELS) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (flash->write_gran > FDB_EDGE_MAX_WRITE_GRAN ||
        (flash->write_gran != 1U && flash->write_gran != 4U &&
         flash->write_gran != 8U && flash->write_gran != 32U)) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (flash->sector_size < 256U || (flash->sector_size & (flash->sector_size - 1U)) != 0U) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (flash->total_size / flash->sector_size < 2U ||
        (flash->total_size % flash->sector_size) != 0U) {
        return FDB_EDGE_ERR_INVALID;
    }

    supplied_flash = *flash;
    supplied_cfg = *cfg;
    memset(db, 0, sizeof(*db));
    db->flash = supplied_flash;
    db->cfg = supplied_cfg;
    flash = &db->flash;
    cfg = &db->cfg;
    if (db->cfg.batch_size == 0U) {
        db->cfg.batch_size = FDB_EDGE_MAX_BLOCK_SAMPLES < 64U ? FDB_EDGE_MAX_BLOCK_SAMPLES : 64U;
    }
    if (db->cfg.batch_size > FDB_EDGE_MAX_BLOCK_SAMPLES) {
        return FDB_EDGE_ERR_INVALID;
    }

    db->sector_count = flash->total_size / flash->sector_size;
    if (db->sector_count > FDB_EDGE_MAX_TRACKED_SECTORS) {
        return FDB_EDGE_ERR_CAPACITY;
    }

    db->commit_aligned_bytes = align_up_u32(8U, flash->write_gran);
    overhead = min_usable_overhead(flash->write_gran);
    if (flash->sector_size <= overhead + flash->write_gran) {
        return FDB_EDGE_ERR_CAPACITY;
    }
    max_usable = flash->sector_size - overhead;
    if (max_usable > (uint32_t)sizeof(db->work_buf)) {
        max_usable = (uint32_t)sizeof(db->work_buf);
    }
    max_usable -= max_usable % flash->write_gran;
    if (max_usable > UINT16_MAX) {
        max_usable = UINT16_MAX;
    }

    if (cfg->max_block_bytes > 0U) {
        if (cfg->max_block_bytes > max_usable) {
            return FDB_EDGE_ERR_CAPACITY;
        }
        db->effective_max_block_bytes = cfg->max_block_bytes;
    } else {
        db->effective_max_block_bytes = max_usable;
    }

    db->cur_sec_idx = -1;
    db->next_sector_seq = 1ULL;

    for (i = 0; i < db->sector_count; i++) {
        uint8_t sec_hdr[FDB_EDGE_SECTOR_HDR_BYTES];
        uint32_t base = i * flash->sector_size;
        fdb_edge_sec_meta_t *meta = &db->sectors[i];

        if (flash->read(flash->ctx, base, sec_hdr, sizeof(sec_hdr)) < 0) {
            return FDB_EDGE_ERR_IO;
        }

        if (is_all_ff(sec_hdr, 32U)) {
            meta->state = is_all_ff(sec_hdr + 32U, 64U)
                ? FDB_EDGE_SEC_CLEAN_ERASED
                : FDB_EDGE_SEC_TORN_HDR;
            if (meta->state == FDB_EDGE_SEC_TORN_HDR) {
                db->torn_blocks_recovered++;
            }
            continue;
        }

        if (get_u32_le(sec_hdr + 0U) != EDGE_SEC_MAGIC ||
            get_u16_le(sec_hdr + 4U) != EDGE_HDR_VER || get_u64_le(sec_hdr + 8U) == 0ULL ||
            fdb_edge_crc32c(sec_hdr, 20U) != get_u32_le(sec_hdr + 20U)) {
            meta->state = FDB_EDGE_SEC_TORN_HDR;
            db->torn_blocks_recovered++;
            continue;
        }

        meta->sector_seq = get_u64_le(sec_hdr + 8U);
        meta->erase_count = get_u32_le(sec_hdr + 16U);
        if (meta->sector_seq >= db->next_sector_seq) {
            db->next_sector_seq = meta->sector_seq == UINT64_MAX ? UINT64_MAX : meta->sector_seq + 1ULL;
        }
        if (db->cur_sec_idx < 0 || meta->sector_seq > db->sectors[db->cur_sec_idx].sector_seq) {
            db->cur_sec_idx = (int32_t)i;
        }

        if (is_all_ff(sec_hdr + 32U, 64U)) {
            meta->state = FDB_EDGE_SEC_ACTIVE_UNSEALED;
        } else if (get_u32_le(sec_hdr + 72U) == EDGE_SEAL_MAGIC &&
                   fdb_edge_crc32c(sec_hdr + 32U, 44U) == get_u32_le(sec_hdr + 76U)) {
            meta->state = FDB_EDGE_SEC_SEALED;
        } else {
            meta->state = FDB_EDGE_SEC_TORN_SEAL;
            db->torn_blocks_recovered++;
        }

        /* The predecessor's seal journals eviction before the victim's header is erased. */
        if (get_u32_le(sec_hdr + 80U) == EDGE_WEAR_MAGIC &&
            fdb_edge_crc32c(sec_hdr + 80U, 12U) == get_u32_le(sec_hdr + 92U)) {
            uint32_t victim = get_u32_le(sec_hdr + 84U);
            uint32_t erase_count = get_u32_le(sec_hdr + 88U);
            if (victim < db->sector_count && victim != i &&
                !has_sector_header(&db->sectors[victim]) && erase_count > db->sectors[victim].erase_count) {
                db->sectors[victim].erase_count = erase_count;
            }
        }
    }

    for (i = 0; i < db->sector_count; i++) {
        if (has_sector_header(&db->sectors[i])) {
            int rc = scan_sector_blocks(db, i, &last);
            if (rc != FDB_EDGE_OK) {
                return rc;
            }
        }
    }

    restore_hlc(db, &last);
    db->initialized = true;
    return FDB_EDGE_OK;
}

static int seal_current_sector(fdb_edge_tsdb_t *db, uint32_t victim, bool will_erase)
{
    uint32_t idx;
    fdb_edge_sec_meta_t *meta;
    uint8_t seal_buf[64];
    uint32_t base;

    if (db->cur_sec_idx < 0) {
        return FDB_EDGE_OK;
    }
    idx = (uint32_t)db->cur_sec_idx;
    meta = &db->sectors[idx];
    if (meta->state != FDB_EDGE_SEC_ACTIVE_UNSEALED) {
        return FDB_EDGE_OK;
    }

    memset(seal_buf, 0x00, sizeof(seal_buf));
    put_u64_le(seal_buf + 0U, meta->min_mono_seq);
    put_u64_le(seal_buf + 8U, meta->max_mono_seq);
    put_u64_le(seal_buf + 16U, (uint64_t)meta->min_wall_ms);
    put_u64_le(seal_buf + 24U, (uint64_t)meta->max_wall_ms);
    put_u32_le(seal_buf + 32U, meta->sample_count);
    put_u32_le(seal_buf + 36U, meta->block_count);
    put_u32_le(seal_buf + 40U, EDGE_SEAL_MAGIC);
    put_u32_le(seal_buf + 44U, fdb_edge_crc32c(seal_buf, 44U));
    if (will_erase) {
        uint32_t prior_count = db->sectors[victim].erase_count;
        put_u32_le(seal_buf + 48U, EDGE_WEAR_MAGIC);
        put_u32_le(seal_buf + 52U, victim);
        put_u32_le(seal_buf + 56U, prior_count == UINT32_MAX ? UINT32_MAX : prior_count + 1U);
        put_u32_le(seal_buf + 60U, fdb_edge_crc32c(seal_buf + 48U, 12U));
    }

    base = idx * db->flash.sector_size;
    if (db->flash.write(db->flash.ctx, base + 32U, seal_buf, sizeof(seal_buf)) < 0) {
        meta->state = FDB_EDGE_SEC_TORN_SEAL;
        db->torn_blocks_recovered++;
        return FDB_EDGE_ERR_IO;
    }
    meta->state = FDB_EDGE_SEC_SEALED;
    return FDB_EDGE_OK;
}

static int check_erased_sector(const fdb_edge_tsdb_t *db, uint32_t idx, bool *erased)
{
    uint8_t bytes[128];
    uint32_t base = idx * db->flash.sector_size;
    uint32_t pos;

    *erased = false;
    for (pos = 0U; pos < db->flash.sector_size; pos += (uint32_t)sizeof(bytes)) {
        if (db->flash.read(db->flash.ctx, base + pos, bytes, sizeof(bytes)) < 0) {
            return FDB_EDGE_ERR_IO;
        }
        if (!is_all_ff(bytes, sizeof(bytes))) {
            return FDB_EDGE_OK;
        }
    }
    *erased = true;
    return FDB_EDGE_OK;
}

static int32_t newest_sector_except(const fdb_edge_tsdb_t *db, uint32_t excluded)
{
    int32_t newest = -1;
    uint32_t i;
    for (i = 0U; i < db->sector_count; i++) {
        if (i != excluded && has_sector_header(&db->sectors[i]) &&
            (newest < 0 || db->sectors[i].sector_seq > db->sectors[newest].sector_seq)) {
            newest = (int32_t)i;
        }
    }
    return newest;
}

static void refresh_failed_erase(fdb_edge_tsdb_t *db, uint32_t victim)
{
    fdb_edge_sec_meta_t *meta = &db->sectors[victim];
    uint8_t header[24];
    uint32_t base = victim * db->flash.sector_size;

    if (!has_sector_header(meta) || db->flash.read(db->flash.ctx, base, header, sizeof(header)) < 0) {
        return;
    }
    if (get_u32_le(header) != EDGE_SEC_MAGIC || get_u64_le(header + 8U) != meta->sector_seq ||
        fdb_edge_crc32c(header, 20U) != get_u32_le(header + 20U)) {
        meta->state = FDB_EDGE_SEC_TORN_HDR;
        db->torn_blocks_recovered++;
        if ((int32_t)victim == db->cur_sec_idx) {
            db->cur_sec_idx = newest_sector_except(db, victim);
        }
    } else {
        fdb_edge_sec_meta_t old_meta = *meta;
        fdb_edge_stamp_t last = {0};
        uint64_t old_blocks = db->committed_blocks;
        uint64_t old_samples = db->flushed_samples;
        int rc = scan_sector_blocks(db, victim, &last);
        db->committed_blocks = old_blocks;
        db->flushed_samples = old_samples;
        if (rc != FDB_EDGE_OK && rc != FDB_EDGE_ERR_CORRUPT) {
            *meta = old_meta;
        }
        /* A surviving header remains readable, but an interrupted erase cannot make its tail writable. */
        meta->has_torn_tail = true;
    }
}

static int pick_and_activate_next_sector(fdb_edge_tsdb_t *db)
{
    uint32_t start = (db->cur_sec_idx >= 0) ? ((uint32_t)db->cur_sec_idx + 1U) % db->sector_count : 0U;
    int32_t chosen = -1;
    int32_t torn = -1;
    bool needs_erase = true;
    uint32_t k;
    uint64_t oldest_seq = UINT64_MAX;
    uint8_t act_buf[32];
    uint32_t base;
    uint32_t erase_count;
    uint64_t sector_seq;
    int rc;

    if (db->next_sector_seq == UINT64_MAX) {
        return FDB_EDGE_ERR_BOUNDS;
    }

    for (k = 0; k < db->sector_count; k++) {
        uint32_t idx = (start + k) % db->sector_count;
        if (db->sectors[idx].state == FDB_EDGE_SEC_CLEAN_ERASED) {
            bool erased;
            rc = check_erased_sector(db, idx, &erased);
            if (rc != FDB_EDGE_OK) {
                return rc;
            }
            if (erased) {
                chosen = (int32_t)idx;
                needs_erase = false;
                break;
            }
            /* An interrupted erase can leave a blank header above old programmed words. */
            db->sectors[idx].state = FDB_EDGE_SEC_TORN_HDR;
            db->torn_blocks_recovered++;
        }
        if (torn < 0 && db->sectors[idx].state == FDB_EDGE_SEC_TORN_HDR) {
            torn = (int32_t)idx;
        }
    }

    if (chosen < 0) {
        chosen = torn;
    }
    if (chosen < 0) {
        for (k = 0; k < db->sector_count; k++) {
            uint32_t idx = (start + k) % db->sector_count;
            if (db->sectors[idx].block_count == 0U) {
                chosen = (int32_t)idx;
                break;
            }
        }
    }
    if (chosen < 0) {
        if (!db->cfg.rollover) {
            return FDB_EDGE_ERR_FULL;
        }
        for (k = 0; k < db->sector_count; k++) {
            uint32_t idx = (start + k) % db->sector_count;
            if ((int32_t)idx == db->cur_sec_idx) {
                continue;
            }
            if (db->sectors[idx].sector_seq < oldest_seq) {
                oldest_seq = db->sectors[idx].sector_seq;
                chosen = (int32_t)idx;
            }
        }
    }

    if (chosen < 0) {
        return FDB_EDGE_ERR_FULL;
    }

    if (chosen != db->cur_sec_idx) {
        rc = seal_current_sector(db, (uint32_t)chosen, needs_erase);
        if (rc != FDB_EDGE_OK) {
            return rc;
        }
    }
    base = (uint32_t)chosen * db->flash.sector_size;
    erase_count = db->sectors[chosen].erase_count;
    if (needs_erase) {
        if (db->flash.erase(db->flash.ctx, base, db->flash.sector_size) < 0) {
            refresh_failed_erase(db, (uint32_t)chosen);
            return FDB_EDGE_ERR_IO;
        }
        if (erase_count < UINT32_MAX) {
            erase_count++;
        }
    }

    sector_seq = db->next_sector_seq++;
    if (chosen == db->cur_sec_idx) {
        db->cur_sec_idx = newest_sector_except(db, (uint32_t)chosen);
    }
    memset(&db->sectors[chosen], 0, sizeof(db->sectors[chosen]));
    db->sectors[chosen].state = FDB_EDGE_SEC_TORN_HDR;
    db->sectors[chosen].erase_count = erase_count;

    memset(act_buf, 0xFF, sizeof(act_buf));
    put_u32_le(act_buf + 0U, EDGE_SEC_MAGIC);
    put_u16_le(act_buf + 4U, EDGE_HDR_VER);
    put_u16_le(act_buf + 6U, 0U);
    put_u64_le(act_buf + 8U, sector_seq);
    put_u32_le(act_buf + 16U, erase_count);
    put_u32_le(act_buf + 20U, fdb_edge_crc32c(act_buf, 20U));
    if (db->flash.write(db->flash.ctx, base, act_buf, sizeof(act_buf)) < 0) {
        db->torn_blocks_recovered++;
        return FDB_EDGE_ERR_IO;
    }

    db->sectors[chosen].state = FDB_EDGE_SEC_ACTIVE_UNSEALED;
    db->sectors[chosen].sector_seq = sector_seq;
    db->sectors[chosen].write_offset = FDB_EDGE_SECTOR_HDR_BYTES;
    db->cur_sec_idx = chosen;
    return FDB_EDGE_OK;
}

static int commit_encoded_block(
    fdb_edge_tsdb_t *db,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t sample_count,
    uint8_t num_channels,
    uint64_t min_seq,
    uint64_t max_seq,
    int64_t min_wall,
    int64_t max_wall,
    uint32_t min_epoch,
    uint32_t max_epoch,
    bool *committed)
{
    uint32_t padded_len;
    uint32_t slot_bytes;
    uint32_t sec_base;
    uint32_t pos;
    uint32_t payload_crc;
    uint8_t blk_hdr[FDB_EDGE_BLOCK_HDR_BYTES];
    uint8_t cmt_buf[EDGE_COMMIT_BUF_BYTES];
    fdb_edge_sec_meta_t *meta;
    int rc;

    *committed = false;
    if (payload_len == 0U || payload_len > db->effective_max_block_bytes) {
        return FDB_EDGE_ERR_CAPACITY;
    }

    padded_len = align_up_u32((uint32_t)payload_len, db->flash.write_gran);
    slot_bytes = FDB_EDGE_BLOCK_HDR_BYTES + padded_len + db->commit_aligned_bytes;

    if (db->cur_sec_idx < 0 || db->sectors[db->cur_sec_idx].state != FDB_EDGE_SEC_ACTIVE_UNSEALED ||
        db->sectors[db->cur_sec_idx].has_torn_tail ||
        slot_bytes > db->flash.sector_size - db->sectors[db->cur_sec_idx].write_offset) {
        rc = pick_and_activate_next_sector(db);
        if (rc != FDB_EDGE_OK) {
            return rc;
        }
    }

    meta = &db->sectors[db->cur_sec_idx];
    sec_base = (uint32_t)db->cur_sec_idx * db->flash.sector_size;
    pos = meta->write_offset;
    payload_crc = fdb_edge_crc32c(payload, payload_len);

    memset(blk_hdr, 0x00, sizeof(blk_hdr));
    put_u32_le(blk_hdr + 0U, EDGE_BLK_MAGIC);
    put_u16_le(blk_hdr + 4U, (uint16_t)payload_len);
    blk_hdr[6] = sample_count;
    blk_hdr[7] = num_channels;
    put_u64_le(blk_hdr + 8U, min_seq);
    put_u64_le(blk_hdr + 16U, max_seq);
    put_u64_le(blk_hdr + 24U, (uint64_t)min_wall);
    put_u64_le(blk_hdr + 32U, (uint64_t)max_wall);
    put_u32_le(blk_hdr + 40U, min_epoch);
    put_u32_le(blk_hdr + 44U, max_epoch);
    put_u32_le(blk_hdr + 48U, payload_crc);
    put_u32_le(blk_hdr + 52U, 0U);
    put_u32_le(blk_hdr + 56U, padded_len);
    put_u32_le(blk_hdr + 60U, fdb_edge_crc32c(blk_hdr, 60U));

    /* Copy payload into padded staging slice (never modifies payload past payload_len if already work_buf). */
    if (payload != db->work_buf) {
        memcpy(db->work_buf, payload, payload_len);
    }
    if (padded_len > payload_len) {
        memset(db->work_buf + payload_len, 0x00, padded_len - payload_len);
    }

    /* Step 1: Write Block Header */
    if (db->flash.write(db->flash.ctx, sec_base + pos, blk_hdr, sizeof(blk_hdr)) < 0) {
        meta->has_torn_tail = true;
        db->torn_blocks_recovered++;
        return FDB_EDGE_ERR_IO;
    }
    /* Step 2: Write Padded Payload */
    if (db->flash.write(
            db->flash.ctx,
            sec_base + pos + FDB_EDGE_BLOCK_HDR_BYTES,
            db->work_buf,
            padded_len) < 0) {
        meta->has_torn_tail = true;
        db->torn_blocks_recovered++;
        return FDB_EDGE_ERR_IO;
    }
    /* Step 3: Write Commit Trailer LAST */
    memset(cmt_buf, 0x00, db->commit_aligned_bytes);
    put_u32_le(cmt_buf + 0U, EDGE_CMT_MAGIC);
    put_u32_le(cmt_buf + 4U, payload_crc ^ 0xA5A55A5AU);
    rc = db->flash.write(
            db->flash.ctx,
            sec_base + pos + FDB_EDGE_BLOCK_HDR_BYTES + padded_len,
            cmt_buf,
            db->commit_aligned_bytes);
    if (rc < 0) {
        uint8_t observed[8];
        meta->has_torn_tail = true;
        /* A driver may fail after programming the bytes checked at cold recovery. */
        if (db->flash.read(db->flash.ctx, sec_base + pos + FDB_EDGE_BLOCK_HDR_BYTES + padded_len,
                observed, sizeof(observed)) < 0) {
            db->commit_readback_pending = true;
            return FDB_EDGE_ERR_IO;
        }
        if (memcmp(observed, cmt_buf, sizeof(observed)) != 0) {
            db->torn_blocks_recovered++;
            return FDB_EDGE_ERR_IO;
        }
    }

    if (meta->block_count == 0U) {
        meta->min_mono_seq = min_seq;
        meta->max_mono_seq = max_seq;
        meta->min_wall_ms = min_wall;
        meta->max_wall_ms = max_wall;
    } else {
        if (min_seq < meta->min_mono_seq) {
            meta->min_mono_seq = min_seq;
        }
        if (max_seq > meta->max_mono_seq) {
            meta->max_mono_seq = max_seq;
        }
        if (min_wall < meta->min_wall_ms) {
            meta->min_wall_ms = min_wall;
        }
        if (max_wall > meta->max_wall_ms) {
            meta->max_wall_ms = max_wall;
        }
    }

    meta->block_count++;
    meta->sample_count += sample_count;
    meta->write_offset = pos + slot_bytes;
    db->committed_blocks++;
    db->flushed_samples += sample_count;
    *committed = true;
    return rc < 0 ? FDB_EDGE_ERR_IO : FDB_EDGE_OK;
}

static int flush_slice(fdb_edge_tsdb_t *db, size_t start, size_t count, size_t encoded_len, bool *committed)
{
    size_t i;
    int64_t min_w;
    int64_t max_w;
    uint32_t min_ep;
    uint32_t max_ep;
    min_w = max_w = db->stage[start].wall_ts_ms;
    min_ep = max_ep = db->stage[start].epoch;
    for (i = 1; i < count; i++) {
        int64_t w = db->stage[start + i].wall_ts_ms;
        uint32_t ep = db->stage[start + i].epoch;
        if (w < min_w) min_w = w;
        if (w > max_w) max_w = w;
        if (ep < min_ep) min_ep = ep;
        if (ep > max_ep) max_ep = ep;
    }

    return commit_encoded_block(
        db,
        db->work_buf,
        encoded_len,
        (uint8_t)count,
        db->cfg.num_channels,
        db->stage[start].mono_seq,
        db->stage[start + count - 1U].mono_seq,
        min_w,
        max_w,
        min_ep,
        max_ep,
        committed);
}

int fdb_edge_tsdb_flush(fdb_edge_tsdb_t *db)
{
    /* Deferred right halves fit in one entry per bit of the on-flash sample count. */
    uint8_t pending[CHAR_BIT];
    size_t depth = 0U;
    size_t committed_count = 0U;
    size_t count;
    int rc;

    if (!db || !db->initialized) {
        return FDB_EDGE_ERR_INVALID;
    }
    rc = refresh_pending_commit(db);
    if (rc != FDB_EDGE_OK) {
        return rc;
    }
    count = db->stage_len;
    while (count > 0U) {
        size_t encoded_len = 0U;
        bool committed;
        rc = fdb_edge_codec_encode(db->stage + committed_count, count, db->work_buf,
            db->effective_max_block_bytes, &encoded_len);
        if (rc == FDB_EDGE_ERR_BOUNDS) {
            size_t half = count / 2U;
            if (half == 0U) {
                rc = FDB_EDGE_ERR_CAPACITY;
                break;
            }
            pending[depth++] = (uint8_t)(count - half);
            count = half;
            continue;
        }
        if (rc != FDB_EDGE_OK) {
            break;
        }
        rc = flush_slice(db, committed_count, count, encoded_len, &committed);
        if (committed) {
            committed_count += count;
        }
        if (rc != FDB_EDGE_OK) {
            break;
        }
        count = depth == 0U ? 0U : pending[--depth];
    }
    consume_stage_prefix(db, committed_count);
    return rc;
}

int fdb_edge_tsdb_append(
    fdb_edge_tsdb_t *db,
    int64_t wall_ts_ms,
    const double *values,
    uint8_t num_channels,
    fdb_edge_stamp_t *out_stamp)
{
    fdb_edge_stamp_t stamp;
    fdb_edge_sample_t *slot;
    int rc;

    if (out_stamp) {
        memset(out_stamp, 0, sizeof(*out_stamp));
    }
    if (!db || !db->initialized || !values) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (num_channels != db->cfg.num_channels) {
        return FDB_EDGE_ERR_INVALID;
    }

    if (db->commit_readback_pending || db->stage_len >= db->cfg.batch_size) {
        rc = fdb_edge_tsdb_flush(db);
        if (rc != FDB_EDGE_OK) {
            return rc;
        }
    }

    rc = fdb_edge_hlc_next(&db->hlc, wall_ts_ms, &stamp);
    if (rc != FDB_EDGE_OK) {
        return rc;
    }
    if (out_stamp) {
        *out_stamp = stamp;
    }

    slot = &db->stage[db->stage_len++];
    memset(slot, 0, sizeof(*slot));
    slot->wall_ts_ms = stamp.wall_ts_ms;
    slot->mono_seq = stamp.mono_seq;
    slot->epoch = stamp.epoch;
    slot->logical_sub = stamp.logical_sub;
    slot->num_channels = num_channels;
    memcpy(slot->values, values, (size_t)num_channels * sizeof(double));

    if (db->stage_len >= db->cfg.batch_size) {
        return fdb_edge_tsdb_flush(db);
    }
    return FDB_EDGE_OK;
}

static bool sample_regresses(const fdb_edge_sample_t *sample, int64_t wall, uint32_t epoch, uint32_t sub)
{
    return sample->epoch < epoch || (sample->epoch == epoch &&
        (sample->wall_ts_ms < wall || (sample->wall_ts_ms == wall && sample->logical_sub < sub)));
}

static int append_raw_block(
    fdb_edge_tsdb_t *db,
    const uint8_t *payload,
    size_t payload_len,
    bool check_range,
    uint64_t from_seq,
    uint64_t to_seq)
{
    size_t dec_cnt = 0U;
    size_t i;
    int64_t min_w;
    int64_t max_w;
    uint32_t min_ep;
    uint32_t max_ep;
    fdb_edge_stamp_t last = {0};
    bool committed;
    int rc;

    if (!db || !db->initialized || !payload) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (payload_len > db->effective_max_block_bytes) {
        return FDB_EDGE_ERR_CAPACITY;
    }
    /* Stage flushes and pending commit recovery use the database's work buffer. */
    if (payload == db->work_buf && (db->stage_len > 0U || db->commit_readback_pending)) {
        return FDB_EDGE_ERR_INVALID;
    }
    rc = refresh_pending_commit(db);
    if (rc != FDB_EDGE_OK) {
        return rc;
    }
    rc = fdb_edge_codec_decode(payload, payload_len, db->decode_buf, FDB_EDGE_MAX_BLOCK_SAMPLES, &dec_cnt);
    if (rc != FDB_EDGE_OK || dec_cnt == 0U) {
        return FDB_EDGE_ERR_CORRUPT;
    }
    if (db->decode_buf[0].num_channels != db->cfg.num_channels) {
        return FDB_EDGE_ERR_INVALID;
    }
    last.wall_ts_ms = db->decode_buf[dec_cnt - 1U].wall_ts_ms;
    last.mono_seq = db->decode_buf[dec_cnt - 1U].mono_seq;
    last.epoch = db->decode_buf[dec_cnt - 1U].epoch;
    last.logical_sub = db->decode_buf[dec_cnt - 1U].logical_sub;
    if (check_range && (db->decode_buf[0].mono_seq != from_seq || last.mono_seq != to_seq)) {
        return FDB_EDGE_ERR_CORRUPT;
    }
    if (db->decode_buf[0].mono_seq <= db->hlc.monotonic_seq ||
        (db->hlc.initialized && sample_regresses(&db->decode_buf[0], db->hlc.last_wall_ms,
            db->hlc.epoch, db->hlc.logical_sub))) {
        return FDB_EDGE_ERR_ROLLBACK;
    }

    min_w = max_w = db->decode_buf[0].wall_ts_ms;
    min_ep = max_ep = db->decode_buf[0].epoch;
    for (i = 1; i < dec_cnt; i++) {
        int64_t w = db->decode_buf[i].wall_ts_ms;
        uint32_t ep = db->decode_buf[i].epoch;
        const fdb_edge_sample_t *previous = &db->decode_buf[i - 1U];
        if (sample_regresses(&db->decode_buf[i], previous->wall_ts_ms, previous->epoch, previous->logical_sub)) {
            return FDB_EDGE_ERR_ROLLBACK;
        }
        if (w < min_w) min_w = w;
        if (w > max_w) max_w = w;
        if (ep < min_ep) min_ep = ep;
        if (ep > max_ep) max_ep = ep;
    }

    rc = fdb_edge_tsdb_flush(db);
    if (rc != FDB_EDGE_OK) {
        return rc;
    }
    rc = commit_encoded_block(
        db,
        payload,
        payload_len,
        (uint8_t)dec_cnt,
        db->cfg.num_channels,
        db->decode_buf[0].mono_seq,
        last.mono_seq,
        min_w,
        max_w,
        min_ep,
        max_ep,
        &committed);
    if (committed) {
        restore_hlc(db, &last);
    }
    return rc;
}

int fdb_edge_tsdb_append_raw_block(fdb_edge_tsdb_t *db, const uint8_t *payload, size_t payload_len)
{
    return append_raw_block(db, payload, payload_len, false, 0ULL, 0ULL);
}

int fdb_edge_tsdb_append_raw_block_range(
    fdb_edge_tsdb_t *db, const uint8_t *payload, size_t payload_len, uint64_t from_seq, uint64_t to_seq)
{
    return append_raw_block(db, payload, payload_len, true, from_seq, to_seq);
}

static int query_internal(
    fdb_edge_tsdb_t *db,
    bool by_wall,
    int64_t from_i,
    int64_t to_i,
    uint64_t from_u,
    uint64_t to_u,
    fdb_edge_sample_cb cb,
    void *user_ctx)
{
    int32_t idx;
    size_t k;
    int rc;

    if (!db || !db->initialized || !cb) {
        return FDB_EDGE_ERR_INVALID;
    }

    rc = refresh_pending_commit(db);
    if (rc != FDB_EDGE_OK) {
        return rc;
    }
    for (idx = next_ordered_sector(db, -1); idx >= 0; idx = next_ordered_sector(db, idx)) {
        const fdb_edge_sec_meta_t *meta = &db->sectors[idx];
        uint32_t sec_base = (uint32_t)idx * db->flash.sector_size;
        uint32_t pos = FDB_EDGE_SECTOR_HDR_BYTES;
        uint32_t b;

        if (meta->block_count == 0U) {
            continue;
        }
        if (by_wall) {
            if (meta->max_wall_ms < from_i || meta->min_wall_ms > to_i) {
                continue;
            }
        } else {
            if (meta->max_mono_seq < from_u || meta->min_mono_seq > to_u) {
                continue;
            }
        }

        for (b = 0; b < meta->block_count; b++) {
            uint8_t blk_hdr[FDB_EDGE_BLOCK_HDR_BYTES];
            uint16_t plen;
            uint32_t padded;
            uint32_t dec_cnt_u = 0U;
            size_t dec_cnt = 0U;
            uint64_t b_min_u;
            uint64_t b_max_u;
            int64_t b_min_i;
            int64_t b_max_i;

            if (db->flash.read(db->flash.ctx, sec_base + pos, blk_hdr, sizeof(blk_hdr)) < 0) {
                return FDB_EDGE_ERR_IO;
            }
            if (!valid_block_header(db, blk_hdr, meta->write_offset - pos)) {
                return FDB_EDGE_ERR_CORRUPT;
            }
            plen = get_u16_le(blk_hdr + 4U);
            padded = get_u32_le(blk_hdr + 56U);
            b_min_u = get_u64_le(blk_hdr + 8U);
            b_max_u = get_u64_le(blk_hdr + 16U);
            b_min_i = get_i64_le(blk_hdr + 24U);
            b_max_i = get_i64_le(blk_hdr + 32U);

            pos += FDB_EDGE_BLOCK_HDR_BYTES;
            if ((by_wall && (b_max_i >= from_i && b_min_i <= to_i)) ||
                (!by_wall && (b_max_u >= from_u && b_min_u <= to_u))) {
                if (db->flash.read(db->flash.ctx, sec_base + pos, db->work_buf, plen) < 0) {
                    return FDB_EDGE_ERR_IO;
                }
                if (fdb_edge_crc32c(db->work_buf, plen) != get_u32_le(blk_hdr + 48U) ||
                    fdb_edge_codec_decode(
                        db->work_buf, plen, db->decode_buf, FDB_EDGE_MAX_BLOCK_SAMPLES, &dec_cnt) != FDB_EDGE_OK) {
                    return FDB_EDGE_ERR_CORRUPT;
                }
                dec_cnt_u = (uint32_t)dec_cnt;
                for (k = 0; k < dec_cnt_u; k++) {
                    bool match = by_wall
                        ? (db->decode_buf[k].wall_ts_ms >= from_i && db->decode_buf[k].wall_ts_ms <= to_i)
                        : (db->decode_buf[k].mono_seq >= from_u && db->decode_buf[k].mono_seq <= to_u);
                    if (match && !cb(&db->decode_buf[k], user_ctx)) {
                        return FDB_EDGE_OK;
                    }
                }
            }
            pos += padded + db->commit_aligned_bytes;
        }
    }

    for (k = 0; k < db->stage_len; k++) {
        bool match = by_wall
            ? (db->stage[k].wall_ts_ms >= from_i && db->stage[k].wall_ts_ms <= to_i)
            : (db->stage[k].mono_seq >= from_u && db->stage[k].mono_seq <= to_u);
        if (match && !cb(&db->stage[k], user_ctx)) {
            return FDB_EDGE_OK;
        }
    }
    return FDB_EDGE_OK;
}

int fdb_edge_tsdb_query_range_wall(
    fdb_edge_tsdb_t *db,
    int64_t from_wall_ms,
    int64_t to_wall_ms,
    fdb_edge_sample_cb cb,
    void *user_ctx)
{
    return query_internal(db, true, from_wall_ms, to_wall_ms, 0ULL, 0ULL, cb, user_ctx);
}

int fdb_edge_tsdb_query_range_mono(
    fdb_edge_tsdb_t *db,
    uint64_t from_seq,
    uint64_t to_seq,
    fdb_edge_sample_cb cb,
    void *user_ctx)
{
    return query_internal(db, false, 0, 0, from_seq, to_seq, cb, user_ctx);
}

int fdb_edge_tsdb_iter_raw_blocks_after(
    fdb_edge_tsdb_t *db,
    uint64_t after_seq,
    fdb_edge_raw_block_cb cb,
    void *user_ctx)
{
    int32_t idx;
    int rc;

    if (!db || !db->initialized || !cb) {
        return FDB_EDGE_ERR_INVALID;
    }

    rc = refresh_pending_commit(db);
    if (rc != FDB_EDGE_OK) {
        return rc;
    }
    for (idx = next_ordered_sector(db, -1); idx >= 0; idx = next_ordered_sector(db, idx)) {
        const fdb_edge_sec_meta_t *meta = &db->sectors[idx];
        uint32_t sec_base = (uint32_t)idx * db->flash.sector_size;
        uint32_t pos = FDB_EDGE_SECTOR_HDR_BYTES;
        uint32_t b;

        if (meta->block_count == 0U || meta->max_mono_seq <= after_seq) {
            continue;
        }
        for (b = 0; b < meta->block_count; b++) {
            uint8_t blk_hdr[FDB_EDGE_BLOCK_HDR_BYTES];
            uint16_t plen;
            uint32_t padded;
            uint64_t b_min_seq;
            uint64_t b_max_seq;
            int64_t b_min_wall;
            int64_t b_max_wall;
            uint8_t scnt;

            if (db->flash.read(db->flash.ctx, sec_base + pos, blk_hdr, sizeof(blk_hdr)) < 0) {
                return FDB_EDGE_ERR_IO;
            }
            if (!valid_block_header(db, blk_hdr, meta->write_offset - pos)) {
                return FDB_EDGE_ERR_CORRUPT;
            }
            plen = get_u16_le(blk_hdr + 4U);
            scnt = blk_hdr[6];
            padded = get_u32_le(blk_hdr + 56U);
            b_min_seq = get_u64_le(blk_hdr + 8U);
            b_max_seq = get_u64_le(blk_hdr + 16U);
            b_min_wall = get_i64_le(blk_hdr + 24U);
            b_max_wall = get_i64_le(blk_hdr + 32U);
            pos += FDB_EDGE_BLOCK_HDR_BYTES;

            if (b_max_seq > after_seq) {
                if (db->flash.read(db->flash.ctx, sec_base + pos, db->work_buf, plen) < 0) {
                    return FDB_EDGE_ERR_IO;
                }
                if (fdb_edge_crc32c(db->work_buf, plen) != get_u32_le(blk_hdr + 48U)) {
                    return FDB_EDGE_ERR_CORRUPT;
                }
                if (!cb(db->work_buf, plen, b_min_seq, b_max_seq, b_min_wall, b_max_wall, scnt, user_ctx)) {
                    return FDB_EDGE_OK;
                }
            }
            pos += padded + db->commit_aligned_bytes;
        }
    }
    return FDB_EDGE_OK;
}

void fdb_edge_tsdb_get_stats(const fdb_edge_tsdb_t *db, fdb_edge_tsdb_stats_t *out_stats)
{
    uint32_t i;

    if (!db || !out_stats) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->total_samples = db->flushed_samples + (uint64_t)db->stage_len;
    out_stats->committed_blocks = db->committed_blocks;
    out_stats->flushed_samples = db->flushed_samples;
    out_stats->staged_samples = (uint64_t)db->stage_len;
    out_stats->rollback_events = (uint64_t)db->hlc.rollback_count;
    out_stats->torn_blocks_recovered = db->torn_blocks_recovered;
    out_stats->last_mono_seq = db->hlc.monotonic_seq;
    out_stats->last_wall_ms = db->hlc.last_wall_ms;
    out_stats->current_epoch = db->hlc.epoch;
    out_stats->sector_count = db->sector_count;
    for (i = 0; i < db->sector_count; i++) {
        if (db->sectors[i].state == FDB_EDGE_SEC_SEALED) {
            out_stats->sealed_sectors++;
        }
    }
}
