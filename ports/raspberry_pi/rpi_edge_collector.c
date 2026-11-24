#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#endif

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "fdb_edge/fdb_edge_tsdb.h"
#include "fdb_edge/fdb_edge_wal_sync.h"

typedef struct {
    FILE *fp;
    uint32_t total_size;
    uint32_t sector_size;
    bool io_failed;
    uint8_t scratch[1024];
} file_flash_ctx_t;

static int seek_file(FILE *fp, uint32_t offset, int whence)
{
#ifdef _WIN32
    return _fseeki64(fp, (int64_t)offset, whence);
#else
    return fseeko(fp, (off_t)offset, whence);
#endif
}

static int64_t tell_file(FILE *fp)
{
#ifdef _WIN32
    return _ftelli64(fp);
#else
    return (int64_t)ftello(fp);
#endif
}

static int sync_file(FILE *fp)
{
    if (fflush(fp) != 0) {
        return FDB_EDGE_ERR_IO;
    }
#ifdef _WIN32
    return _commit(_fileno(fp)) == 0 ? FDB_EDGE_OK : FDB_EDGE_ERR_IO;
#else
    return fsync(fileno(fp)) == 0 ? FDB_EDGE_OK : FDB_EDGE_ERR_IO;
#endif
}

static int sync_parent_directory(const char *path)
{
#ifdef _WIN32
    (void)path;
    return FDB_EDGE_OK;
#else
    const char *slash = strrchr(path, '/');
    int fd;
    int rc;

    if (!slash) {
        fd = open(".", O_RDONLY | O_DIRECTORY);
    } else {
        size_t len = slash == path ? 1U : (size_t)(slash - path);
        char *parent = malloc(len + 1U);
        if (!parent) {
            return FDB_EDGE_ERR_IO;
        }
        memcpy(parent, path, len);
        parent[len] = '\0';
        fd = open(parent, O_RDONLY | O_DIRECTORY);
        free(parent);
    }
    if (fd < 0) {
        return FDB_EDGE_ERR_IO;
    }
    rc = fsync(fd);
    if (close(fd) != 0 || rc != 0) {
        return FDB_EDGE_ERR_IO;
    }
    return FDB_EDGE_OK;
#endif
}

/* Compare open handles before truncating anything, including alias paths and hard links. */
static int same_file(FILE *first, FILE *second)
{
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION a;
    BY_HANDLE_FILE_INFORMATION b;
    if (!GetFileInformationByHandle((HANDLE)_get_osfhandle(_fileno(first)), &a) ||
        !GetFileInformationByHandle((HANDLE)_get_osfhandle(_fileno(second)), &b)) {
        return -1;
    }
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
        a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
#else
    struct stat a;
    struct stat b;
    if (fstat(fileno(first), &a) != 0 || fstat(fileno(second), &b) != 0) {
        return -1;
    }
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
#endif
}

static int truncate_file(FILE *fp)
{
#ifdef _WIN32
    return _chsize_s(_fileno(fp), 0) == 0 ? FDB_EDGE_OK : FDB_EDGE_ERR_IO;
#else
    return ftruncate(fileno(fp), 0) == 0 ? FDB_EDGE_OK : FDB_EDGE_ERR_IO;
#endif
}

static bool flash_has_range(const file_flash_ctx_t *f, uint32_t addr, size_t size)
{
    return addr <= f->total_size && size <= f->total_size - addr && size <= INT_MAX;
}

static int flash_failure(file_flash_ctx_t *f, int status)
{
    f->io_failed = true;
    return status;
}

static int file_flash_read(void *ctx, uint32_t addr, uint8_t *buf, size_t size)
{
    file_flash_ctx_t *f = (file_flash_ctx_t *)ctx;
    if (!flash_has_range(f, addr, size)) {
        return FDB_EDGE_ERR_BOUNDS;
    }
    if (seek_file(f->fp, addr, SEEK_SET) != 0 || fread(buf, 1U, size, f->fp) != size) {
        return flash_failure(f, FDB_EDGE_ERR_IO);
    }
    return (int)size;
}

static int file_flash_write(void *ctx, uint32_t addr, const uint8_t *buf, size_t size)
{
    file_flash_ctx_t *f = (file_flash_ctx_t *)ctx;
    size_t done = 0U;

    if (!flash_has_range(f, addr, size)) {
        return FDB_EDGE_ERR_BOUNDS;
    }
    while (done < size) {
        size_t chunk = size - done;
        size_t i;
        uint32_t pos = addr + (uint32_t)done;

        if (chunk > sizeof(f->scratch)) {
            chunk = sizeof(f->scratch);
        }
        if (seek_file(f->fp, pos, SEEK_SET) != 0 || fread(f->scratch, 1U, chunk, f->fp) != chunk) {
            return flash_failure(f, FDB_EDGE_ERR_IO);
        }
        for (i = 0U; i < chunk; i++) {
            if ((f->scratch[i] & buf[done + i]) != buf[done + i]) {
                return flash_failure(f, FDB_EDGE_ERR_NOR_VIOLATION);
            }
        }
        if (seek_file(f->fp, pos, SEEK_SET) != 0 || fwrite(buf + done, 1U, chunk, f->fp) != chunk) {
            return flash_failure(f, FDB_EDGE_ERR_IO);
        }
        done += chunk;
    }
    if (sync_file(f->fp) != FDB_EDGE_OK) {
        return flash_failure(f, FDB_EDGE_ERR_IO);
    }
    return (int)size;
}

static int write_erased(file_flash_ctx_t *f, uint32_t addr, size_t size)
{
    memset(f->scratch, 0xFF, sizeof(f->scratch));
    if (seek_file(f->fp, addr, SEEK_SET) != 0) {
        return flash_failure(f, FDB_EDGE_ERR_IO);
    }
    while (size > 0U) {
        size_t chunk = size;
        if (chunk > sizeof(f->scratch)) {
            chunk = sizeof(f->scratch);
        }
        if (fwrite(f->scratch, 1U, chunk, f->fp) != chunk) {
            return flash_failure(f, FDB_EDGE_ERR_IO);
        }
        size -= chunk;
    }
    if (sync_file(f->fp) != FDB_EDGE_OK) {
        return flash_failure(f, FDB_EDGE_ERR_IO);
    }
    return FDB_EDGE_OK;
}

static int file_flash_erase(void *ctx, uint32_t addr, size_t size)
{
    file_flash_ctx_t *f = (file_flash_ctx_t *)ctx;
    int rc;

    if (!flash_has_range(f, addr, size)) {
        return FDB_EDGE_ERR_BOUNDS;
    }
    if (size == 0U || addr % f->sector_size != 0U || size % f->sector_size != 0U) {
        return FDB_EDGE_ERR_INVALID;
    }
    rc = write_erased(f, addr, size);
    return rc == FDB_EDGE_OK ? (int)size : rc;
}

static int init_backing_file(
    file_flash_ctx_t *f, const char *path, uint32_t total_size, uint32_t sec_size, FILE *wal)
{
    bool fresh = false;

    f->fp = fopen(path, "r+b");
    if (!f->fp && errno == ENOENT) {
        f->fp = fopen(path, "w+bx");
        fresh = true;
    }
    if (!f->fp || same_file(f->fp, wal) != 0) {
        return FDB_EDGE_ERR_IO;
    }
    f->total_size = total_size;
    f->sector_size = sec_size;

    if (fresh) {
        if (write_erased(f, 0U, total_size) != FDB_EDGE_OK ||
            sync_parent_directory(path) != FDB_EDGE_OK) {
            return FDB_EDGE_ERR_IO;
        }
    } else if (seek_file(f->fp, 0U, SEEK_END) != 0 || tell_file(f->fp) != (int64_t)total_size) {
        return FDB_EDGE_ERR_BOUNDS;
    }
    return FDB_EDGE_OK;
}

static bool parse_uint32(const char *value, uint32_t limit, uint32_t *out)
{
    char *end;
    unsigned long long parsed;

    if (value[0] < '0' || value[0] > '9') {
        return false;
    }
    errno = 0;
    parsed = strtoull(value, &end, 0);
    if (errno == ERANGE || *end != '\0' || parsed > limit) {
        return false;
    }
    *out = (uint32_t)parsed;
    return true;
}

static int collect_wal(fdb_wal_receiver_t *rx, file_flash_ctx_t *flash, FILE *wal, FILE *ack)
{
    uint8_t io_buf[1024];
    uint8_t reply_buf[64];
    size_t nread;

    while ((nread = fread(io_buf, 1U, sizeof(io_buf), wal)) > 0U) {
        size_t pos = 0U;
        while (pos < nread) {
            const uint8_t *delim = memchr(io_buf + pos, 0, nread - pos);
            size_t end = delim ? (size_t)(delim - io_buf) + 1U : nread;
            size_t reply_len = 0U;
            int rc = fdb_wal_receiver_feed(
                rx, io_buf + pos, end - pos, ack ? reply_buf : NULL,
                sizeof(reply_buf), &reply_len);
            if (rc != FDB_EDGE_OK || flash->io_failed ||
                (ack && fwrite(reply_buf, 1U, reply_len, ack) != reply_len)) {
                return FDB_EDGE_ERR_IO;
            }
            pos = end;
        }
    }
    if (ferror(wal)) {
        return FDB_EDGE_ERR_IO;
    }
    return rx->rx_len == 0U && !rx->overflowed ? FDB_EDGE_OK : FDB_EDGE_ERR_CORRUPT;
}

int main(int argc, char **argv)
{
    const char *wal_in = NULL;
    const char *db_img = "rpi_edge_tsdb.bin";
    const char *ack_out = NULL;
    uint32_t device_id = 1U;
    uint32_t sectors = 128U;
    uint32_t sector_size = FDB_EDGE_DEFAULT_SECTOR_SIZE;
    uint32_t channels = 4U;
    uint32_t total_size;
    int i;
    int status = 0;

    file_flash_ctx_t fctx = {0};
    fdb_flash_ops_t ops;
    fdb_edge_tsdb_config_t cfg;
    static fdb_edge_tsdb_t db;
    static fdb_wal_receiver_t rx;
    FILE *wal = NULL;
    FILE *ack = NULL;
    fdb_edge_tsdb_stats_t stats;

    for (i = 1; i < argc; i++) {
        const char *option = argv[i];
        const char *value;
        bool valid;

        if (strcmp(option, "--help") == 0) {
            puts("Usage: rpi_edge_collector --wal-in <file> --db-img <file> [--ack-out <file>] "
                 "[--device-id N] [--sectors N] [--sector-size B] [--channels N]");
            return 0;
        }
        if (i + 1 >= argc || argv[i + 1][0] == '\0' || strncmp(argv[i + 1], "--", 2U) == 0) {
            fprintf(stderr, "Error: missing value for %s\n", option);
            return 2;
        }
        value = argv[++i];
        valid = true;
        if (strcmp(option, "--wal-in") == 0) {
            wal_in = value;
        } else if (strcmp(option, "--db-img") == 0) {
            db_img = value;
        } else if (strcmp(option, "--ack-out") == 0) {
            ack_out = value;
        } else if (strcmp(option, "--device-id") == 0) {
            valid = parse_uint32(value, UINT32_MAX, &device_id);
        } else if (strcmp(option, "--sectors") == 0) {
            valid = parse_uint32(value, FDB_EDGE_MAX_TRACKED_SECTORS, &sectors);
        } else if (strcmp(option, "--sector-size") == 0) {
            valid = parse_uint32(value, INT_MAX, &sector_size);
        } else if (strcmp(option, "--channels") == 0) {
            valid = parse_uint32(value, FDB_EDGE_MAX_CHANNELS, &channels);
        } else {
            valid = false;
        }
        if (!valid) {
            fprintf(stderr, "Error: invalid option or value for %s\n", option);
            return 2;
        }
    }

    if (!wal_in) {
        fprintf(stderr, "Error: --wal-in is required\n");
        return 2;
    }
    if (sectors < 2U || sectors > FDB_EDGE_MAX_TRACKED_SECTORS ||
        sector_size < 256U || sector_size > INT_MAX || (sector_size & (sector_size - 1U)) != 0U ||
        sectors > UINT32_MAX / sector_size || channels == 0U ||
        channels > FDB_EDGE_MAX_CHANNELS || channels > UINT8_MAX) {
        fprintf(stderr, "Error: invalid flash geometry or channel count\n");
        return 2;
    }
    total_size = sectors * sector_size;

    wal = fopen(wal_in, "rb");
    if (!wal) {
        fprintf(stderr, "Error: cannot open WAL input %s\n", wal_in);
        return 5;
    }
    if (init_backing_file(&fctx, db_img, total_size, sector_size, wal) != FDB_EDGE_OK) {
        fprintf(stderr, "Error: cannot use backing image %s with the requested geometry\n", db_img);
        status = 3;
        goto cleanup;
    }
    ops.ctx = &fctx;
    ops.total_size = total_size;
    ops.sector_size = sector_size;
    ops.write_gran = 1U;
    ops.read = file_flash_read;
    ops.write = file_flash_write;
    ops.erase = file_flash_erase;

    fdb_edge_tsdb_default_config(&cfg, (uint8_t)channels);
    if (fdb_edge_tsdb_open(&db, &ops, &cfg) != FDB_EDGE_OK || fctx.io_failed) {
        fprintf(stderr, "Error: fdb_edge_tsdb_open failed\n");
        status = 4;
        goto cleanup;
    }
    if (ack_out) {
        ack = fopen(ack_out, "ab");
        if (!ack || same_file(ack, wal) != 0 || same_file(ack, fctx.fp) != 0 ||
            truncate_file(ack) != FDB_EDGE_OK) {
            fprintf(stderr, "Error: cannot use ACK output %s\n", ack_out);
            status = 6;
            goto cleanup;
        }
    }

    fdb_wal_receiver_init(&rx, &db, device_id);
    if (collect_wal(&rx, &fctx, wal, ack) != FDB_EDGE_OK) {
        fprintf(stderr, "Error: incomplete WAL stream or collector I/O failure\n");
        status = 7;
    }
    if (ack && (sync_file(ack) != FDB_EDGE_OK || sync_parent_directory(ack_out) != FDB_EDGE_OK)) {
        fprintf(stderr, "Error: cannot persist ACK output %s\n", ack_out);
        status = 7;
    }

cleanup:
    if (ack && fclose(ack) != 0 && status == 0) {
        fprintf(stderr, "Error: cannot close ACK output\n");
        status = 7;
    }
    if (fctx.fp && fclose(fctx.fp) != 0 && status == 0) {
        fprintf(stderr, "Error: cannot close backing image\n");
        status = 7;
    }
    if (fclose(wal) != 0 && status == 0) {
        fprintf(stderr, "Error: cannot close WAL input\n");
        status = 7;
    }
    if (status != 0) {
        return status;
    }
    fdb_edge_tsdb_get_stats(&db, &stats);
    printf(
        "{\"status\":\"ok\",\"device_id\":%u,\"accepted_frames\":%llu,\"corrupt_frames\":%llu,"
        "\"duplicate_frames\":%llu,\"out_of_order_frames\":%llu,\"peer_last_seq\":%llu,"
        "\"total_samples\":%llu,\"committed_blocks\":%llu}\n",
        device_id,
        (unsigned long long)rx.frames_accepted,
        (unsigned long long)rx.frames_corrupt,
        (unsigned long long)rx.frames_duplicate,
        (unsigned long long)rx.frames_out_of_order,
        (unsigned long long)rx.peer_last_seq,
        (unsigned long long)stats.total_samples,
        (unsigned long long)stats.committed_blocks);
    return 0;
}
