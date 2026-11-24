#ifndef FDB_EDGE_PRIVACY_H
#define FDB_EDGE_PRIVACY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FDB_PRIV_PRIVATE_KEY = 1,
    FDB_PRIV_DSN_PASSWORD = 2,
    FDB_PRIV_BEARER_TOKEN = 3,
    FDB_PRIV_API_KEY = 4,
    FDB_PRIV_SESSION_OR_PSK = 5,
    FDB_PRIV_IMSI = 6,
    FDB_PRIV_IMEI = 7,
    FDB_PRIV_MSISDN = 8
} fdb_privacy_cat_t;

typedef struct {
    size_t redaction_count;
    size_t out_len;
    uint32_t category_mask;
    bool clean;
} fdb_edge_privacy_result_t;

int fdb_edge_privacy_redact(
    const char *input,
    size_t input_len,
    char *out_buf,
    size_t out_cap,
    fdb_edge_privacy_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif /* FDB_EDGE_PRIVACY_H */
