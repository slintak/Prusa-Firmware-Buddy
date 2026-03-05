#include "jwt.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include <mbedtls/base64.h>

#define JSMN_HEADER
#include <jsmn.h>

namespace buddy::oauth::jwt {

namespace {
constexpr size_t MAX_PAYLOAD_B64 = 2048;
constexpr size_t MAX_PAYLOAD_JSON = 2048;
constexpr size_t TOKENS_COUNT = 64;

size_t next_token_index(const jsmntok_t *tokens, size_t cnt, size_t idx) {
    if (idx >= cnt) {
        return idx;
    }
    const jsmntok_t &tok = tokens[idx];
    size_t next = idx + 1;
    if (tok.type == JSMN_OBJECT) {
        for (int i = 0; i < tok.size * 2 && next < cnt; ++i) {
            next = next_token_index(tokens, cnt, next);
        }
    } else if (tok.type == JSMN_ARRAY) {
        for (int i = 0; i < tok.size && next < cnt; ++i) {
            next = next_token_index(tokens, cnt, next);
        }
    }
    return next;
}

bool token_equals(const char *json, const jsmntok_t &tok, const char *key) {
    const size_t key_len = strlen(key);
    const size_t tok_len = tok.end > tok.start ? static_cast<size_t>(tok.end - tok.start) : 0;
    return tok.type == JSMN_STRING && tok_len == key_len && strncmp(json + tok.start, key, key_len) == 0;
}

bool json_get_string(const char *json, const jsmntok_t *tokens, size_t cnt, const char *key, char *out, size_t out_len) {
    if (cnt == 0 || tokens[0].type != JSMN_OBJECT) {
        return false;
    }
    size_t idx = 1;
    for (int i = 0; i < tokens[0].size && idx + 1 < cnt; ++i) {
        const auto &k = tokens[idx];
        const auto &v = tokens[idx + 1];
        if (token_equals(json, k, key)) {
            const int len = std::max(0, v.end - v.start);
            if (static_cast<size_t>(len) >= out_len) {
                return false;
            }
            memcpy(out, json + v.start, static_cast<size_t>(len));
            out[len] = '\0';
            return true;
        }
        idx = next_token_index(tokens, cnt, idx + 1);
    }
    return false;
}

bool set_error(Error *error, Error value) {
    if (error) {
        *error = value;
    }
    return false;
}
} // namespace

const char *to_str(Error error) {
    switch (error) {
    case Error::None:
        return "none";
    case Error::InvalidFormat:
        return "invalid_format";
    case Error::DecodeFailed:
        return "decode_failed";
    case Error::ParseFailed:
        return "parse_failed";
    case Error::MissingClaim:
        return "missing_claim";
    default:
        return "unknown";
    }
}

bool extract_mqtt_username(const char *jwt_token, char *out, size_t out_len, Error *error) {
    if (error) {
        *error = Error::None;
    }
    if (jwt_token == nullptr || out == nullptr || out_len == 0) {
        return set_error(error, Error::InvalidFormat);
    }

    const char *dot1 = strchr(jwt_token, '.');
    if (dot1 == nullptr) {
        return set_error(error, Error::InvalidFormat);
    }
    const char *dot2 = strchr(dot1 + 1, '.');
    if (dot2 == nullptr || dot2 == dot1 + 1) {
        return set_error(error, Error::InvalidFormat);
    }

    const size_t payload_len = static_cast<size_t>(dot2 - (dot1 + 1));
    if (payload_len == 0 || payload_len >= MAX_PAYLOAD_B64) {
        return set_error(error, Error::DecodeFailed);
    }

    std::array<unsigned char, MAX_PAYLOAD_B64> payload_b64 {};
    memcpy(payload_b64.data(), dot1 + 1, payload_len);
    for (size_t i = 0; i < payload_len; ++i) {
        if (payload_b64[i] == '-') {
            payload_b64[i] = '+';
        } else if (payload_b64[i] == '_') {
            payload_b64[i] = '/';
        }
    }

    size_t padded_len = payload_len;
    while ((padded_len % 4) != 0 && padded_len + 1 < payload_b64.size()) {
        payload_b64[padded_len++] = '=';
    }
    payload_b64[padded_len] = '\0';

    std::array<unsigned char, MAX_PAYLOAD_JSON> payload_json {};
    size_t decoded_len = 0;
    if (mbedtls_base64_decode(payload_json.data(), payload_json.size() - 1, &decoded_len, payload_b64.data(), padded_len) != 0) {
        return set_error(error, Error::DecodeFailed);
    }
    payload_json[decoded_len] = '\0';

    jsmn_parser parser;
    jsmn_init(&parser);
    std::array<jsmntok_t, TOKENS_COUNT> tokens {};
    const int count = jsmn_parse(&parser, reinterpret_cast<const char *>(payload_json.data()), decoded_len, tokens.data(), tokens.size());
    if (count < 1) {
        return set_error(error, Error::ParseFailed);
    }

    if (json_get_string(reinterpret_cast<const char *>(payload_json.data()), tokens.data(), count, "mqtt_username", out, out_len)) {
        return true;
    }
    if (json_get_string(reinterpret_cast<const char *>(payload_json.data()), tokens.data(), count, "device_id", out, out_len)) {
        return true;
    }
    return set_error(error, Error::MissingClaim);
}

} // namespace buddy::oauth::jwt
