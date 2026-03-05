#pragma once

#include <cstddef>

namespace buddy::oauth::jwt {

enum class Error {
    None,
    InvalidFormat,
    DecodeFailed,
    ParseFailed,
    MissingClaim,
};

const char *to_str(Error error);

bool extract_mqtt_username(const char *jwt_token, char *out, size_t out_len, Error *error = nullptr);

} // namespace buddy::oauth::jwt
