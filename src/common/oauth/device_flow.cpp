#include "device_flow.hpp"

#include <common/tls/tls.hpp>
#include <common/timing.h>
#include <http/connect_error.h>
#include <http/httpc.hpp>
#include <logging/log.hpp>
#define JSMN_HEADER
#include <jsmn.h>
#include <support_utils.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <variant>

#include <cmsis_os.h>

namespace buddy::oauth {
LOG_COMPONENT_DEF(oauth_df, logging::Severity::info);

namespace {
constexpr uint8_t TLS_TIMEOUT_S = 30;
constexpr uint16_t HTTPS_DEFAULT_PORT = 443;
constexpr size_t HOST_BUF_LEN = 96;
constexpr size_t PATH_BUF_LEN = 192;
constexpr size_t REQ_BODY_BUF_LEN = 2048;
constexpr size_t RESP_BODY_BUF_LEN = 4096;
constexpr size_t TOKENS_COUNT = 64;

struct ScratchBuffers {
    char host[HOST_BUF_LEN] = {};
    char path[PATH_BUF_LEN] = {};
    char req_body[REQ_BODY_BUF_LEN] = {};
    char resp_body[RESP_BODY_BUF_LEN] = {};
    jsmntok_t tokens[TOKENS_COUNT] = {};
    char response_error[64] = {};
};

// Keep large work buffers out of task stack (connect2 task is memory constrained).
static ScratchBuffers scratch;

class TlsConnectionFactory final : public http::ConnectionFactory {
private:
    class HttpTlsConnection;

public:
    TlsConnectionFactory(const char *host, uint16_t port, bool custom_cert)
        : host_(host)
        , port_(port)
        , custom_cert_(custom_cert) {
    }

    std::variant<http::Connection *, http::Error> connection() override {
        if (!connected_) {
            tls_ = std::make_unique<HttpTlsConnection>(TLS_TIMEOUT_S, custom_cert_);
            if (!tls_) {
                return http::Error::Memory;
            }
            if (auto err = tls_->open(host_, port_); err.has_value()) {
                tls_.reset();
                return err.value();
            }
            connected_ = true;
        }
        return tls_.get();
    }

    const char *host() override {
        return host_;
    }

    void invalidate() override {
        tls_.reset();
        connected_ = false;
    }

private:
    class HttpTlsConnection final : public http::Connection {
    public:
        HttpTlsConnection(uint8_t timeout_s, bool custom_cert)
            : http::Connection(timeout_s)
            , tls_(timeout_s, custom_cert) {
        }

        std::optional<http::Error> open(const char *host, uint16_t port) {
            tls_.set_io_timeout_s(get_timeout_s());
            return tls_.connection(host, port, host, port);
        }

        std::variant<size_t, http::Error> rx(uint8_t *buffer, size_t len, bool nonblock) override {
            return tls_.rx(buffer, len, nonblock);
        }

        std::variant<size_t, http::Error> tx(const uint8_t *buffer, size_t len) override {
            return tls_.tx(buffer, len);
        }

        bool poll_readable(uint32_t timeout) override {
            return tls_.poll_readable(timeout);
        }

    private:
        buddy::tls::tls tls_;
    };

    const char *host_;
    uint16_t port_;
    bool custom_cert_;
    bool connected_ = false;
    std::unique_ptr<HttpTlsConnection> tls_;
};

class FormPostRequest final : public http::Request {
public:
    FormPostRequest(const char *url, const char *body)
        : url_(url) {
        strlcpy(body_, body, sizeof(body_));
    }

    const char *url() const override {
        return url_;
    }

    http::ContentType content_type() const override {
        return http::ContentType::ApplicationXWwwFormUrlencoded;
    }

    http::Method method() const override {
        return http::Method::Post;
    }

    std::variant<size_t, http::Error> write_body_chunk(char *out, size_t size) override {
        if (sent_) {
            return static_cast<size_t>(0);
        }
        const size_t len = strlen(body_);
        if (len > size) {
            return http::Error::InternalError;
        }
        memcpy(out, body_, len);
        sent_ = true;
        return len;
    }

private:
    const char *url_;
    char body_[REQ_BODY_BUF_LEN] = {};
    bool sent_ = false;
};

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

bool json_get_u32(const char *json, const jsmntok_t *tokens, size_t cnt, const char *key, uint32_t &out) {
    char tmp[24] = {};
    if (!json_get_string(json, tokens, cnt, key, tmp, sizeof(tmp))) {
        return false;
    }
    char *end = nullptr;
    const unsigned long parsed = strtoul(tmp, &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    out = static_cast<uint32_t>(parsed);
    return true;
}

bool parse_https_url(const char *url, char *host_out, size_t host_out_len, uint16_t &port_out, char *path_out, size_t path_out_len) {
    if (url == nullptr || strncmp(url, "https://", 8) != 0) {
        return false;
    }
    const char *p = url + 8;
    const char *host_start = p;
    while (*p != '\0' && *p != ':' && *p != '/') {
        ++p;
    }
    const size_t host_len = static_cast<size_t>(p - host_start);
    if (host_len == 0 || host_len >= host_out_len) {
        return false;
    }
    memcpy(host_out, host_start, host_len);
    host_out[host_len] = '\0';

    port_out = HTTPS_DEFAULT_PORT;
    if (*p == ':') {
        ++p;
        char port_buf[8] = {};
        size_t i = 0;
        while (*p != '\0' && *p != '/' && i + 1 < sizeof(port_buf)) {
            port_buf[i++] = *p++;
        }
        port_buf[i] = '\0';
        if (i == 0) {
            return false;
        }
        char *end = nullptr;
        const unsigned long parsed = strtoul(port_buf, &end, 10);
        if (end == nullptr || *end != '\0' || parsed == 0 || parsed > 65535) {
            return false;
        }
        port_out = static_cast<uint16_t>(parsed);
    }

    if (*p == '\0') {
        strlcpy(path_out, "/", path_out_len);
    } else {
        strlcpy(path_out, p, path_out_len);
    }
    return true;
}

bool send_form_post(const char *host, uint16_t port, bool custom_cert, const char *path, const char *body,
    char *resp_body, size_t resp_body_len, http::Status &status_out) {
    auto factory = std::make_unique<TlsConnectionFactory>(host, port, custom_cert);
    if (!factory) {
        log_info(oauth_df, "http setup failed: no memory for factory");
        return false;
    }
    auto client = std::make_unique<http::HttpClient>(*factory);
    if (!client) {
        log_info(oauth_df, "http setup failed: no memory for client");
        return false;
    }
    auto req = std::make_unique<FormPostRequest>(path, body);
    if (!req) {
        log_info(oauth_df, "http setup failed: no memory for request");
        return false;
    }
    auto result = client->send(*req);
    if (std::holds_alternative<http::Error>(result)) {
        log_info(oauth_df, "http send failed: %s host=%s port=%u path=%s", http::to_str(std::get<http::Error>(result)), host, static_cast<unsigned>(port), path);
        factory->invalidate();
        return false;
    }

    auto response = std::move(std::get<http::Response>(result));
    status_out = response.status;
    log_info(oauth_df, "http response host=%s port=%u path=%s status=%u content_length=%u keep_alive=%u",
        host,
        static_cast<unsigned>(port),
        path,
        static_cast<unsigned>(status_out),
        static_cast<unsigned>(response.content_length()),
        response.can_keep_alive ? 1U : 0U);
    auto read_res = response.read_all(reinterpret_cast<uint8_t *>(resp_body), resp_body_len - 1);
    if (std::holds_alternative<http::Error>(read_res)) {
        log_info(oauth_df, "http read failed: %s host=%s port=%u path=%s status=%u",
            http::to_str(std::get<http::Error>(read_res)),
            host, static_cast<unsigned>(port), path, static_cast<unsigned>(status_out));
        factory->invalidate();
        return false;
    }

    const size_t read_len = std::get<size_t>(read_res);
    resp_body[read_len] = '\0';
    factory->invalidate();
    return true;
}

bool send_form_post_keep_alive(http::HttpClient &client, TlsConnectionFactory &factory,
    const char *host, uint16_t port, const char *path, const char *body,
    char *resp_body, size_t resp_body_len, http::Status &status_out) {
    FormPostRequest req(path, body);
    auto result = client.send(req);
    if (std::holds_alternative<http::Error>(result)) {
        log_info(oauth_df, "http send failed: %s host=%s port=%u path=%s", http::to_str(std::get<http::Error>(result)), host, static_cast<unsigned>(port), path);
        factory.invalidate();
        return false;
    }

    auto response = std::move(std::get<http::Response>(result));
    status_out = response.status;
    log_info(oauth_df, "http response host=%s port=%u path=%s status=%u content_length=%u keep_alive=%u",
        host,
        static_cast<unsigned>(port),
        path,
        static_cast<unsigned>(status_out),
        static_cast<unsigned>(response.content_length()),
        response.can_keep_alive ? 1U : 0U);
    auto read_res = response.read_all(reinterpret_cast<uint8_t *>(resp_body), resp_body_len - 1);
    if (std::holds_alternative<http::Error>(read_res)) {
        log_info(oauth_df, "http read failed: %s host=%s port=%u path=%s status=%u",
            http::to_str(std::get<http::Error>(read_res)),
            host, static_cast<unsigned>(port), path, static_cast<unsigned>(status_out));
        factory.invalidate();
        return false;
    }

    const size_t read_len = std::get<size_t>(read_res);
    resp_body[read_len] = '\0';
    if (!response.can_keep_alive) {
        factory.invalidate();
    }
    return true;
}

bool set_error(Error *error, Error value) {
    if (error) {
        *error = value;
    }
    return false;
}

bool parse_token_json(const char *json, http::Status status, Tokens &out, uint32_t &interval_s, Error *error) {
    jsmn_parser parser;
    jsmn_init(&parser);
    memset(scratch.tokens, 0, sizeof(scratch.tokens));
    const int count = jsmn_parse(&parser, json, strlen(json), scratch.tokens, static_cast<unsigned>(std::size(scratch.tokens)));
    if (count < 1) {
        return set_error(error, Error::ParseError);
    }

    if (status == http::Status::Ok) {
        const bool have_access = json_get_string(json, scratch.tokens, count, "access_token", out.access_token, sizeof(out.access_token));
        const bool have_refresh = json_get_string(json, scratch.tokens, count, "refresh_token", out.refresh_token, sizeof(out.refresh_token));
        out.expires_in_s = 0;
        (void)json_get_u32(json, scratch.tokens, count, "expires_in", out.expires_in_s);
        if (!have_access || !have_refresh) {
            return set_error(error, Error::MissingField);
        }
        return true;
    }

    memset(scratch.response_error, 0, sizeof(scratch.response_error));
    if (!json_get_string(json, scratch.tokens, count, "error", scratch.response_error, sizeof(scratch.response_error))) {
        return set_error(error, Error::ResponseError);
    }
    if (strcmp(scratch.response_error, "authorization_pending") == 0) {
        return false;
    }
    if (strcmp(scratch.response_error, "slow_down") == 0) {
        interval_s += 5;
        return false;
    }
    if (strcmp(scratch.response_error, "access_denied") == 0) {
        return set_error(error, Error::AuthorizationDenied);
    }
    return set_error(error, Error::ResponseError);
}
} // namespace

const char *to_str(Error error) {
    switch (error) {
    case Error::None:
        return "none";
    case Error::InvalidConfig:
        return "invalid_config";
    case Error::InvalidUrl:
        return "invalid_url";
    case Error::HttpError:
        return "http_error";
    case Error::ParseError:
        return "parse_error";
    case Error::MissingField:
        return "missing_field";
    case Error::AuthorizationDenied:
        return "authorization_denied";
    case Error::Canceled:
        return "canceled";
    case Error::PollingTimeout:
        return "polling_timeout";
    case Error::ResponseError:
        return "response_error";
    default:
        return "unknown";
    }
}

bool request_device_code(const DeviceFlowConfig &cfg, DeviceCode &out, Error *error) {
    if (error) {
        *error = Error::None;
    }

    if (cfg.device_auth_url == nullptr || cfg.client_id == nullptr || cfg.client_id[0] == '\0') {
        return set_error(error, Error::InvalidConfig);
    }

    uint16_t port = 0;
    scratch = {};
    if (!parse_https_url(cfg.device_auth_url, scratch.host, sizeof(scratch.host), port, scratch.path, sizeof(scratch.path))) {
        return set_error(error, Error::InvalidUrl);
    }

    snprintf(scratch.req_body, sizeof(scratch.req_body), "client_id=%s&scope=%s", cfg.client_id, cfg.scope ? cfg.scope : "mqtt");

    http::Status status = http::Status::UnknownStatus;
    if (!send_form_post(scratch.host, port, cfg.custom_cert, scratch.path, scratch.req_body, scratch.resp_body, sizeof(scratch.resp_body), status)) {
        return set_error(error, Error::HttpError);
    }
    if (status != http::Status::Ok) {
        return set_error(error, Error::ResponseError);
    }

    jsmn_parser parser;
    jsmn_init(&parser);
    const int tok_count = jsmn_parse(&parser, scratch.resp_body, strlen(scratch.resp_body), scratch.tokens, static_cast<unsigned>(std::size(scratch.tokens)));
    if (tok_count < 1) {
        return set_error(error, Error::ParseError);
    }

    const bool have_device_code = json_get_string(scratch.resp_body, scratch.tokens, tok_count, "device_code", out.device_code, sizeof(out.device_code));
    const bool have_user_code = json_get_string(scratch.resp_body, scratch.tokens, tok_count, "user_code", out.user_code, sizeof(out.user_code));
    const bool have_uri = json_get_string(scratch.resp_body, scratch.tokens, tok_count, "verification_uri", out.verification_uri, sizeof(out.verification_uri));
    out.interval_s = cfg.initial_poll_interval_s;
    (void)json_get_u32(scratch.resp_body, scratch.tokens, tok_count, "interval", out.interval_s);
    if (!have_device_code || !have_user_code || !have_uri) {
        return set_error(error, Error::MissingField);
    }

    return true;
}

bool poll_tokens(const DeviceFlowConfig &cfg, const DeviceCode &device_code, Tokens &out, Error *error) {
    if (error) {
        *error = Error::None;
    }

    if (cfg.token_url == nullptr || cfg.client_id == nullptr || cfg.client_id[0] == '\0' || device_code.device_code[0] == '\0') {
        return set_error(error, Error::InvalidConfig);
    }

    uint16_t port = 0;
    scratch = {};
    if (!parse_https_url(cfg.token_url, scratch.host, sizeof(scratch.host), port, scratch.path, sizeof(scratch.path))) {
        return set_error(error, Error::InvalidUrl);
    }

    TlsConnectionFactory factory(scratch.host, port, cfg.custom_cert);
    http::HttpClient client(factory);

    uint32_t interval_s = device_code.interval_s > 0 ? device_code.interval_s : cfg.initial_poll_interval_s;
    if (interval_s < 2) {
        interval_s = 2;
    }
    const char *sn = (cfg.serial_number && cfg.serial_number[0] != '\0') ? cfg.serial_number : "UNKNOWN_SN";
    const uint32_t timeout_s = cfg.poll_timeout_s > 0 ? cfg.poll_timeout_s : (30 * 60);
    const uint32_t started_ms = ticks_ms();
    uint8_t attempts = 0;

    while (true) {
        if (cfg.max_poll_attempts > 0 && attempts >= cfg.max_poll_attempts) {
            return set_error(error, Error::PollingTimeout);
        }
        if ((ticks_ms() - started_ms) >= (timeout_s * 1000U)) {
            return set_error(error, Error::PollingTimeout);
        }
        ++attempts;

        memset(scratch.req_body, 0, sizeof(scratch.req_body));
        snprintf(scratch.req_body, sizeof(scratch.req_body),
            "grant_type=urn:ietf:params:oauth:grant-type:device_code&device_code=%s&client_id=%s&sn=%s",
            device_code.device_code, cfg.client_id, sn);

        memset(scratch.resp_body, 0, sizeof(scratch.resp_body));
        http::Status status = http::Status::UnknownStatus;
        if (!send_form_post_keep_alive(client, factory, scratch.host, port, scratch.path, scratch.req_body, scratch.resp_body, sizeof(scratch.resp_body), status)) {
            // Keep polling on transient transport errors.
            osDelay(interval_s * 1000U);
            continue;
        }

        if (parse_token_json(scratch.resp_body, status, out, interval_s, error)) {
            return true;
        }
        if (error != nullptr && *error != Error::None) {
            return false;
        }
        osDelay(interval_s * 1000U);
    }
}

bool run_device_flow(const DeviceFlowConfig &cfg, DeviceCode &device_code, Tokens &out,
    Error *error, void (*on_device_code)(const DeviceCode &, void *), void *on_device_code_ctx,
    bool (*should_abort)(void *), void *should_abort_ctx) {
    if (error) {
        *error = Error::None;
    }
    if (cfg.device_auth_url == nullptr || cfg.token_url == nullptr || cfg.client_id == nullptr || cfg.client_id[0] == '\0') {
        return set_error(error, Error::InvalidConfig);
    }

    scratch = {};
    char token_host[HOST_BUF_LEN] = {};
    char token_path[PATH_BUF_LEN] = {};
    uint16_t auth_port = 0;
    uint16_t token_port = 0;

    if (!parse_https_url(cfg.device_auth_url, scratch.host, sizeof(scratch.host), auth_port, scratch.path, sizeof(scratch.path))) {
        return set_error(error, Error::InvalidUrl);
    }
    if (!parse_https_url(cfg.token_url, token_host, sizeof(token_host), token_port, token_path, sizeof(token_path))) {
        return set_error(error, Error::InvalidUrl);
    }
    if (strcmp(scratch.host, token_host) != 0 || auth_port != token_port) {
        return set_error(error, Error::InvalidConfig);
    }

    TlsConnectionFactory factory(scratch.host, auth_port, cfg.custom_cert);
    http::HttpClient client(factory);

    memset(scratch.req_body, 0, sizeof(scratch.req_body));
    snprintf(scratch.req_body, sizeof(scratch.req_body), "client_id=%s&scope=%s", cfg.client_id, cfg.scope ? cfg.scope : "mqtt");
    http::Status status = http::Status::UnknownStatus;
    if (!send_form_post_keep_alive(client, factory, scratch.host, auth_port, scratch.path, scratch.req_body, scratch.resp_body, sizeof(scratch.resp_body), status)) {
        return set_error(error, Error::HttpError);
    }
    if (status != http::Status::Ok) {
        return set_error(error, Error::ResponseError);
    }

    jsmn_parser parser;
    jsmn_init(&parser);
    memset(scratch.tokens, 0, sizeof(scratch.tokens));
    const int tok_count = jsmn_parse(&parser, scratch.resp_body, strlen(scratch.resp_body), scratch.tokens, static_cast<unsigned>(std::size(scratch.tokens)));
    if (tok_count < 1) {
        return set_error(error, Error::ParseError);
    }

    const bool have_device_code = json_get_string(scratch.resp_body, scratch.tokens, tok_count, "device_code", device_code.device_code, sizeof(device_code.device_code));
    const bool have_user_code = json_get_string(scratch.resp_body, scratch.tokens, tok_count, "user_code", device_code.user_code, sizeof(device_code.user_code));
    const bool have_uri = json_get_string(scratch.resp_body, scratch.tokens, tok_count, "verification_uri", device_code.verification_uri, sizeof(device_code.verification_uri));
    device_code.interval_s = cfg.initial_poll_interval_s;
    (void)json_get_u32(scratch.resp_body, scratch.tokens, tok_count, "interval", device_code.interval_s);
    if (!have_device_code || !have_user_code || !have_uri) {
        return set_error(error, Error::MissingField);
    }
    if (on_device_code != nullptr) {
        on_device_code(device_code, on_device_code_ctx);
    }

    uint32_t interval_s = device_code.interval_s > 0 ? device_code.interval_s : cfg.initial_poll_interval_s;
    if (interval_s < 2) {
        interval_s = 2;
    }
    const char *sn = (cfg.serial_number && cfg.serial_number[0] != '\0') ? cfg.serial_number : "UNKNOWN_SN";
    const uint32_t timeout_s = cfg.poll_timeout_s > 0 ? cfg.poll_timeout_s : (30 * 60);
    const uint32_t started_ms = ticks_ms();
    uint8_t attempts = 0;

    while (true) {
        if (should_abort != nullptr && should_abort(should_abort_ctx)) {
            return set_error(error, Error::Canceled);
        }
        if (cfg.max_poll_attempts > 0 && attempts >= cfg.max_poll_attempts) {
            return set_error(error, Error::PollingTimeout);
        }
        if ((ticks_ms() - started_ms) >= (timeout_s * 1000U)) {
            return set_error(error, Error::PollingTimeout);
        }
        ++attempts;

        memset(scratch.req_body, 0, sizeof(scratch.req_body));
        snprintf(scratch.req_body, sizeof(scratch.req_body),
            "grant_type=urn:ietf:params:oauth:grant-type:device_code&device_code=%s&client_id=%s&sn=%s",
            device_code.device_code, cfg.client_id, sn);

        memset(scratch.resp_body, 0, sizeof(scratch.resp_body));
        status = http::Status::UnknownStatus;
        if (!send_form_post_keep_alive(client, factory, scratch.host, auth_port, token_path, scratch.req_body, scratch.resp_body, sizeof(scratch.resp_body), status)) {
            osDelay(interval_s * 1000U);
            continue;
        }

        if (parse_token_json(scratch.resp_body, status, out, interval_s, error)) {
            return true;
        }
        if (error != nullptr && *error != Error::None) {
            return false;
        }
        osDelay(interval_s * 1000U);
    }
}

bool refresh_tokens(const DeviceFlowConfig &cfg, const char *refresh_token, Tokens &out, Error *error) {
    if (error) {
        *error = Error::None;
    }
    if (cfg.token_url == nullptr || cfg.client_id == nullptr || cfg.client_id[0] == '\0' || refresh_token == nullptr || refresh_token[0] == '\0') {
        return set_error(error, Error::InvalidConfig);
    }

    uint16_t port = 0;
    scratch = {};
    if (!parse_https_url(cfg.token_url, scratch.host, sizeof(scratch.host), port, scratch.path, sizeof(scratch.path))) {
        return set_error(error, Error::InvalidUrl);
    }

    snprintf(scratch.req_body, sizeof(scratch.req_body),
        "grant_type=refresh_token&refresh_token=%s&client_id=%s",
        refresh_token, cfg.client_id);

    http::Status status = http::Status::UnknownStatus;
    if (!send_form_post(scratch.host, port, cfg.custom_cert, scratch.path, scratch.req_body, scratch.resp_body, sizeof(scratch.resp_body), status)) {
        return set_error(error, Error::HttpError);
    }

    jsmn_parser parser;
    jsmn_init(&parser);
    memset(scratch.tokens, 0, sizeof(scratch.tokens));
    const int count = jsmn_parse(&parser, scratch.resp_body, strlen(scratch.resp_body), scratch.tokens, static_cast<unsigned>(std::size(scratch.tokens)));
    if (count < 1) {
        return set_error(error, Error::ParseError);
    }

    if (status != http::Status::Ok) {
        return set_error(error, Error::ResponseError);
    }

    const bool have_access = json_get_string(scratch.resp_body, scratch.tokens, count, "access_token", out.access_token, sizeof(out.access_token));
    const bool have_refresh = json_get_string(scratch.resp_body, scratch.tokens, count, "refresh_token", out.refresh_token, sizeof(out.refresh_token));
    out.expires_in_s = 0;
    (void)json_get_u32(scratch.resp_body, scratch.tokens, count, "expires_in", out.expires_in_s);

    if (!have_access) {
        return set_error(error, Error::MissingField);
    }
    if (!have_refresh) {
        // Refresh token might be omitted by server; keep caller-provided token.
    }
    return true;
}

} // namespace buddy::oauth
