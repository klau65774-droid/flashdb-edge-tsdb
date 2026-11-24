#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <flashdb.h>
#include <fal.h>

#include "fdb_edge/fdb_edge_flash_sim.h"
#include "fdb_edge/fdb_edge_privacy.h"
#include "fdb_edge/fdb_edge_tsdb.h"
#include "fdb_edge/fdb_edge_wal_sync.h"

static uint64_t now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    uint64_t ticks, hz;
    if ((freq.QuadPart == 0 && !QueryPerformanceFrequency(&freq)) ||
        !QueryPerformanceCounter(&counter)) {
        return GetTickCount64() * 1000ULL;
    }
    ticks = (uint64_t)counter.QuadPart;
    hz = (uint64_t)freq.QuadPart;
    return (ticks / hz) * 1000000ULL + (uint64_t)((double)(ticks % hz) * 1000000.0 / (double)hz);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000L);
#endif
}

static uint64_t xorshift64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static bool parse_uint(const char *text, int base, uint64_t max, uint64_t *out)
{
    char *end;
    unsigned long long value;

    if (text[0] < '0' || text[0] > '9') {
        return false;
    }
    errno = 0;
    value = strtoull(text, &end, base);
    if (errno != 0 || *end != '\0' || value > max) {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= (m <= 2U) ? 1 : 0;
    {
        int64_t era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = (unsigned)(y - era * 400);
        unsigned adjusted_month = m > 2U ? m - 3U : m + 9U;
        unsigned doy = (153U * adjusted_month + 2U) / 5U + d - 1U;
        unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
        return era * 146097 + (int64_t)doe - 719468;
    }
}

static bool parse_decimal_digits(const char *text, size_t len, unsigned *out)
{
    unsigned value = 0U;
    size_t i;
    for (i = 0U; i < len; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10U + (unsigned)(text[i] - '0');
    }
    *out = value;
    return true;
}

static bool parse_iso_ts_ms(const char *s, int64_t *out_ms)
{
    static const unsigned days_in_month[12] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
    unsigned y, m, d, hh, mm, ss, max_day;
    int64_t days;
    if (strlen(s) != 19U || s[4] != '-' || s[7] != '-' || (s[10] != ' ' && s[10] != 'T') ||
        s[13] != ':' || s[16] != ':' ||
        !parse_decimal_digits(s, 4U, &y) || !parse_decimal_digits(s + 5, 2U, &m) ||
        !parse_decimal_digits(s + 8, 2U, &d) || !parse_decimal_digits(s + 11, 2U, &hh) ||
        !parse_decimal_digits(s + 14, 2U, &mm) || !parse_decimal_digits(s + 17, 2U, &ss) ||
        m == 0U || m > 12U || d == 0U || hh > 23U || mm > 59U || ss > 59U) {
        return false;
    }
    max_day = days_in_month[m - 1U];
    if (m == 2U && y % 4U == 0U && (y % 100U != 0U || y % 400U == 0U)) {
        max_day++;
    }
    if (d > max_day) {
        return false;
    }
    days = days_from_civil(y, m, d);
    *out_ms = (days * 86400LL + (int64_t)hh * 3600LL + (int64_t)mm * 60LL + ss) * 1000LL;
    return true;
}

static fdb_time_t g_up_time_stub = 1700000000LL;
static fdb_time_t up_get_time(void)
{
    return ++g_up_time_stub;
}

/* ==================== Upstream Reproduction Subcommand ==================== */
typedef struct {
    struct fdb_tsdb *db;
    size_t count;
    size_t ok_reads;
} up_count_ctx_t;

static bool up_iter_cb(fdb_tsl_t tsl, void *arg)
{
    up_count_ctx_t *ctx = (up_count_ctx_t *)arg;
    uint8_t buf[128] = {0};
    struct fdb_blob blob;
    ctx->count++;
    if (tsl->status == FDB_TSL_WRITE && tsl->log_len <= sizeof(buf)) {
        fdb_blob_make(&blob, buf, tsl->log_len);
        if (fdb_blob_read((fdb_db_t)ctx->db, fdb_tsl_to_blob(tsl, &blob)) == tsl->log_len) {
            ctx->ok_reads++;
        }
    }
    return false;
}

static bool edge_count_cb(const fdb_edge_sample_t *sample, void *arg)
{
    size_t *cnt = (size_t *)arg;
    (void)sample;
    (*cnt)++;
    return true;
}

static int cmd_repro_upstream(void)
{
    /* 1. Issue #16 (-1h clock rollback) */
    fdb_flash_sim_t sim_up;
    fdb_flash_sim_t sim_edge;
    fdb_flash_ops_t ops_edge;
    struct fdb_tsdb up_db;
    static fdb_edge_tsdb_t edge_db;
    fdb_edge_tsdb_config_t cfg;
    struct fdb_blob blob;
    int up_ok_16 = 0, up_drop_16 = 0;
    size_t edge_ok_16 = 0, edge_queried_16 = 0;
    int i;

    /* Upstream Issue #249 / Capacity underflow test (max_len=4050 < sector=4096) */
    fdb_flash_sim_t sim_up_cap;
    struct fdb_tsdb up_cap_db;
    up_count_ctx_t up_cap_iter;
    int up_cap_append_rc = 0;

    fdb_flash_sim_alloc_init(&sim_up, 8U * 4096U, 4096U, 1U);
    memset(&up_db, 0, sizeof(up_db));
    fdb_upstream_bind_sim(&sim_up);
    fdb_tsdb_init(&up_db, "up_16", "tsdb_sim", up_get_time, 128U, NULL);

    for (i = 0; i < 10; i++) {
        double v = 25.0 + (double)i;
        fdb_time_t ts = 1700000000LL + (int64_t)i * 60LL;
        fdb_blob_make(&blob, &v, sizeof(v));
        if (fdb_tsl_append_with_ts(&up_db, &blob, ts) == FDB_NO_ERR) up_ok_16++;
        else up_drop_16++;
    }
    for (i = 0; i < 5; i++) {
        double v = 35.0 + (double)i;
        /* -1 hour clock rollback */
        fdb_time_t ts = 1700000000LL - 3600LL + (int64_t)i * 60LL;
        fdb_blob_make(&blob, &v, sizeof(v));
        if (fdb_tsl_append_with_ts(&up_db, &blob, ts) == FDB_NO_ERR) up_ok_16++;
        else up_drop_16++;
    }
    fdb_tsdb_deinit(&up_db);
    fdb_upstream_bind_sim(NULL);

    fdb_flash_sim_alloc_init(&sim_edge, 8U * 4096U, 4096U, 1U);
    fdb_flash_sim_as_ops(&sim_edge, &ops_edge);
    fdb_edge_tsdb_default_config(&cfg, 1U);
    cfg.batch_size = 8U;
    fdb_edge_tsdb_open(&edge_db, &ops_edge, &cfg);
    for (i = 0; i < 10; i++) {
        double v = 25.0 + (double)i;
        int64_t ts = 1700000000000LL + (int64_t)i * 60000LL;
        if (fdb_edge_tsdb_append(&edge_db, ts, &v, 1U, NULL) == FDB_EDGE_OK) edge_ok_16++;
    }
    for (i = 0; i < 5; i++) {
        double v = 35.0 + (double)i;
        int64_t ts = 1700000000000LL - 3600000LL + (int64_t)i * 60000LL;
        if (fdb_edge_tsdb_append(&edge_db, ts, &v, 1U, NULL) == FDB_EDGE_OK) edge_ok_16++;
    }
    fdb_edge_tsdb_flush(&edge_db);
    fdb_edge_tsdb_query_range_mono(&edge_db, 1ULL, UINT64_MAX, edge_count_cb, &edge_queried_16);
    {
        fdb_edge_tsdb_stats_t est;
        fdb_edge_tsdb_get_stats(&edge_db, &est);

        /* Capacity underflow repro on unmodified upstream: max_len=4050 passes (max_len < 4096) */
        fdb_flash_sim_alloc_init(&sim_up_cap, 4U * 4096U, 4096U, 1U);
        memset(&up_cap_db, 0, sizeof(up_cap_db));
        fdb_upstream_bind_sim(&sim_up_cap);
        fdb_tsdb_init(&up_cap_db, "up_cap", "tsdb_sim", up_get_time, 4050U, NULL);
        {
            static uint8_t big_blob[4050];
            memset(big_blob, 0xAB, sizeof(big_blob));
            fdb_blob_make(&blob, big_blob, sizeof(big_blob));
            up_cap_append_rc = (int)fdb_tsl_append_with_ts(&up_cap_db, &blob, 1700000001LL);
        }
        up_cap_iter.db = &up_cap_db;
        up_cap_iter.count = 0U;
        up_cap_iter.ok_reads = 0U;
        fdb_tsl_iter(&up_cap_db, up_iter_cb, &up_cap_iter);
        fdb_tsdb_deinit(&up_cap_db);
        fdb_upstream_bind_sim(NULL);

        /* Enhanced guard: catches oversized max_block_bytes cleanly, and handles 10KB in 16KB sector cleanly */
        fdb_flash_sim_t sim_guard, sim_16k;
        fdb_flash_ops_t ops_guard, ops_16k;
        static fdb_edge_tsdb_t db_16k;
        int guard_rc;
        int ok_16k_open;
        size_t queried_16k = 0U;

        fdb_flash_sim_alloc_init(&sim_guard, 4U * 4096U, 4096U, 1U);
        fdb_flash_sim_as_ops(&sim_guard, &ops_guard);
        fdb_edge_tsdb_default_config(&cfg, 1U);
        cfg.max_block_bytes = 4050U;
        guard_rc = fdb_edge_tsdb_open(&db_16k, &ops_guard, &cfg);

        fdb_flash_sim_alloc_init(&sim_16k, 4U * 16384U, 16384U, 1U);
        fdb_flash_sim_as_ops(&sim_16k, &ops_16k);
        fdb_edge_tsdb_default_config(&cfg, 1U);
        cfg.max_block_bytes = 10240U;
        cfg.batch_size = 32U;
        ok_16k_open = fdb_edge_tsdb_open(&db_16k, &ops_16k, &cfg);
        for (i = 0; i < 64; i++) {
            double v = 10.0 + (double)i;
            fdb_edge_tsdb_append(&db_16k, 1700000000000LL + (int64_t)i * 1000LL, &v, 1U, NULL);
        }
        fdb_edge_tsdb_flush(&db_16k);
        fdb_edge_tsdb_query_range_mono(&db_16k, 1ULL, UINT64_MAX, edge_count_cb, &queried_16k);

        printf(
            "{\"issue_16_rollback\":{\"upstream_ok\":%d,\"upstream_dropped\":%d,"
            "\"edge_ok\":%zu,\"edge_queried\":%zu,\"edge_rollbacks\":%llu,\"edge_epoch\":%u},"
            "\"issue_249_capacity\":{\"upstream_4050_in_4096_append_rc\":%d,\"upstream_remain_underflowed\":%u,"
            "\"upstream_iter_count\":%zu,\"edge_oversize_rc\":%d,\"edge_10k_in_16k_open_rc\":%d,"
            "\"edge_10k_in_16k_queried\":%zu}}\n",
            up_ok_16, up_drop_16,
            edge_ok_16, edge_queried_16, (unsigned long long)est.rollback_events, est.current_epoch,
            up_cap_append_rc, (unsigned)up_cap_db.cur_sec.remain,
            up_cap_iter.count, guard_rc, ok_16k_open, queried_16k);

        fdb_flash_sim_free(&sim_guard);
        fdb_flash_sim_free(&sim_16k);
    }

    fdb_flash_sim_free(&sim_up);
    fdb_flash_sim_free(&sim_edge);
    fdb_flash_sim_free(&sim_up_cap);
    return 0;
}

/* ==================== CSV Benchmark Subcommand ==================== */
typedef struct {
    int64_t wall_ms;
    double values[FDB_EDGE_MAX_CHANNELS];
} csv_row_t;

static int read_csv_line(FILE *fp, char *buf, size_t cap)
{
    size_t len = 0U;
    int byte;

    while ((byte = fgetc(fp)) != EOF && byte != '\n') {
        if (byte == 0 || len == cap - 1U) {
            return -1;
        }
        buf[len++] = (char)byte;
    }
    if (ferror(fp)) {
        return -1;
    }
    if (byte == EOF && len == 0U) {
        return 0;
    }
    if (len > 0U && buf[len - 1U] == '\r') {
        len--;
    }
    buf[len] = '\0';
    return 1;
}

static bool parse_csv_double(const char **p, bool last, double *out)
{
    char *end;
    double value;

    errno = 0;
    value = strtod(*p, &end);
    if (end == *p || errno != 0 || !isfinite(value)) {
        return false;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end != (last ? '\0' : ',')) {
        return false;
    }
    *p = last ? end : end + 1;
    *out = value;
    return true;
}

static bool parse_csv_row(char *line, uint8_t nch, csv_row_t *row)
{
    const char *field;
    uint8_t ch;

    if (nch == 1U) {
        char *comma = strchr(line, ',');
        if (!comma) {
            return false;
        }
        *comma = '\0';
        if (!parse_iso_ts_ms(line, &row->wall_ms)) {
            return false;
        }
        field = comma + 1;
    } else {
        const int64_t epoch_ms = 1700000000000LL;
        const double signed_limit = 9223372036854775808.0;
        int64_t offset_ms;
        double seconds, rounded_ms;
        field = line;
        if (!parse_csv_double(&field, false, &seconds) || fabs(seconds) > signed_limit / 1000.0) {
            return false;
        }
        rounded_ms = round(seconds * 1000.0);
        if (rounded_ms < -signed_limit || rounded_ms >= signed_limit) {
            return false;
        }
        offset_ms = (int64_t)rounded_ms;
        if (offset_ms > INT64_MAX - epoch_ms) {
            return false;
        }
        row->wall_ms = epoch_ms + offset_ms;
    }

    for (ch = 0U; ch < nch; ch++) {
        if (!parse_csv_double(&field, ch == nch - 1U, &row->values[ch])) {
            return false;
        }
    }
    return true;
}

static bool sample_matches(const fdb_edge_sample_t *sample, int64_t wall_ms,
                           const double *values, uint8_t nch)
{
    return sample->wall_ts_ms == wall_ms && sample->num_channels == nch &&
        memcmp(sample->values, values, (size_t)nch * sizeof(values[0])) == 0;
}

typedef struct {
    const csv_row_t *expected;
    size_t expected_len;
    uint8_t num_channels;
    size_t visited;
    size_t matched;
    size_t mismatch;
    double max_abs_err;
} verify_query_ctx_t;

static bool verify_sample_cb(const fdb_edge_sample_t *s, void *arg)
{
    verify_query_ctx_t *ctx = (verify_query_ctx_t *)arg;
    size_t idx = ctx->visited++;
    uint8_t ch;

    if (idx >= ctx->expected_len || s->mono_seq != (uint64_t)idx + 1ULL) {
        ctx->mismatch++;
        return true;
    }
    if (sample_matches(s, ctx->expected[idx].wall_ms, ctx->expected[idx].values, ctx->num_channels)) {
        ctx->matched++;
        return true;
    }
    ctx->mismatch++;
    if (s->num_channels == ctx->num_channels) {
        for (ch = 0U; ch < ctx->num_channels; ch++) {
            double diff = fabs(s->values[ch] - ctx->expected[idx].values[ch]);
            if (!isfinite(diff) || diff > ctx->max_abs_err) {
                ctx->max_abs_err = diff;
            }
        }
    }
    return true;
}

typedef struct {
    struct fdb_tsdb *db;
    size_t rows_read;
    size_t byte_errors;
} up_bench_query_t;

static bool up_bench_cb(fdb_tsl_t tsl, void *arg)
{
    up_bench_query_t *q = (up_bench_query_t *)arg;
    double vals[FDB_EDGE_MAX_CHANNELS] = {0};
    struct fdb_blob blob;
    if (tsl->status == FDB_TSL_WRITE && tsl->log_len <= sizeof(vals)) {
        fdb_blob_make(&blob, vals, tsl->log_len);
        if (fdb_blob_read((fdb_db_t)q->db, fdb_tsl_to_blob(tsl, &blob)) == tsl->log_len) {
            q->rows_read++;
        } else {
            q->byte_errors++;
        }
    }
    return false;
}

static int cmd_bench_csv(int argc, char **argv)
{
    const char *csv_path = NULL;
    const char *mode = "nab";
    uint32_t sector_size = 4096U;
    uint32_t sectors = 512U;
    uint32_t total_size;
    uint16_t batch = 64U;
    int64_t rollback_at = -1;
    int i;

    FILE *fp;
    char line[2048];
    size_t row_cap = 1024U;
    csv_row_t *rows;
    size_t row_cnt = 0U;
    size_t line_number = 1U;
    uint8_t nch;
    int line_status;

    for (i = 0; i < argc; i += 2) {
        const char *opt = argv[i];
        const char *value = i + 1 < argc ? argv[i + 1] : "";
        uint64_t parsed;

        if (strcmp(opt, "--csv") == 0 && value[0] != '\0') {
            csv_path = value;
        } else if (strcmp(opt, "--mode") == 0 && value[0] != '\0') {
            mode = value;
        } else if (strcmp(opt, "--sector-size") == 0 && parse_uint(value, 0, UINT32_MAX, &parsed)) {
            sector_size = (uint32_t)parsed;
        } else if (strcmp(opt, "--sectors") == 0 && parse_uint(value, 0, UINT32_MAX, &parsed)) {
            sectors = (uint32_t)parsed;
        } else if (strcmp(opt, "--batch") == 0 && parse_uint(value, 0, FDB_EDGE_MAX_BLOCK_SAMPLES, &parsed)) {
            batch = (uint16_t)parsed;
        } else if (strcmp(opt, "--rollback-at") == 0 && strcmp(value, "-1") == 0) {
            rollback_at = -1;
        } else if (strcmp(opt, "--rollback-at") == 0 && parse_uint(value, 10, INT64_MAX, &parsed)) {
            rollback_at = (int64_t)parsed;
        } else {
            fprintf(stderr, "Invalid bench-csv option: %s\n", opt);
            return 2;
        }
    }

    if (!csv_path) {
        fprintf(stderr, "Missing --csv\n");
        return 2;
    }
    if (strcmp(mode, "nab") == 0) {
        nch = 1U;
    } else if (strcmp(mode, "imu") == 0 && FDB_EDGE_MAX_CHANNELS >= 9U) {
        nch = 9U;
    } else {
        fprintf(stderr, "Unsupported CSV mode: %s\n", mode);
        return 2;
    }
    if (sectors < 2U || sectors > FDB_EDGE_MAX_TRACKED_SECTORS ||
        sector_size < 256U || sector_size > INT_MAX || (sector_size & (sector_size - 1U)) != 0U ||
        sectors >= UINT32_MAX / sector_size) {
        fprintf(stderr, "Invalid bench-csv flash geometry\n");
        return 2;
    }
    total_size = sectors * sector_size;

    fp = fopen(csv_path, "rb");
    if (!fp) {
        fprintf(stderr, "Cannot open CSV: %s\n", csv_path);
        return 3;
    }

    rows = (csv_row_t *)malloc(row_cap * sizeof(*rows));
    if (!rows) {
        fclose(fp);
        return 4;
    }

    if (read_csv_line(fp, line, sizeof(line)) != 1 || line[0] == '\0') {
        fprintf(stderr, "Missing or invalid CSV header\n");
        free(rows);
        fclose(fp);
        return 5;
    }

    while ((line_status = read_csv_line(fp, line, sizeof(line))) == 1) {
        csv_row_t r = {0};
        line_number++;
        if (line[0] == '\0') {
            continue;
        }
        if (!parse_csv_row(line, nch, &r)) {
            line_status = -1;
            break;
        }
        if (rollback_at >= 0 && (uint64_t)row_cnt >= (uint64_t)rollback_at) {
            if (r.wall_ms < INT64_MIN + 3600000LL) {
                line_status = -1;
                break;
            }
            r.wall_ms -= 3600000LL;
        }

        if (row_cnt == row_cap) {
            csv_row_t *grown = NULL;
            if (row_cap <= SIZE_MAX / sizeof(*rows) / 2U) {
                row_cap *= 2U;
                grown = (csv_row_t *)realloc(rows, row_cap * sizeof(*rows));
            }
            if (!grown) {
                fprintf(stderr, "Cannot grow benchmark row buffer\n");
                free(rows);
                fclose(fp);
                return 6;
            }
            rows = grown;
        }

        rows[row_cnt++] = r;
    }
    if (fclose(fp) != 0 || line_status < 0 || row_cnt == 0U) {
        fprintf(stderr, "Invalid or empty benchmark CSV at or after line %zu\n", line_number);
        free(rows);
        return 5;
    }

    /* ---- Run Upstream Baseline ---- */
    fdb_flash_sim_t sim_up;
    fdb_flash_sim_stats_t up_flash_stats;
    struct fdb_tsdb up_db;
    size_t up_ok = 0U, up_dropped = 0U;
    uint64_t up_write_us, up_query_us;
    up_bench_query_t up_q;

    if (fdb_flash_sim_alloc_init(&sim_up, total_size, sector_size, 1U) != FDB_EDGE_OK) {
        fprintf(stderr, "Cannot initialize upstream benchmark flash\n");
        free(rows);
        return 7;
    }
    memset(&up_db, 0, sizeof(up_db));
    fdb_upstream_bind_sim(&sim_up);
    if (fdb_tsdb_init(&up_db, "up_bench", "tsdb_sim", up_get_time, (size_t)nch * sizeof(double) + 16U, NULL) != FDB_NO_ERR) {
        fprintf(stderr, "Cannot open upstream benchmark database\n");
        fdb_upstream_bind_sim(NULL);
        fdb_flash_sim_free(&sim_up);
        free(rows);
        return 7;
    }

    {
        uint64_t t0 = now_us();
        size_t r;
        for (r = 0; r < row_cnt; r++) {
            struct fdb_blob blob;
            /* Upstream supports integer timestamps via fdb_time_t; pass milliseconds directly under 64-bit mode. */
            fdb_time_t ts = (fdb_time_t)rows[r].wall_ms;
            fdb_blob_make(&blob, (void *)rows[r].values, (size_t)nch * sizeof(double));
            if (fdb_tsl_append_with_ts(&up_db, &blob, ts) == FDB_NO_ERR) {
                up_ok++;
            } else {
                up_dropped++;
            }
        }
        up_write_us = now_us() - t0;

        up_q.db = &up_db;
        up_q.rows_read = 0U;
        up_q.byte_errors = 0U;
        t0 = now_us();
        fdb_tsl_iter(&up_db, up_bench_cb, &up_q);
        up_query_us = now_us() - t0;
    }
    fdb_tsdb_deinit(&up_db);
    fdb_upstream_bind_sim(NULL);
    fdb_flash_sim_get_stats(&sim_up, &up_flash_stats);
    fdb_flash_sim_free(&sim_up);

    /* ---- Run Enhanced HSR/FlashDB-Edge Engine ---- */
    fdb_flash_sim_t sim_edge;
    fdb_flash_ops_t ops_edge;
    fdb_flash_sim_stats_t edge_flash_stats;
    static fdb_edge_tsdb_t edge_db;
    fdb_edge_tsdb_config_t cfg;
    fdb_edge_tsdb_stats_t edge_stats;
    size_t edge_ok = 0U, edge_dropped = 0U;
    uint64_t edge_write_us, edge_query_us;
    verify_query_ctx_t vctx;

    if (fdb_flash_sim_alloc_init(&sim_edge, total_size, sector_size, 1U) != FDB_EDGE_OK) {
        fprintf(stderr, "Cannot initialize edge benchmark flash\n");
        free(rows);
        return 7;
    }
    fdb_flash_sim_as_ops(&sim_edge, &ops_edge);
    fdb_edge_tsdb_default_config(&cfg, nch);
    cfg.batch_size = batch;
    if (fdb_edge_tsdb_open(&edge_db, &ops_edge, &cfg) != FDB_EDGE_OK) {
        fprintf(stderr, "Cannot open edge benchmark database\n");
        goto edge_failure;
    }

    {
        uint64_t t0 = now_us();
        size_t r;
        for (r = 0; r < row_cnt; r++) {
            fdb_edge_stamp_t stamp = {0};
            int rc = fdb_edge_tsdb_append(&edge_db, rows[r].wall_ms, rows[r].values, nch, &stamp);
            if (rc != FDB_EDGE_OK) {
                fprintf(stderr, "Edge append failed at CSV row %zu (%d)\n", r + 1U, rc);
                goto edge_failure;
            }
            if (stamp.mono_seq != 0ULL) {
                edge_ok++;
            } else {
                edge_dropped++;
            }
        }
        if (fdb_edge_tsdb_flush(&edge_db) != FDB_EDGE_OK || edge_db.stage_len != 0U) {
            fprintf(stderr, "Edge benchmark flush did not commit the staged rows\n");
            goto edge_failure;
        }
        edge_write_us = now_us() - t0;

        memset(&vctx, 0, sizeof(vctx));
        vctx.expected = rows;
        vctx.expected_len = row_cnt;
        vctx.num_channels = nch;
        t0 = now_us();
        if (fdb_edge_tsdb_query_range_mono(&edge_db, 0ULL, UINT64_MAX, verify_sample_cb, &vctx) != FDB_EDGE_OK) {
            fprintf(stderr, "Edge benchmark query failed\n");
            goto edge_failure;
        }
        if (vctx.visited < vctx.expected_len) {
            vctx.mismatch += vctx.expected_len - vctx.visited;
        }
        edge_query_us = now_us() - t0;
    }

    fdb_edge_tsdb_get_stats(&edge_db, &edge_stats);
    fdb_flash_sim_get_stats(&sim_edge, &edge_flash_stats);
    fdb_flash_sim_free(&sim_edge);
    free(rows);

    {
        char max_error[32] = "null";
        double reduction = (edge_flash_stats.total_bytes_written > 0ULL)
            ? (double)up_flash_stats.total_bytes_written / (double)edge_flash_stats.total_bytes_written
            : 0.0;
        if (isfinite(vctx.max_abs_err)) {
            snprintf(max_error, sizeof(max_error), "%.12g", vctx.max_abs_err);
        }
        printf(
            "{\"rows\":%zu,\"channels\":%u,\"flash_bytes\":%u,"
            "\"upstream\":{\"written_ok\":%zu,\"dropped\":%zu,\"queried\":%zu,"
            "\"flash_bytes_written\":%llu,\"erases\":%llu,\"nor_violations\":%llu,"
            "\"write_us\":%llu,\"query_us\":%llu},"
            "\"edge\":{\"written_ok\":%zu,\"dropped\":%zu,\"queried_matched\":%zu,\"mismatch\":%zu,"
            "\"flash_bytes_written\":%llu,\"erases\":%llu,\"nor_violations\":%llu,"
            "\"rollbacks\":%llu,\"write_us\":%llu,\"query_us\":%llu,"
            "\"bit_exact\":%s,\"max_abs_err\":%s},"
            "\"write_reduction_x\":%.4f}\n",
            row_cnt, (unsigned)nch, total_size,
            up_ok, up_dropped, up_q.rows_read,
            (unsigned long long)up_flash_stats.total_bytes_written,
            (unsigned long long)up_flash_stats.total_erases,
            (unsigned long long)up_flash_stats.nor_bit_violations,
            (unsigned long long)up_write_us,
            (unsigned long long)up_query_us,
            edge_ok, edge_dropped, vctx.matched, vctx.mismatch,
            (unsigned long long)edge_flash_stats.total_bytes_written,
            (unsigned long long)edge_flash_stats.total_erases,
            (unsigned long long)edge_flash_stats.nor_bit_violations,
            (unsigned long long)edge_stats.rollback_events,
            (unsigned long long)edge_write_us,
            (unsigned long long)edge_query_us,
            vctx.mismatch == 0U ? "true" : "false",
            max_error,
            reduction);
    }
    return 0;

edge_failure:
    fdb_flash_sim_free(&sim_edge);
    free(rows);
    return 7;
}

/* ==================== Power-Cut Torn-Write Fault Injection ==================== */
typedef struct {
    uint64_t count;
    uint64_t prior_count;
    uint64_t matched_prior;
    bool mismatch;
} fault_verify_ctx_t;

static void fault_values(uint64_t seq, double values[2])
{
    values[0] = 100.0 + (double)seq * 0.25;
    values[1] = -5.0 + (double)seq;
}

static int64_t fault_timestamp(uint64_t seq)
{
    return 1700000000000LL + (int64_t)seq * 100LL;
}

static bool fault_verify_cb(const fdb_edge_sample_t *s, void *arg)
{
    fault_verify_ctx_t *ctx = (fault_verify_ctx_t *)arg;
    double values[2];
    ctx->count++;
    fault_values(ctx->count, values);
    if (s->mono_seq != ctx->count || !sample_matches(s, fault_timestamp(ctx->count), values, 2U)) {
        ctx->mismatch = true;
    } else if (ctx->count <= ctx->prior_count) {
        ctx->matched_prior++;
    }
    return true;
}

static int fault_append(fdb_edge_tsdb_t *db, uint64_t seq)
{
    double values[2];
    fault_values(seq, values);
    return fdb_edge_tsdb_append(db, fault_timestamp(seq), values, 2U, NULL);
}

static bool run_fault_trial(uint32_t gran, uint32_t pre_blocks, uint32_t cut_bytes,
                            bool tear_bit, uint64_t *torn, fdb_flash_sim_stats_t *flash_stats,
                            bool *committed_loss)
{
    const uint32_t batch = 16U;
    uint64_t committed_before_cut = (uint64_t)pre_blocks * batch;
    uint64_t recovered_count;
    uint64_t seq;
    fdb_flash_sim_t sim;
    fdb_flash_ops_t ops;
    static fdb_edge_tsdb_t db;
    fdb_edge_tsdb_config_t cfg;
    fault_verify_ctx_t verify = {0};
    bool passed = false;
    int rc = FDB_EDGE_OK;

    *committed_loss = false;
    *torn = 0ULL;
    memset(flash_stats, 0, sizeof(*flash_stats));
    if (fdb_flash_sim_alloc_init(&sim, 8U * 4096U, 4096U, gran) != FDB_EDGE_OK) {
        return false;
    }
    fdb_flash_sim_as_ops(&sim, &ops);
    fdb_edge_tsdb_default_config(&cfg, 2U);
    cfg.batch_size = (uint16_t)batch;
    cfg.rollover = false;
    if (fdb_edge_tsdb_open(&db, &ops, &cfg) != FDB_EDGE_OK) {
        goto done;
    }
    for (seq = 1ULL; seq <= committed_before_cut; seq++) {
        if (fault_append(&db, seq) != FDB_EDGE_OK) {
            goto done;
        }
    }
    if (fdb_edge_tsdb_flush(&db) != FDB_EDGE_OK || db.stage_len != 0U ||
        db.flushed_samples != committed_before_cut) {
        goto done;
    }

    fdb_flash_sim_arm_power_cut(&sim, cut_bytes, tear_bit);
    for (; seq <= committed_before_cut + batch && rc == FDB_EDGE_OK; seq++) {
        rc = fault_append(&db, seq);
    }
    if (!sim.power_cut_tripped || (rc != FDB_EDGE_ERR_POWER_CUT && rc != FDB_EDGE_ERR_IO)) {
        goto done;
    }
    fdb_flash_sim_disarm_power_cut(&sim);

    if (fdb_edge_tsdb_open(&db, &ops, &cfg) != FDB_EDGE_OK) {
        goto done;
    }
    *torn = db.torn_blocks_recovered;
    verify.prior_count = committed_before_cut;
    if (fdb_edge_tsdb_query_range_mono(&db, 0ULL, UINT64_MAX, fault_verify_cb, &verify) != FDB_EDGE_OK) {
        goto done;
    }
    *committed_loss = verify.matched_prior != committed_before_cut;
    recovered_count = verify.count;
    if (verify.mismatch || db.hlc.monotonic_seq != recovered_count ||
        (recovered_count != committed_before_cut && recovered_count != committed_before_cut + batch)) {
        goto done;
    }

    /* Reopen again so the final query reads the newly committed sample from flash. */
    if (fault_append(&db, recovered_count + 1ULL) != FDB_EDGE_OK ||
        fdb_edge_tsdb_flush(&db) != FDB_EDGE_OK || db.stage_len != 0U ||
        fdb_edge_tsdb_open(&db, &ops, &cfg) != FDB_EDGE_OK) {
        goto done;
    }
    memset(&verify, 0, sizeof(verify));
    verify.prior_count = committed_before_cut;
    if (fdb_edge_tsdb_query_range_mono(&db, 0ULL, UINT64_MAX, fault_verify_cb, &verify) != FDB_EDGE_OK) {
        goto done;
    }
    *committed_loss = verify.matched_prior != committed_before_cut;
    passed = !verify.mismatch && verify.count == recovered_count + 1ULL &&
        db.hlc.monotonic_seq == verify.count;

done:
    fdb_flash_sim_get_stats(&sim, flash_stats);
    fdb_flash_sim_free(&sim);
    return passed && flash_stats->nor_bit_violations == 0ULL;
}

static int cmd_fault_inject(int argc, char **argv)
{
    uint32_t trials = 1000U;
    uint64_t seed = 42ULL;
    uint32_t t;
    uint32_t passed = 0U;
    uint32_t committed_loss_count = 0U;
    uint64_t total_torn = 0ULL;
    uint64_t total_nor_violations = 0ULL;
    uint64_t power_cuts_triggered = 0ULL;
    static const uint32_t gran_list[] = {1U, 4U, 8U, 32U};
    int i;

    for (i = 0; i < argc; i += 2) {
        const char *value = i + 1 < argc ? argv[i + 1] : "";
        uint64_t parsed;
        if (strcmp(argv[i], "--trials") == 0 && parse_uint(value, 0, UINT32_MAX, &parsed) && parsed > 0U) {
            trials = (uint32_t)parsed;
        } else if (strcmp(argv[i], "--seed") != 0 || !parse_uint(value, 0, UINT64_MAX, &seed)) {
            fprintf(stderr, "Invalid fault-inject option: %s\n", argv[i]);
            return 2;
        }
    }

    for (t = 0; t < trials; t++) {
        uint32_t gran = gran_list[t % (sizeof(gran_list) / sizeof(gran_list[0]))];
        uint32_t pre_blocks = 2U + (uint32_t)(xorshift64(&seed) % 8U);
        uint32_t cut_bytes = 4U + (uint32_t)(xorshift64(&seed) % 140U);
        bool tear_bit = (t / (sizeof(gran_list) / sizeof(gran_list[0])) % 2U) == 0U;
        uint64_t torn;
        fdb_flash_sim_stats_t fstats;
        bool committed_loss;

        if (run_fault_trial(gran, pre_blocks, cut_bytes, tear_bit, &torn, &fstats, &committed_loss)) {
            passed++;
        } else if (committed_loss) {
            committed_loss_count++;
        }
        total_torn += torn;
        total_nor_violations += fstats.nor_bit_violations;
        power_cuts_triggered += fstats.power_cuts_triggered;
    }

    printf(
        "{\"trials\":%u,\"passed\":%u,\"committed_loss_count\":%u,"
        "\"other_failure_count\":%u,\"power_cuts_triggered\":%llu,"
        "\"torn_tails_fenced\":%llu,\"nor_bit_violations\":%llu}\n",
        trials, passed, committed_loss_count, trials - passed - committed_loss_count,
        (unsigned long long)power_cuts_triggered,
        (unsigned long long)total_torn,
        (unsigned long long)total_nor_violations);
    return passed == trials ? 0 : 1;
}

/* ==================== STM32 -> Raspberry Pi Noisy WAL Sync ==================== */
static void sync_values(uint32_t index, double values[4])
{
    values[0] = 20.0 + 0.05 * (double)index;
    values[1] = sin((double)index * 0.1);
    values[2] = 100.0 + (double)(index % 17U);
    values[3] = -0.5 * (double)index;
}

static int64_t sync_timestamp(uint32_t index)
{
    return 1700000000000LL + (int64_t)index * 500LL;
}

typedef struct {
    uint32_t expected_samples;
    uint32_t verified;
    bool mismatch;
} sync_verify_ctx_t;

static bool sync_verify_cb(const fdb_edge_sample_t *sample, void *arg)
{
    sync_verify_ctx_t *ctx = (sync_verify_ctx_t *)arg;
    uint32_t index = ctx->verified;
    double values[4];

    if (index >= ctx->expected_samples || sample->mono_seq != (uint64_t)index + 1ULL) {
        ctx->mismatch = true;
        return false;
    }
    sync_values(index, values);
    if (sample->epoch != 0U || sample->logical_sub != 0U ||
        !sample_matches(sample, sync_timestamp(index), values, 4U)) {
        ctx->mismatch = true;
        return false;
    }
    ctx->verified++;
    return true;
}

static bool sync_verify(fdb_edge_tsdb_t *db, uint32_t samples)
{
    sync_verify_ctx_t ctx = {samples, 0U, false};
    return fdb_edge_tsdb_query_range_mono(db, 0ULL, UINT64_MAX, sync_verify_cb, &ctx) == FDB_EDGE_OK &&
        ctx.verified == samples && !ctx.mismatch;
}

static bool feed_sync_frame(fdb_wal_receiver_t *rx, const uint8_t *frame, size_t frame_len,
                            uint32_t samples, uint64_t *acked)
{
    uint8_t reply_buf[FDB_EDGE_MAX_FRAME_BYTES];
    size_t off;

    for (off = 0U; off < frame_len; off += 19U) {
        size_t chunk = frame_len - off > 19U ? 19U : frame_len - off;
        size_t reply_len = 0U;
        size_t pos = 0U;
        if (fdb_wal_receiver_feed(rx, frame + off, chunk, reply_buf, sizeof(reply_buf), &reply_len) != FDB_EDGE_OK) {
            return false;
        }
        while (pos < reply_len) {
            const uint8_t *delim = (const uint8_t *)memchr(reply_buf + pos, 0, reply_len - pos);
            fdb_wal_frame_hdr_t hdr;
            if (!delim ||
                fdb_wal_decode_frame(reply_buf + pos, (size_t)(delim - (reply_buf + pos)), &hdr, NULL, 0U) != FDB_EDGE_OK ||
                hdr.device_id != rx->expected_device_id || hdr.flags != 0U ||
                hdr.from_seq != hdr.to_seq || hdr.payload_len != 0U) {
                return false;
            }
            if (hdr.type == FDB_WAL_MSG_ACK) {
                if (hdr.to_seq == 0ULL || hdr.to_seq > samples) {
                    return false;
                }
                if (hdr.to_seq > *acked) {
                    *acked = hdr.to_seq;
                }
            } else if (hdr.type != FDB_WAL_MSG_NACK) {
                return false;
            }
            pos = (size_t)(delim - reply_buf) + 1U;
        }
    }
    return true;
}

static int cmd_sync_demo(int argc, char **argv)
{
    uint32_t samples = 2000U;
    uint32_t corrupt_pct = 5U;
    uint64_t seed = 7ULL;
    int i;

    fdb_flash_sim_t sim_src = {0}, sim_dst = {0};
    fdb_flash_ops_t ops_src, ops_dst;
    static fdb_edge_tsdb_t src_db, dst_db;
    fdb_edge_tsdb_config_t cfg;
    static fdb_wal_receiver_t rx;

    static uint8_t wire_buf[512U * 1024U];
    size_t wire_len = 0U;
    uint32_t frame_cnt = 0U;
    size_t pos = 0U;
    uint64_t injected_corruptions = 0ULL;
    uint64_t duplicate_replays = 0ULL;
    uint32_t attempts = 0U;
    uint64_t acked = 0ULL;
    bool synced_exact = false;

    for (i = 0; i < argc; i += 2) {
        const char *value = i + 1 < argc ? argv[i + 1] : "";
        uint64_t parsed;
        if (strcmp(argv[i], "--samples") == 0 && parse_uint(value, 0, UINT32_MAX, &parsed) && parsed > 0U) {
            samples = (uint32_t)parsed;
        } else if (strcmp(argv[i], "--corrupt-pct") == 0 && parse_uint(value, 0, 100U, &parsed)) {
            corrupt_pct = (uint32_t)parsed;
        } else if (strcmp(argv[i], "--seed") != 0 || !parse_uint(value, 0, UINT64_MAX, &seed)) {
            fprintf(stderr, "Invalid sync-demo option: %s\n", argv[i]);
            return 2;
        }
    }

    if (fdb_flash_sim_alloc_init(&sim_src, 64U * 4096U, 4096U, 1U) != FDB_EDGE_OK ||
        fdb_flash_sim_alloc_init(&sim_dst, 64U * 4096U, 4096U, 1U) != FDB_EDGE_OK) {
        fprintf(stderr, "Cannot initialize WAL sync flash\n");
        goto done;
    }
    fdb_flash_sim_as_ops(&sim_src, &ops_src);
    fdb_flash_sim_as_ops(&sim_dst, &ops_dst);
    fdb_edge_tsdb_default_config(&cfg, 4U);
    cfg.batch_size = 50U;
    cfg.rollover = false;
    if (fdb_edge_tsdb_open(&src_db, &ops_src, &cfg) != FDB_EDGE_OK ||
        fdb_edge_tsdb_open(&dst_db, &ops_dst, &cfg) != FDB_EDGE_OK) {
        fprintf(stderr, "Cannot open WAL sync database\n");
        goto done;
    }
    fdb_wal_receiver_init(&rx, &dst_db, 101U);

    for (uint32_t s = 0; s < samples; s++) {
        double v[4];
        sync_values(s, v);
        if (fdb_edge_tsdb_append(&src_db, sync_timestamp(s), v, 4U, NULL) != FDB_EDGE_OK) {
            fprintf(stderr, "WAL sync source append failed\n");
            goto done;
        }
    }
    if (fdb_edge_tsdb_flush(&src_db) != FDB_EDGE_OK || src_db.stage_len != 0U) {
        fprintf(stderr, "WAL sync source flush failed\n");
        goto done;
    }

    while (acked < (uint64_t)samples && attempts < 20U) {
        attempts++;
        wire_len = 0U;
        frame_cnt = 0U;
        if (fdb_wal_export_after(&src_db, 101U, acked, wire_buf, sizeof(wire_buf), &wire_len, &frame_cnt) != FDB_EDGE_OK) {
            fprintf(stderr, "WAL sync export failed\n");
            goto done;
        }
        if (wire_len == 0U) {
            break;
        }
        /* Split wire_buf into individual frames (0x00 terminated), optionally flip a byte or duplicate */
        pos = 0U;
        while (pos < wire_len) {
            size_t fend = pos;
            while (fend < wire_len && wire_buf[fend] != 0x00U) {
                fend++;
            }
            if (fend == wire_len) {
                fprintf(stderr, "WAL sync export returned an incomplete frame\n");
                goto done;
            }
            fend++;
            size_t flen = fend - pos;
            uint8_t tmp[FDB_EDGE_MAX_FRAME_BYTES];
            if (flen > sizeof(tmp)) {
                fprintf(stderr, "WAL sync export frame exceeds the wire limit\n");
                goto done;
            }
            memcpy(tmp, wire_buf + pos, flen);

            if (attempts == 1U && corrupt_pct > 0U && flen > 6U &&
                (xorshift64(&seed) % 100U) < corrupt_pct) {
                tmp[4] ^= 0x5AU;
                injected_corruptions++;
            }
            /* Feed in irregular UART chunks (e.g., 19 bytes at a time) */
            if (!feed_sync_frame(&rx, tmp, flen, samples, &acked)) {
                fprintf(stderr, "WAL sync receiver or reply validation failed\n");
                goto done;
            }
            /* Deliberately duplicate clean first frame on pass 1 to verify idempotent dedup */
            if (attempts == 1U && pos == 0U && flen > 6U) {
                duplicate_replays++;
                if (!feed_sync_frame(&rx, wire_buf + pos, flen, samples, &acked)) {
                    fprintf(stderr, "WAL sync replay validation failed\n");
                    goto done;
                }
            }
            pos = fend;
        }
    }

    {
        fdb_edge_tsdb_stats_t src_s, dst_s;
        fdb_edge_tsdb_get_stats(&src_db, &src_s);
        fdb_edge_tsdb_get_stats(&dst_db, &dst_s);
        synced_exact = acked == samples && rx.peer_last_seq == samples &&
            src_s.flushed_samples == samples && dst_s.flushed_samples == samples &&
            dst_db.stage_len == 0U && sync_verify(&src_db, samples) && sync_verify(&dst_db, samples);
        printf(
            "{\"samples\":%u,\"attempts\":%u,\"injected_corruptions\":%llu,"
            "\"duplicate_replays\":%llu,\"accepted_frames\":%llu,\"corrupt_frames\":%llu,"
            "\"duplicate_frames\":%llu,\"out_of_order_frames\":%llu,"
            "\"src_flushed\":%llu,\"dst_flushed\":%llu,\"synced_exact\":%s}\n",
            samples, attempts,
            (unsigned long long)injected_corruptions,
            (unsigned long long)duplicate_replays,
            (unsigned long long)rx.frames_accepted,
            (unsigned long long)rx.frames_corrupt,
            (unsigned long long)rx.frames_duplicate,
            (unsigned long long)rx.frames_out_of_order,
            (unsigned long long)src_s.flushed_samples,
            (unsigned long long)dst_s.flushed_samples,
            synced_exact ? "true" : "false");
    }

done:
    fdb_flash_sim_free(&sim_src);
    fdb_flash_sim_free(&sim_dst);
    return synced_exact ? 0 : 1;
}

/* ==================== OOD Privacy + DoS Benchmark ==================== */
static int cmd_privacy_bench(void)
{
    char phone1[32], phone2[32], pem1[128], pem2[128], key_label[32];
    char buf[512];
    fdb_edge_privacy_result_t res;
    int recall = 0;
    int benign_fp = 0;
    int dos_pass = 0;

    snprintf(phone1, sizeof(phone1), "phone:+86%s%s%s", "139", "1279", "5002");
    snprintf(phone2, sizeof(phone2), "from (%s%s%s)", "188", "8531", "7713");
    snprintf(key_label, sizeof(key_label), "%s %s", "EC", "PRIVATE KEY");
    snprintf(pem1, sizeof(pem1), "-----BEGIN %s-----\nRklYVFVSRQ==\n-----END %s-----", key_label, key_label);
    snprintf(pem2, sizeof(pem2), "-----BEGIN %s-----\r\nQUJDREVGRw==\r\n-----END %s-----", "RSA PRIVATE KEY", "RSA PRIVATE KEY");

    {
        const char *secrets[15] = {
            "imsi=460091827364510",
            "subscriber={\"imsi\":\"460201010738590\"}",
            "imei: 356789021654309",
            "device=351294087652310/1",
            phone1,
            phone2,
            "authorization: Bearer Az7_8b-xy.UV98920==",
            "\"Authorization\": \"Bearer 9.e\"",
            "stripe=sk_live_FAKEabcXY7210",
            "restricted rk_test_FAKE0123421",
            "google AIzaZZZZZZZ_fake_fixture_0123456789",
            "hf_FakeRepoABC156adksj2",
            pem1,
            "\"quic_session_ticket\":\"eE5VSjg5UDFRNTBzTg==\"",
            "postgresql+asyncpg://pool:fixture%2Fpass@db.invalid:5432/rail"
        };
        const char *tokens[15] = {
            "460091827364510",
            "460201010738590",
            "356789021654309",
            "351294087652310",
            "12795002",
            "85317713",
            "Az7_8b-xy.UV98920==",
            "9.e",
            "sk_live_FAKEabcXY7210",
            "rk_test_FAKE0123421",
            "AIzaZZZZZZZ_fake_fixture_0123456789",
            "hf_FakeRepoABC156adksj2",
            "RklYVFVSRQ==",
            "eE5VSjg5UDFRNTBzTg==",
            "fixture%2Fpass"
        };
        const char *benign[15] = {
            "E57A7F46-78B1-4EF1-8823-0413F3530399",
            "route 9998049239300031",
            "0000000000000000000000000000000000000000000000000000000000000000",
            "sha256 abcde1abcde1abcde1abcde1abcde1abcde1abcde1abcde1abcde1abcde14720",
            "2026-11-24T14:55:22.205Z",
            "2026-11-24 14:55:22+08:00",
            "1795272922",
            "1795272922000",
            "1795272922000971",
            "1795272922000971911",
            "Bearer certificates are optional.",
            "The STM32 node sampled the accelerometer before reset.",
            "build_id=hf_examples",
            "ticket=\"waiting\"",
            "postgres://db.invalid/db"
        };
        int i;
        for (i = 0; i < 15; i++) {
            int rc = fdb_edge_privacy_redact(secrets[i], strlen(secrets[i]), buf, sizeof(buf), &res);
            if (rc == FDB_EDGE_OK && res.redaction_count == 1U && strstr(buf, tokens[i]) == NULL) {
                recall++;
            }
        }
        for (i = 0; i < 15; i++) {
            int rc = fdb_edge_privacy_redact(benign[i], strlen(benign[i]), buf, sizeof(buf), &res);
            if (rc != FDB_EDGE_OK || !res.clean || strcmp(buf, benign[i]) != 0) {
                benign_fp++;
            }
        }
        (void)pem2;

        /* DoS check 1: reject > MAX_PRIVACY_BYTES */
        {
            static char huge[FDB_EDGE_MAX_PRIVACY_BYTES + 2U];
            memset(huge, 'A', sizeof(huge) - 1U);
            if (fdb_edge_privacy_redact(huge, FDB_EDGE_MAX_PRIVACY_BYTES + 1U, buf, sizeof(buf), &res) == FDB_EDGE_ERR_BOUNDS) {
                dos_pass++;
            }
        }
        /* DoS check 2: tight output buffer */
        if (fdb_edge_privacy_redact(secrets[0], strlen(secrets[0]), buf, 8U, &res) == FDB_EDGE_ERR_BOUNDS) {
            dos_pass++;
        }
        /* DoS check 3: oversized WAL frame rejection */
        {
            uint8_t zeros[FDB_EDGE_MAX_FRAME_BYTES];
            fdb_wal_frame_hdr_t hdr;
            memset(zeros, 0x01, sizeof(zeros));
            if (fdb_wal_decode_frame(zeros, FDB_EDGE_MAX_FRAME_BYTES, &hdr, NULL, 0U) == FDB_EDGE_ERR_CORRUPT) {
                dos_pass++;
            }
        }
        /* DoS check 4: capacity guard rejects oversized block for 4KB sector */
        {
            fdb_flash_sim_t sim;
            fdb_flash_ops_t ops;
            static fdb_edge_tsdb_t db;
            fdb_edge_tsdb_config_t cfg;
            fdb_flash_sim_alloc_init(&sim, 4U * 4096U, 4096U, 1U);
            fdb_flash_sim_as_ops(&sim, &ops);
            fdb_edge_tsdb_default_config(&cfg, 1U);
            cfg.max_block_bytes = 4096U;
            if (fdb_edge_tsdb_open(&db, &ops, &cfg) == FDB_EDGE_ERR_CAPACITY) {
                dos_pass++;
            }
            fdb_flash_sim_free(&sim);
        }
    }

    printf(
        "{\"secret_recall\":%d,\"secret_total\":15,\"benign_fp\":%d,\"benign_total\":15,\"dos_checks_passed\":%d,\"dos_checks_total\":4}\n",
        recall, benign_fp, dos_pass);
    return (recall == 15 && benign_fp == 0 && dos_pass == 4) ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: fdb_edge_cli <repro-upstream|bench-csv|fault-inject|sync-demo|privacy-bench> [args...]\n");
        return 2;
    }
    if (strcmp(argv[1], "repro-upstream") == 0) {
        return cmd_repro_upstream();
    }
    if (strcmp(argv[1], "bench-csv") == 0) {
        return cmd_bench_csv(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "fault-inject") == 0) {
        return cmd_fault_inject(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "sync-demo") == 0) {
        return cmd_sync_demo(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "privacy-bench") == 0) {
        return cmd_privacy_bench();
    }
    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    return 2;
}
