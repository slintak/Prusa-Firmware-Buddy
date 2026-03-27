#pragma once

#include <cstddef>
#include <cstdint>

namespace buddy::oauth {

struct DeviceFlowConfig {
    const char *device_auth_url = nullptr;
    const char *token_url = nullptr;
    const char *client_id = nullptr;
    const char *scope = "mqtt";
    const char *serial_number = nullptr;
    bool custom_cert = false;
    uint8_t max_poll_attempts = 60;
    uint32_t initial_poll_interval_s = 5;
    uint32_t poll_timeout_s = 30 * 60;
};

struct DeviceCode {
    char device_code[128] = {};
    char user_code[64] = {};
    char verification_uri[192] = {};
    uint32_t interval_s = 5;
};

struct Tokens {
    char access_token[1536] = {};
    char refresh_token[1536] = {};
    uint32_t expires_in_s = 0;
};

enum class Error {
    None,
    InvalidConfig,
    InvalidUrl,
    HttpError,
    ParseError,
    MissingField,
    AuthorizationDenied,
    Canceled,
    PollingTimeout,
    ResponseError,
};

const char *to_str(Error error);

bool request_device_code(const DeviceFlowConfig &cfg, DeviceCode &out, Error *error = nullptr);
bool poll_tokens(const DeviceFlowConfig &cfg, const DeviceCode &device_code, Tokens &out, Error *error = nullptr);
bool run_device_flow(const DeviceFlowConfig &cfg, DeviceCode &device_code, Tokens &out,
    Error *error = nullptr, void (*on_device_code)(const DeviceCode &, void *) = nullptr, void *on_device_code_ctx = nullptr,
    bool (*should_abort)(void *) = nullptr, void *should_abort_ctx = nullptr);
bool refresh_tokens(const DeviceFlowConfig &cfg, const char *refresh_token, Tokens &out, Error *error = nullptr);

} // namespace buddy::oauth
