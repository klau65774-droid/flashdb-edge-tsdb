#include "fdb_edge/fdb_edge_privacy.h"

#include <ctype.h>
#include <string.h>

#include "fdb_edge/fdb_edge_config.h"

static bool is_token_char(unsigned char c)
{
    return isalnum(c) || c == '_' || c == '.' || c == '~' || c == '+' || c == '/' || c == '-' || c == '=' || c == '%';
}

static bool is_word_sep_before(const char *s, size_t pos)
{
    if (pos == 0U) {
        return true;
    }
    unsigned char prev = (unsigned char)s[pos - 1U];
    return !(isalnum(prev) || prev == '_' || prev == '-');
}

static bool is_word_sep_after(const char *s, size_t len, size_t end)
{
    if (end >= len) {
        return true;
    }
    unsigned char nxt = (unsigned char)s[end];
    return !(isalnum(nxt) || nxt == '_' || nxt == '-');
}

static bool strncaseeq_ascii(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

static size_t pem_private_label_len(const char *s, size_t len, size_t pos)
{
    static const char begin[] = "-----BEGIN ";
    static const char suffix[] = "PRIVATE KEY";
    const size_t prefix_len = sizeof(begin) - 1U;
    const size_t suffix_len = sizeof(suffix) - 1U;
    const size_t max_label_len = 64U - prefix_len - 5U;
    size_t label_len;

    if (len - pos < prefix_len || memcmp(s + pos, begin, prefix_len) != 0) {
        return 0U;
    }
    pos += prefix_len;
    for (label_len = 0U; label_len <= max_label_len && len - pos - label_len >= 5U; label_len++) {
        unsigned char ch = (unsigned char)s[pos + label_len];
        if (ch == '-' && memcmp(s + pos + label_len, "-----", 5U) == 0) {
            if (label_len >= suffix_len &&
                (label_len == suffix_len || s[pos + label_len - suffix_len - 1U] == ' ') &&
                memcmp(s + pos + label_len - suffix_len, suffix, suffix_len) == 0) {
                return label_len;
            }
            break;
        }
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == '-')) {
            break;
        }
    }
    return 0U;
}

static size_t match_pem_block(const char *s, size_t len, size_t pos, size_t *resume)
{
    static const char end[] = "-----END ";
    const size_t begin_len = sizeof("-----BEGIN ") - 1U;
    const size_t end_len = sizeof(end) - 1U;
    size_t label_len = pem_private_label_len(s, len, pos);
    size_t search;
    size_t footer_len;

    if (label_len == 0U) {
        return 0U;
    }
    footer_len = end_len + label_len + 5U;
    for (search = pos + begin_len + label_len + 5U; search < len; search++) {
        if (len - search >= footer_len &&
            memcmp(s + search, end, end_len) == 0 &&
            memcmp(s + search + end_len, s + pos + begin_len, label_len) == 0 &&
            memcmp(s + search + end_len + label_len, "-----", 5U) == 0) {
            return search + footer_len - pos;
        }
        if (s[search] == '-' && pem_private_label_len(s, len, search) != 0U) {
            break;
        }
    }
    /* Leave unmatched text to the other matchers. The PEM scan resumes at the
     * next header, so no interval of the input is repeatedly searched. */
    *resume = search;
    return 0U;
}

static bool is_scheme_char(unsigned char ch)
{
    return isalnum(ch) || ch == '+' || ch == '.' || ch == '-';
}

static bool match_dsn_password(
    const char *s,
    size_t len,
    size_t pos,
    size_t *resume,
    size_t *out_secret_start,
    size_t *out_secret_end)
{
    size_t p = pos;
    size_t user_end;
    size_t pass_start;
    size_t pass_end;

    if (!isalpha((unsigned char)s[p])) {
        return false;
    }
    while (p < len && is_scheme_char((unsigned char)s[p])) {
        p++;
    }
    /* All later candidate schemes in this run share the same authority. */
    *resume = p;
    if (len - p < 3U || memcmp(s + p, "://", 3U) != 0) {
        return false;
    }
    p += 3U;
    user_end = p;
    while (user_end < len && s[user_end] != ':' && s[user_end] != '@' && s[user_end] != '/' &&
           !isspace((unsigned char)s[user_end])) {
        user_end++;
    }
    if (user_end >= len || s[user_end] != ':') {
        return false;
    }
    pass_start = user_end + 1U;
    pass_end = pass_start;
    while (pass_end < len && s[pass_end] != '@' && s[pass_end] != '/' &&
           !isspace((unsigned char)s[pass_end]) && s[pass_end] != '"' && s[pass_end] != '\'') {
        pass_end++;
    }
    if (pass_end <= pass_start || pass_end >= len || s[pass_end] != '@') {
        return false;
    }
    *out_secret_start = pass_start;
    *out_secret_end = pass_end;
    return true;
}

static bool match_bearer(
    const char *s,
    size_t len,
    size_t pos,
    size_t *out_secret_start,
    size_t *out_secret_end)
{
    size_t p;
    size_t tok_start;
    size_t tok_end;
    bool has_digit_or_sym = false;

    if (!is_word_sep_before(s, pos) || pos + 7U > len || !strncaseeq_ascii(s + pos, "bearer", 6U)) {
        return false;
    }
    p = pos + 6U;
    if (p >= len || (s[p] != ' ' && s[p] != '\t')) {
        return false;
    }
    while (p < len && (s[p] == ' ' || s[p] == '\t')) {
        p++;
    }
    tok_start = p;
    tok_end = p;
    while (tok_end < len && is_token_char((unsigned char)s[tok_end])) {
        unsigned char ch = (unsigned char)s[tok_end];
        if (isdigit(ch) || ch == '_' || ch == '-' || ch == '.' || ch == '/' || ch == '+' || ch == '=') {
            has_digit_or_sym = true;
        }
        tok_end++;
    }
    if ((tok_end - tok_start) < 3U || !has_digit_or_sym || !is_word_sep_after(s, len, tok_end)) {
        return false;
    }
    *out_secret_start = tok_start;
    *out_secret_end = tok_end;
    return true;
}

static size_t match_api_key(const char *s, size_t len, size_t pos)
{
    size_t p;
    size_t prefix_len = 0U;

    if (!is_word_sep_before(s, pos)) {
        return 0U;
    }
    if (pos + 16U <= len &&
        (memcmp(s + pos, "sk_live_", 8U) == 0 ||
         memcmp(s + pos, "sk_test_", 8U) == 0 ||
         memcmp(s + pos, "rk_live_", 8U) == 0 ||
         memcmp(s + pos, "rk_test_", 8U) == 0)) {
        prefix_len = 8U;
    } else if (pos + 24U <= len && memcmp(s + pos, "AIza", 4U) == 0) {
        prefix_len = 4U;
    } else if (pos + 15U <= len && memcmp(s + pos, "hf_", 3U) == 0) {
        size_t q = pos + 3U;
        if (q + 12U > len) {
            return 0U;
        }
        /* Require mixed case or digits for hf_ tokens so plain identifiers like hf_examples don't trigger. */
        while (q < len && isalnum((unsigned char)s[q])) {
            q++;
        }
        {
            size_t k;
            bool has_upper = false;
            bool has_digit = false;
            for (k = pos + 3U; k < q; k++) {
                if (isupper((unsigned char)s[k])) has_upper = true;
                if (isdigit((unsigned char)s[k])) has_digit = true;
            }
            if (q - (pos + 3U) >= 12U && (has_upper || has_digit) && is_word_sep_after(s, len, q)) {
                return q - pos;
            }
        }
        return 0U;
    } else {
        return 0U;
    }

    p = pos + prefix_len;
    while (p < len && (isalnum((unsigned char)s[p]) || s[p] == '_' || s[p] == '-')) {
        p++;
    }
    if (p - pos < prefix_len + 8U || !is_word_sep_after(s, len, p)) {
        return 0U;
    }
    return p - pos;
}

static bool match_kv_secret(
    const char *s,
    size_t len,
    size_t pos,
    size_t *out_secret_start,
    size_t *out_secret_end)
{
    static const char *const k_keys[] = {"quic_session_ticket", "session_ticket", "ticket", "wifi_pass", "psk"};
    size_t i;

    if (!is_word_sep_before(s, pos)) {
        return false;
    }
    for (i = 0; i < sizeof(k_keys) / sizeof(k_keys[0]); i++) {
        size_t klen = strlen(k_keys[i]);
        if (pos + klen < len && strncaseeq_ascii(s + pos, k_keys[i], klen)) {
            size_t p = pos + klen;
            size_t vstart;
            size_t vend;
            char quote = '\0';
            if (s[p] == '"' || s[p] == '\'') {
                p++;
            }
            while (p < len && (s[p] == ' ' || s[p] == '\t')) {
                p++;
            }
            if (p >= len || (s[p] != '=' && s[p] != ':')) {
                continue;
            }
            p++;
            while (p < len && (s[p] == ' ' || s[p] == '\t')) {
                p++;
            }
            if (p < len && (s[p] == '"' || s[p] == '\'')) {
                quote = s[p++];
            }
            vstart = p;
            vend = p;
            if (quote != '\0') {
                while (vend < len && s[vend] != quote && s[vend] != '\r' && s[vend] != '\n') {
                    if (s[vend] == '\\' && len - vend > 1U &&
                        (s[vend + 1U] == quote || s[vend + 1U] == '\\')) {
                        vend++;
                    }
                    vend++;
                }
                if (vend >= len || s[vend] != quote) {
                    quote = '\0';
                    vend = vstart;
                }
            }
            if (quote == '\0') {
                while (vend < len && is_token_char((unsigned char)s[vend])) {
                    vend++;
                }
            }
            {
                size_t min_len = (i >= 3U) ? 8U : 12U;
                if (vend - vstart >= min_len) {
                    *out_secret_start = vstart;
                    *out_secret_end = vend;
                    return true;
                }
            }
        }
    }
    return false;
}

static size_t count_digits_at(const char *s, size_t len, size_t pos)
{
    size_t p = pos;
    while (p < len && isdigit((unsigned char)s[p])) {
        p++;
    }
    return p - pos;
}

static int append_str(char *dst, size_t cap, size_t *out_pos, const char *src, size_t n)
{
    if (n >= cap - *out_pos) {
        return FDB_EDGE_ERR_BOUNDS;
    }
    if (n > 0U) {
        memcpy(dst + *out_pos, src, n);
        *out_pos += n;
    }
    return FDB_EDGE_OK;
}

int fdb_edge_privacy_redact(
    const char *input,
    size_t input_len,
    char *out_buf,
    size_t out_cap,
    fdb_edge_privacy_result_t *out_result)
{
    size_t i = 0U;
    size_t out_pos = 0U;
    size_t count = 0U;
    size_t pem_resume = 0U;
    size_t dsn_resume = 0U;
    uint32_t mask = 0U;

    if (!input || !out_buf || !out_result || out_cap == 0U) {
        return FDB_EDGE_ERR_INVALID;
    }
    if (input_len > FDB_EDGE_MAX_PRIVACY_BYTES) {
        return FDB_EDGE_ERR_BOUNDS;
    }

    while (i < input_len) {
        size_t match_len = 0U;
        size_t sec_s = 0U;
        size_t sec_e = 0U;

        if (i >= pem_resume && (match_len = match_pem_block(input, input_len, i, &pem_resume)) > 0U) {
            if (append_str(out_buf, out_cap, &out_pos, "[REDACTED_PRIVATE_KEY]", 22U) != FDB_EDGE_OK) {
                return FDB_EDGE_ERR_BOUNDS;
            }
            count++;
            mask |= (1U << FDB_PRIV_PRIVATE_KEY);
            i += match_len;
            continue;
        }
        if (i >= dsn_resume && match_dsn_password(input, input_len, i, &dsn_resume, &sec_s, &sec_e)) {
            if (append_str(out_buf, out_cap, &out_pos, input + i, sec_s - i) != FDB_EDGE_OK ||
                append_str(out_buf, out_cap, &out_pos, "[REDACTED_DSN_PASSWORD]", 23U) != FDB_EDGE_OK) {
                return FDB_EDGE_ERR_BOUNDS;
            }
            count++;
            mask |= (1U << FDB_PRIV_DSN_PASSWORD);
            i = sec_e;
            continue;
        }
        if (match_bearer(input, input_len, i, &sec_s, &sec_e)) {
            if (append_str(out_buf, out_cap, &out_pos, input + i, sec_s - i) != FDB_EDGE_OK ||
                append_str(out_buf, out_cap, &out_pos, "[REDACTED_BEARER]", 17U) != FDB_EDGE_OK) {
                return FDB_EDGE_ERR_BOUNDS;
            }
            count++;
            mask |= (1U << FDB_PRIV_BEARER_TOKEN);
            i = sec_e;
            continue;
        }
        if (match_kv_secret(input, input_len, i, &sec_s, &sec_e)) {
            if (append_str(out_buf, out_cap, &out_pos, input + i, sec_s - i) != FDB_EDGE_OK ||
                append_str(out_buf, out_cap, &out_pos, "[REDACTED_QUIC_TICKET]", 22U) != FDB_EDGE_OK) {
                return FDB_EDGE_ERR_BOUNDS;
            }
            count++;
            mask |= (1U << FDB_PRIV_SESSION_OR_PSK);
            i = sec_e;
            continue;
        }
        if ((match_len = match_api_key(input, input_len, i)) > 0U) {
            if (append_str(out_buf, out_cap, &out_pos, "[REDACTED_API_KEY]", 18U) != FDB_EDGE_OK) {
                return FDB_EDGE_ERR_BOUNDS;
            }
            count++;
            mask |= (1U << FDB_PRIV_API_KEY);
            i += match_len;
            continue;
        }

        /* Optional +86/- MSISDN prefix */
        if (is_word_sep_before(input, i) && i + 14U <= input_len &&
            memcmp(input + i, "+86", 3U) == 0) {
            size_t off = i + 3U;
            if (off < input_len && (input[off] == ' ' || input[off] == '-')) {
                off++;
            }
            if (off + 11U <= input_len && count_digits_at(input, input_len, off) == 11U &&
                input[off] == '1' && input[off + 1U] >= '3' && input[off + 1U] <= '9' &&
                is_word_sep_after(input, input_len, off + 11U)) {
                if (append_str(out_buf, out_cap, &out_pos, "[REDACTED_MSISDN]", 17U) != FDB_EDGE_OK) {
                    return FDB_EDGE_ERR_BOUNDS;
                }
                count++;
                mask |= (1U << FDB_PRIV_MSISDN);
                i = off + 11U;
                continue;
            }
        }

        if (is_word_sep_before(input, i) && isdigit((unsigned char)input[i])) {
            size_t dlen = count_digits_at(input, input_len, i);
            if (is_word_sep_after(input, input_len, i + dlen)) {
                if (dlen == 15U && memcmp(input + i, "460", 3U) == 0 &&
                    ((input[i + 3U] == '0' || input[i + 3U] == '1') ||
                     (input[i + 3U] == '2' && input[i + 4U] == '0'))) {
                    if (append_str(out_buf, out_cap, &out_pos, "[REDACTED_IMSI]", 15U) != FDB_EDGE_OK) {
                        return FDB_EDGE_ERR_BOUNDS;
                    }
                    count++;
                    mask |= (1U << FDB_PRIV_IMSI);
                    i += 15U;
                    continue;
                }
                if (dlen == 15U) {
                    if (append_str(out_buf, out_cap, &out_pos, "[REDACTED_IMEI]", 15U) != FDB_EDGE_OK) {
                        return FDB_EDGE_ERR_BOUNDS;
                    }
                    count++;
                    mask |= (1U << FDB_PRIV_IMEI);
                    i += 15U;
                    continue;
                }
                if (dlen == 11U && input[i] == '1' && input[i + 1U] >= '3' && input[i + 1U] <= '9') {
                    if (append_str(out_buf, out_cap, &out_pos, "[REDACTED_MSISDN]", 17U) != FDB_EDGE_OK) {
                        return FDB_EDGE_ERR_BOUNDS;
                    }
                    count++;
                    mask |= (1U << FDB_PRIV_MSISDN);
                    i += 11U;
                    continue;
                }
            }
        }

        if (append_str(out_buf, out_cap, &out_pos, input + i, 1U) != FDB_EDGE_OK) {
            return FDB_EDGE_ERR_BOUNDS;
        }
        i++;
    }

    out_buf[out_pos] = '\0';
    out_result->redaction_count = count;
    out_result->out_len = out_pos;
    out_result->category_mask = mask;
    out_result->clean = (count == 0U);
    return FDB_EDGE_OK;
}
