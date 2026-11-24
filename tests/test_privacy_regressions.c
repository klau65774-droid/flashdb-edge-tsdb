#include <stdio.h>
#include <string.h>

#include "fdb_edge/fdb_edge_config.h"
#include "fdb_edge/fdb_edge_privacy.h"
#include "test_common.h"

static void pem_boundaries(void)
{
    char input[512], out[512], first[32], second[32];
    fdb_edge_privacy_result_t result;
    int len;

    snprintf(first, sizeof(first), "%s %s", "RSA", "PRIVATE KEY");
    snprintf(second, sizeof(second), "%s", "PRIVATE KEY");
    len = snprintf(input, sizeof(input),
                   "-----BEGIN %s-----\r\nfixture-one\r\n-----END %s-----"
                   "-----BEGIN %s-----\nfixture-two\n-----END %s-----",
                   first, first, second, second);
    CHECK(len > 0 && (size_t)len < sizeof(input));
    CHECK(fdb_edge_privacy_redact(input, (size_t)len, out, sizeof(out), &result) == FDB_EDGE_OK);
    CHECK(strcmp(out, "[REDACTED_PRIVATE_KEY][REDACTED_PRIVATE_KEY]") == 0);
    CHECK(result.redaction_count == 2U);
    CHECK(result.category_mask == (1U << FDB_PRIV_PRIVATE_KEY));

    len = snprintf(input, sizeof(input),
                   "-----BEGIN %s-----\n"
                   "https://u:fixture-pass@db.invalid\n"
                   "-----END %s-----\n"
                   "-----BEGIN %s-----\nfixture-three\n-----END %s-----",
                   first, second, second, second);
    CHECK(len > 0 && (size_t)len < sizeof(input));
    CHECK(fdb_edge_privacy_redact(input, (size_t)len, out, sizeof(out), &result) == FDB_EDGE_OK);
    CHECK(result.redaction_count == 2U);
    CHECK(result.category_mask == ((1U << FDB_PRIV_PRIVATE_KEY) | (1U << FDB_PRIV_DSN_PASSWORD)));
    CHECK(strstr(out, "https://u:[REDACTED_DSN_PASSWORD]@db.invalid") != NULL);
    CHECK(strstr(out, "fixture-three") == NULL);
}

static void adjacent_credentials(void)
{
    const char input[] = "Bearer ab-1,sk_test_mockToken123 wifi_pass=\"dummy \\\"quoted\\\" value\""
                         " postgres+tls://u:dummy%2Fkey@db.invalid";
    const char expected[] = "Bearer [REDACTED_BEARER],[REDACTED_API_KEY] wifi_pass=\"[REDACTED_QUIC_TICKET]\""
                            " postgres+tls://u:[REDACTED_DSN_PASSWORD]@db.invalid";
    char out[sizeof(expected)];
    fdb_edge_privacy_result_t result;

    CHECK(fdb_edge_privacy_redact(input, sizeof(input) - 1U, out, sizeof(out), &result) == FDB_EDGE_OK);
    CHECK(strcmp(out, expected) == 0);
    CHECK(result.out_len == sizeof(expected) - 1U && result.redaction_count == 4U);
    CHECK(result.category_mask == ((1U << FDB_PRIV_BEARER_TOKEN) | (1U << FDB_PRIV_API_KEY) |
                                   (1U << FDB_PRIV_SESSION_OR_PSK) | (1U << FDB_PRIV_DSN_PASSWORD)));
    CHECK(fdb_edge_privacy_redact(input, sizeof(input) - 1U, out, sizeof(out) - 1U, &result) ==
          FDB_EDGE_ERR_BOUNDS);
    {
        const char incomplete[] = "psk=\"dummyToken123";
        CHECK(fdb_edge_privacy_redact(incomplete, sizeof(incomplete) - 1U, out, sizeof(out), &result) ==
              FDB_EDGE_OK);
        CHECK(strcmp(out, "psk=\"[REDACTED_QUIC_TICKET]") == 0);
    }
}

static void max_size_input(void)
{
    static char input[FDB_EDGE_MAX_PRIVACY_BYTES];
    static char out[sizeof(input) + 1U];
    fdb_edge_privacy_result_t result;
    int header_size = snprintf(input, sizeof(input), "-----BEGIN %s-----\n", "PRIVATE KEY");

    CHECK(header_size > 0 && (size_t)header_size < sizeof(input));
    memset(input + header_size, 'a', sizeof(input) - (size_t)header_size);
    CHECK(fdb_edge_privacy_redact(input, sizeof(input), out, sizeof(out), &result) == FDB_EDGE_OK);
    CHECK(result.clean && result.out_len == sizeof(input));
    CHECK(memcmp(out, input, sizeof(input)) == 0 && out[sizeof(input)] == '\0');
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (strcmp(argv[1], "pem_boundaries") == 0) {
        pem_boundaries();
    } else if (strcmp(argv[1], "adjacent_credentials") == 0) {
        adjacent_credentials();
    } else {
        CHECK(strcmp(argv[1], "max_size") == 0);
        max_size_input();
    }
    return 0;
}
