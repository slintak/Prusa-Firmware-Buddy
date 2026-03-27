#include "client.hpp"

#include <cmsis_os.h>
#include "command.pb.h"
#include "event.pb.h"
#include "gcode.pb.h"
#include <cctype>
#include <connect/marlin_printer.hpp>
#include <common/timing.h>
#include <common/print_utils.hpp>
#include <cstring>
#include <common/crc32.h>
#include <common/config.h>
#include <common/marlin_client.hpp>
#include <algorithm>
#include <netdev.h>
#include <netif_settings.h>
#include <otp.hpp>
#include <common/oauth/jwt.hpp>
#include <support_utils.h>
#include <logging/log.hpp>
#include <connect/printer_common.hpp>
#include <ctime>
#include <pb_decode.h>
#include <pb_encode.h>

LOG_COMPONENT_REF(connect2);

namespace connect2_client {

namespace {
constexpr uint32_t IDLE_DELAY_MS = 50;
constexpr uint32_t CONFIG_REFRESH_MS = 10000;
constexpr uint32_t CONNECT_TIMEOUT_MS = 60000;
constexpr uint16_t DEFAULT_PORT_PLAIN = 1883;
constexpr uint16_t DEFAULT_PORT_TLS = 8883;
constexpr const char *OAUTH_CLIENT_ID = "buddy-connect2";
constexpr uint32_t OAUTH_FAILURE_MIN_BACKOFF_MS = 5000;
constexpr size_t FORCE_GCODE_SLOTS = 8;
constexpr size_t FORCE_GCODE_MAX_LEN = MARLIN_MAX_REQUEST;

const char *command_payload_name(pb_size_t which_payload) {
    switch (which_payload) {
    case connect2_CommandEnvelope_send_info_tag:
        return "SEND_INFO";
    case connect2_CommandEnvelope_send_job_info_tag:
        return "SEND_JOB_INFO";
    case connect2_CommandEnvelope_send_file_info_tag:
        return "SEND_FILE_INFO";
    case connect2_CommandEnvelope_send_transfer_info_tag:
        return "SEND_TRANSFER_INFO";
    case connect2_CommandEnvelope_pause_print_tag:
        return "PAUSE_PRINT";
    case connect2_CommandEnvelope_resume_print_tag:
        return "RESUME_PRINT";
    case connect2_CommandEnvelope_stop_print_tag:
        return "STOP_PRINT";
    case connect2_CommandEnvelope_start_print_tag:
        return "START_PRINT";
    case connect2_CommandEnvelope_set_printer_ready_tag:
        return "SET_PRINTER_READY";
    case connect2_CommandEnvelope_cancel_printer_ready_tag:
        return "CANCEL_PRINTER_READY";
    case connect2_CommandEnvelope_set_idle_tag:
        return "SET_IDLE";
    case connect2_CommandEnvelope_send_state_info_tag:
        return "SEND_STATE_INFO";
    case connect2_CommandEnvelope_reset_printer_tag:
        return "RESET_PRINTER";
    case connect2_CommandEnvelope_set_token_tag:
        return "SET_TOKEN";
    default:
        return "UNKNOWN";
    }
}

const char *event_type_name(connect2_EventType event_type) {
    switch (event_type) {
    case connect2_EventType_EVENT_TYPE_INFO:
        return "INFO";
    case connect2_EventType_EVENT_TYPE_ACCEPTED:
        return "ACCEPTED";
    case connect2_EventType_EVENT_TYPE_REJECTED:
        return "REJECTED";
    case connect2_EventType_EVENT_TYPE_JOB_INFO:
        return "JOB_INFO";
    case connect2_EventType_EVENT_TYPE_FILE_INFO:
        return "FILE_INFO";
    case connect2_EventType_EVENT_TYPE_FILE_CHANGED:
        return "FILE_CHANGED";
    case connect2_EventType_EVENT_TYPE_TRANSFER_INFO:
        return "TRANSFER_INFO";
    case connect2_EventType_EVENT_TYPE_FINISHED:
        return "FINISHED";
    case connect2_EventType_EVENT_TYPE_FAILED:
        return "FAILED";
    case connect2_EventType_EVENT_TYPE_TRANSFER_STOPPED:
        return "TRANSFER_STOPPED";
    case connect2_EventType_EVENT_TYPE_TRANSFER_ABORTED:
        return "TRANSFER_ABORTED";
    case connect2_EventType_EVENT_TYPE_TRANSFER_FINISHED:
        return "TRANSFER_FINISHED";
    case connect2_EventType_EVENT_TYPE_CANCELABLE_CHANGED:
        return "CANCELABLE_CHANGED";
    case connect2_EventType_EVENT_TYPE_STATE_CHANGED:
        return "STATE_CHANGED";
    default:
        return "UNSPECIFIED";
    }
}

const char *printer_type_name() {
#if PRINTER_IS_PRUSA_MK4()
    return "MK4S";
#elif PRINTER_IS_PRUSA_MK3_5()
    return "MK3.5";
#elif PRINTER_IS_PRUSA_COREONE()
    return "COREONE";
#elif PRINTER_IS_PRUSA_MINI()
    return "MINI";
#elif PRINTER_IS_PRUSA_XL()
    return "XL";
#elif PRINTER_IS_PRUSA_iX()
    return "iX";
#else
    return "UNKNOWN";
#endif
}

const char *current_state_string() {
    return printer_state::to_str(printer_state::get_state(connect_client::MarlinPrinter::is_printer_ready()));
}

const connect_client::Printer::PrinterInfo &printer_info_snapshot() {
    static connect_client::Printer::PrinterInfo info {};
    static bool initialized = false;
    if (!initialized) {
        connect_client::init_info(info);
        initialized = true;
    }
    return info;
}

struct StringDecodeTarget {
    char *buffer;
    size_t size;
};

bool decode_string_field(pb_istream_t *stream, const pb_field_t *, void **arg) {
    if (arg == nullptr || *arg == nullptr) {
        return false;
    }
    auto *target = static_cast<StringDecodeTarget *>(*arg);
    const size_t len = stream->bytes_left;
    if (target->buffer == nullptr || target->size == 0 || len >= target->size) {
        return false;
    }
    if (!pb_read(stream, reinterpret_cast<pb_byte_t *>(target->buffer), len)) {
        return false;
    }
    target->buffer[len] = '\0';
    return true;
}

bool encode_string_field(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    if (arg == nullptr || *arg == nullptr) {
        return true;
    }
    const char *value = static_cast<const char *>(*arg);
    const size_t len = strlen(value);
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }
    return pb_encode_string(stream, reinterpret_cast<const pb_byte_t *>(value), len);
}

bool trim_line(const char *begin, const char *end, char *out, size_t out_size) {
    while (begin < end && isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    while (end > begin && isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }

    const size_t len = static_cast<size_t>(end - begin);
    if (len == 0) {
        out[0] = '\0';
        return true;
    }
    if (len >= out_size) {
        return false;
    }

    memcpy(out, begin, len);
    out[len] = '\0';
    return true;
}

void normalize_gcode_line(char *line) {
    if (line == nullptr || line[0] == '\0') {
        return;
    }

    bool in_comment = false;
    bool first_token = true;
    for (char *p = line; *p != '\0'; ++p) {
        if (*p == ';') {
            in_comment = true;
        }
        if (in_comment) {
            continue;
        }
        if (isspace(static_cast<unsigned char>(*p))) {
            continue;
        }

        if (first_token) {
            while (*p != '\0' && !isspace(static_cast<unsigned char>(*p)) && *p != ';') {
                if (isalpha(static_cast<unsigned char>(*p))) {
                    *p = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
                }
                ++p;
            }
            first_token = false;
            if (*p == '\0') {
                break;
            }
            continue;
        }

        if (isalpha(static_cast<unsigned char>(*p))) {
            const char next = *(p + 1);
            if (isdigit(static_cast<unsigned char>(next)) || next == '-' || next == '+' || next == '.') {
                *p = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
            }
        }

        while (*p != '\0' && !isspace(static_cast<unsigned char>(*p)) && *p != ';') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
    }
}

OnlineError oauth_error_to_online_error(buddy::oauth::Error error) {
    switch (error) {
    case buddy::oauth::Error::AuthorizationDenied:
        return OnlineError::Auth;
    case buddy::oauth::Error::HttpError:
        return OnlineError::Network;
    case buddy::oauth::Error::InvalidUrl:
    case buddy::oauth::Error::InvalidConfig:
        return OnlineError::Internal;
    case buddy::oauth::Error::ParseError:
    case buddy::oauth::Error::MissingField:
    case buddy::oauth::Error::ResponseError:
    case buddy::oauth::Error::PollingTimeout:
        return OnlineError::Protocol;
    case buddy::oauth::Error::None:
    default:
        return OnlineError::NoError;
    }
}

OnlineError mqtt_error_to_online_error(enum MQTTErrors error) {
    switch (error) {
    case MQTT_ERROR_CONNECTION_REFUSED:
        return OnlineError::Auth;
    case MQTT_ERROR_SOCKET_ERROR:
    case MQTT_ERROR_CONNECTION_CLOSED:
        return OnlineError::Network;
    default:
        return OnlineError::Connection;
    }
}

void build_verification_url_with_code(const char *verification_uri, const char *user_code, char *out, size_t out_len) {
    if (verification_uri == nullptr || verification_uri[0] == '\0') {
        out[0] = '\0';
        return;
    }
    const char separator = strchr(verification_uri, '?') != nullptr ? '&' : '?';
    snprintf(out, out_len, "%s%cuser_code=%s", verification_uri, separator, user_code ? user_code : "");
}

} // namespace

Client::Client(buddy::mqtt::Client &mqtt_client)
    : mqtt_client_(mqtt_client) {
    mqtt_client_.set_publish_callback(&Client::publish_callback, this);
}

void Client::run() {
    for (;;) {
        step();
        sleep_idle(IDLE_DELAY_MS);
    }
}

void Client::request_registration() {
    registration_requested_.store(true);
}

void Client::cancel_registration() {
    registration_cancel_requested_.store(true);
}

OnlineStatus Client::last_status() const {
    return OnlineStatus { status_.load(), error_.load() };
}

bool Client::has_stored_auth() const {
    return has_stored_auth_.load();
}

RegistrationInfo Client::registration_info() const {
    RegistrationInfo info {};
    info.available = false;
    if (!registration_info_valid_.load()) {
        return info;
    }

    strlcpy(info.verification_uri, verification_uri_, sizeof(info.verification_uri));
    strlcpy(info.user_code, user_code_, sizeof(info.user_code));
    strlcpy(info.verification_url_with_code, verification_url_with_code_, sizeof(info.verification_url_with_code));

    if (!registration_info_valid_.load()) {
        info = {};
        return info;
    }

    info.available = true;
    return info;
}

void Client::step() {
    const uint32_t now = ticks_ms();
    refresh_config(now);

    const bool net_ready = network_ready();
    if (net_ready != last_net_ready_) {
        log_info(connect2, "network %s", net_ready ? "ready" : "down");
        last_net_ready_ = net_ready;
    }
    if (!net_ready) {
        if (state_ != State::Disabled) {
            mqtt_client_.disconnect();
            telemetry_.reset();
            subscribed_ = false;
            state_ = State::Disconnected;
            next_action_ms_ = 0;
        }
        status_.store(ConnectionStatus::Error);
        error_.store(OnlineError::Network);
        return;
    }

    if (registration_requested_.exchange(false)) {
        mqtt_client_.disconnect();
        telemetry_.reset();
        subscribed_ = false;
        auth_ = {};
        (void)clear_oauth_storage();
        has_stored_auth_.store(false);
        state_ = State::Disconnected;
        next_action_ms_ = 0;
        backoff_.reset();
        registration_info_valid_.store(false);
        verification_uri_[0] = '\0';
        user_code_[0] = '\0';
        verification_url_with_code_[0] = '\0';
        registration_in_progress_ = true;
        log_info(connect2, "oauth manual registration requested");
    }

    if (registration_cancel_requested_.exchange(false)) {
        registration_in_progress_ = false;
        registration_info_valid_.store(false);
        verification_uri_[0] = '\0';
        user_code_[0] = '\0';
        verification_url_with_code_[0] = '\0';
        log_info(connect2, "oauth manual registration canceled");
        if (!has_valid_auth()) {
            status_.store(ConnectionStatus::AuthRequired);
            error_.store(OnlineError::NoError);
            state_ = State::RegistrationRequired;
            next_action_ms_ = 0;
            return;
        }
    }

    switch (state_) {
    case State::Disabled:
        status_.store(cfg_.enabled ? ConnectionStatus::NoConfig : ConnectionStatus::Off);
        error_.store(OnlineError::NoError);
        return;
    case State::Disconnected:
        status_.store(ConnectionStatus::Authorizing);
        error_.store(OnlineError::NoError);
        if (next_action_ms_ != 0 && ticks_diff(now, next_action_ms_) < 0) {
            return;
        }
        if (cfg_.oauth_device_auth_url[0] != '\0' && cfg_.oauth_token_url[0] != '\0') {
            mqtt_client_.disconnect();
            subscribed_ = false;
            const uint32_t now_epoch = static_cast<uint32_t>(time(nullptr));
            if (!has_valid_auth()) {
                if (!registration_in_progress_) {
                    status_.store(ConnectionStatus::AuthRequired);
                    error_.store(OnlineError::NoError);
                    state_ = State::RegistrationRequired;
                    next_action_ms_ = 0;
                    return;
                }

                log_info(connect2, "oauth no stored credentials, starting device flow");
                if (!run_oauth_device_flow()) {
                    if (!registration_in_progress_) {
                        status_.store(ConnectionStatus::AuthRequired);
                        error_.store(OnlineError::NoError);
                        state_ = State::RegistrationRequired;
                        next_action_ms_ = 0;
                        return;
                    }
                    registration_in_progress_ = false;
                    log_info(connect2, "oauth device flow failed");
                    status_.store(ConnectionStatus::Error);
                    enter_backoff(now, OAUTH_FAILURE_MIN_BACKOFF_MS);
                    return;
                }
                registration_in_progress_ = false;
            } else if (should_refresh_token(now_epoch)) {
                log_info(connect2, "oauth token past half-life, starting refresh");
                if (!run_oauth_refresh()) {
                    log_info(connect2, "oauth refresh failed, registration required");
                    status_.store(ConnectionStatus::AuthRequired);
                    state_ = State::RegistrationRequired;
                    next_action_ms_ = 0;
                    return;
                }
            } else {
                log_info(connect2, "oauth using stored access token user=%s exp_at=%lu now=%lu",
                    auth_.mqtt_username,
                    static_cast<unsigned long>(auth_.expires_at_epoch_s),
                    static_cast<unsigned long>(now_epoch));
            }
            log_info(connect2, "oauth auth ready");
        }
        {
            const uint16_t port = cfg_.port != 0 ? cfg_.port : (cfg_.tls ? DEFAULT_PORT_TLS : DEFAULT_PORT_PLAIN);
            char will_topic[128];
            const bool have_will = telemetry_.build_online_topic(will_topic, sizeof(will_topic));
            const char *will_payload = "0";
            const size_t will_payload_len = strlen(will_payload);
            if (!mqtt_client_.connect(cfg_.host, port, cfg_.tls, cfg_.custom_cert,
                    has_valid_auth() ? auth_.mqtt_username : nullptr,
                    has_valid_auth() ? auth_.access_token : nullptr,
                    have_will ? will_topic : nullptr,
                    have_will ? will_payload : nullptr,
                    have_will ? will_payload_len : 0,
                    1, true)) {
                status_.store(ConnectionStatus::Error);
                error_.store(mqtt_error_to_online_error(mqtt_client_.last_error()));
                enter_backoff(now);
                return;
            }
        }
        state_ = State::Connecting;
        status_.store(ConnectionStatus::Connecting);
        error_.store(OnlineError::NoError);
        next_action_ms_ = now + CONNECT_TIMEOUT_MS;
        return;
    case State::Connecting:
        mqtt_client_.step();
        if (mqtt_client_.is_connected()) {
            state_ = State::Connected;
            status_.store(ConnectionStatus::Online);
            error_.store(OnlineError::NoError);
            next_action_ms_ = 0;
            backoff_.reset();
            subscribed_ = false;
            return;
        }
        if (mqtt_client_.last_error() == MQTT_ERROR_CONNECTION_REFUSED) {
            log_info(connect2, "mqtt auth rejected, registration required");
            status_.store(ConnectionStatus::AuthRequired);
            error_.store(OnlineError::Auth);
            state_ = State::RegistrationRequired;
            mqtt_client_.disconnect();
            telemetry_.reset();
            subscribed_ = false;
            next_action_ms_ = 0;
            return;
        }
        if (next_action_ms_ != 0 && ticks_diff(now, next_action_ms_) >= 0) {
            status_.store(ConnectionStatus::Error);
            error_.store(OnlineError::Connection);
            enter_backoff(now);
        }
        return;
    case State::Connected:
        status_.store(ConnectionStatus::Online);
        error_.store(OnlineError::NoError);
        mqtt_client_.step();
        if (!mqtt_client_.is_connected()) {
            status_.store(ConnectionStatus::Error);
            error_.store(mqtt_error_to_online_error(mqtt_client_.last_error()));
            enter_backoff(now);
            telemetry_.reset();
            subscribed_ = false;
            return;
        }
        if (!subscribed_ && ensure_subscribe_topics()) {
            subscribed_ = true;
        }
        flush_pending_events();
        telemetry_.tick(now, mqtt_client_);
        return;
    case State::Backoff:
        status_.store(ConnectionStatus::Error);
        if (next_action_ms_ != 0 && ticks_diff(now, next_action_ms_) >= 0) {
            state_ = State::Disconnected;
        }
        return;
    case State::RegistrationRequired:
        status_.store(ConnectionStatus::AuthRequired);
        error_.store(OnlineError::Auth);
        return;
    }
}

void Client::sleep_idle(uint32_t ms) {
    osDelay(ms);
}

void Client::refresh_config(uint32_t now_ms) {
    if (!is_idle_state(state_)) {
        return;
    }
    if (next_cfg_check_ms_ != 0 && ticks_diff(now_ms, next_cfg_check_ms_) < 0) {
        return;
    }
    next_cfg_check_ms_ = now_ms + CONFIG_REFRESH_MS;

    cfg_ = load_config();
    const uint32_t cfg_hash = config_hash(cfg_);
    if (cfg_hash == last_cfg_hash_) {
        return;
    }
    last_cfg_hash_ = cfg_hash;

    log_info(connect2, "cfg enabled=%d host=%s port=%u tls=%d custom_cert=%d oauth_tls=%d oauth_custom_cert=%d oauth_device_auth_url=%s oauth_token_url=%s",
        cfg_.enabled,
        cfg_.host,
        static_cast<unsigned>(cfg_.port),
        cfg_.tls,
        cfg_.custom_cert,
        cfg_.oauth_tls,
        cfg_.oauth_custom_cert,
        cfg_.oauth_device_auth_url,
        cfg_.oauth_token_url);

    mqtt_client_.disconnect();
    load_auth_from_storage();
    apply_auth_identity();
    telemetry_.reset();
    subscribed_ = false;
    registration_in_progress_ = false;
    backoff_.reset();
    if (!cfg_.enabled || cfg_.host[0] == '\0') {
        state_ = State::Disabled;
        next_action_ms_ = 0;
    } else {
        state_ = State::Disconnected;
        next_action_ms_ = 0;
    }
}

void Client::enter_backoff(uint32_t now_ms, uint32_t min_delay_ms) {
    mqtt_client_.disconnect();
    telemetry_.reset();
    subscribed_ = false;
    state_ = State::Backoff;
    const uint32_t delay_ms = std::max(backoff_.fail(), min_delay_ms);
    next_action_ms_ = now_ms + delay_ms;
}

uint32_t Client::config_hash(const Config &cfg) {
    uint32_t crc = 0;
    crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(cfg.host), strlen(cfg.host));
    crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(&cfg.port), sizeof(cfg.port));
    crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(&cfg.tls), sizeof(cfg.tls));
    crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(&cfg.custom_cert), sizeof(cfg.custom_cert));
    crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(&cfg.oauth_tls), sizeof(cfg.oauth_tls));
    crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(&cfg.oauth_custom_cert), sizeof(cfg.oauth_custom_cert));
    crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(&cfg.enabled), sizeof(cfg.enabled));
    crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(cfg.oauth_device_auth_url), strlen(cfg.oauth_device_auth_url));
    crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(cfg.oauth_token_url), strlen(cfg.oauth_token_url));
    return crc;
}

bool Client::run_oauth_device_flow() {
    serial_nr_t sn {};
    const uint8_t sn_len = otp_get_serial_nr(sn);
    const char *sn_value = (sn_len > 1 && sn[0] != '\0') ? sn.data() : "UNKNOWN_SN";

    buddy::oauth::DeviceFlowConfig oauth_cfg {};
    oauth_cfg.device_auth_url = cfg_.oauth_device_auth_url;
    oauth_cfg.token_url = cfg_.oauth_token_url;
    oauth_cfg.client_id = OAUTH_CLIENT_ID;
    oauth_cfg.scope = "mqtt";
    oauth_cfg.serial_number = sn_value;
    oauth_cfg.custom_cert = cfg_.oauth_custom_cert;
    oauth_cfg.initial_poll_interval_s = 2;
    oauth_cfg.poll_timeout_s = 30 * 60;
    oauth_cfg.max_poll_attempts = 0;

    static buddy::oauth::DeviceCode device_code {};
    device_code = {};
    buddy::oauth::Error error = buddy::oauth::Error::None;
    registration_info_valid_.store(false);
    verification_uri_[0] = '\0';
    user_code_[0] = '\0';
    verification_url_with_code_[0] = '\0';
    static buddy::oauth::Tokens tokens {};
    tokens = {};
    if (!buddy::oauth::run_device_flow(oauth_cfg, device_code, tokens, &error,
            &Client::oauth_device_code_ready_cb, this, &Client::oauth_should_abort_cb, this)) {
        if (error == buddy::oauth::Error::Canceled) {
            registration_in_progress_ = false;
            registration_info_valid_.store(false);
            verification_uri_[0] = '\0';
            user_code_[0] = '\0';
            verification_url_with_code_[0] = '\0';
            return false;
        }
        log_info(connect2, "oauth device flow failed: %s", buddy::oauth::to_str(error));
        error_.store(oauth_error_to_online_error(error));
        return false;
    }

    registration_info_valid_.store(false);
    verification_uri_[0] = '\0';
    user_code_[0] = '\0';
    verification_url_with_code_[0] = '\0';

    buddy::oauth::jwt::Error jwt_error = buddy::oauth::jwt::Error::None;
    char mqtt_username[sizeof(auth_.mqtt_username)] = {};
    if (!buddy::oauth::jwt::extract_mqtt_username(tokens.access_token, mqtt_username, sizeof(mqtt_username), &jwt_error)) {
        log_info(connect2, "oauth jwt parse failed: %s", buddy::oauth::jwt::to_str(jwt_error));
        error_.store(OnlineError::Protocol);
        return false;
    }

    const uint32_t now_epoch = static_cast<uint32_t>(time(nullptr));
    auth_ = {};
    strlcpy(auth_.access_token, tokens.access_token, sizeof(auth_.access_token));
    strlcpy(auth_.refresh_token, tokens.refresh_token, sizeof(auth_.refresh_token));
    strlcpy(auth_.mqtt_username, mqtt_username, sizeof(auth_.mqtt_username));
    auth_.expires_in_s = tokens.expires_in_s;
    auth_.obtained_at_epoch_s = now_epoch;
    auth_.expires_at_epoch_s = (tokens.expires_in_s > 0 && now_epoch > 0) ? (now_epoch + tokens.expires_in_s) : 0;
    if (!save_oauth_storage(auth_)) {
        error_.store(OnlineError::Internal);
        return false;
    }
    has_stored_auth_.store(true);
    apply_auth_identity();
    log_info(connect2, "oauth tokens stored user=%s access_len=%u refresh_len=%u expires_in=%lu",
        auth_.mqtt_username,
        static_cast<unsigned>(strlen(auth_.access_token)),
        static_cast<unsigned>(strlen(auth_.refresh_token)),
        static_cast<unsigned long>(auth_.expires_in_s));
    return true;
}

bool Client::run_oauth_refresh() {
    if (!has_valid_auth()) {
        return false;
    }

    buddy::oauth::DeviceFlowConfig oauth_cfg {};
    oauth_cfg.token_url = cfg_.oauth_token_url;
    oauth_cfg.client_id = OAUTH_CLIENT_ID;
    oauth_cfg.custom_cert = cfg_.oauth_custom_cert;

    // Keep large token buffers off the task stack (connect2 task is memory constrained).
    static buddy::oauth::Tokens refreshed {};
    refreshed = {};
    strlcpy(refreshed.refresh_token, auth_.refresh_token, sizeof(refreshed.refresh_token));
    buddy::oauth::Error error = buddy::oauth::Error::None;
    if (!buddy::oauth::refresh_tokens(oauth_cfg, auth_.refresh_token, refreshed, &error)) {
        log_info(connect2, "oauth refresh failed: %s", buddy::oauth::to_str(error));
        error_.store(oauth_error_to_online_error(error));
        return false;
    }

    buddy::oauth::jwt::Error jwt_error = buddy::oauth::jwt::Error::None;
    char mqtt_username[sizeof(auth_.mqtt_username)] = {};
    if (!buddy::oauth::jwt::extract_mqtt_username(refreshed.access_token, mqtt_username, sizeof(mqtt_username), &jwt_error)) {
        log_info(connect2, "oauth refresh jwt parse failed: %s", buddy::oauth::jwt::to_str(jwt_error));
        error_.store(OnlineError::Protocol);
        return false;
    }

    const uint32_t now_epoch = static_cast<uint32_t>(time(nullptr));
    strlcpy(auth_.access_token, refreshed.access_token, sizeof(auth_.access_token));
    strlcpy(auth_.refresh_token, refreshed.refresh_token, sizeof(auth_.refresh_token));
    strlcpy(auth_.mqtt_username, mqtt_username, sizeof(auth_.mqtt_username));
    auth_.expires_in_s = refreshed.expires_in_s;
    auth_.obtained_at_epoch_s = now_epoch;
    auth_.expires_at_epoch_s = (refreshed.expires_in_s > 0 && now_epoch > 0) ? (now_epoch + refreshed.expires_in_s) : 0;
    if (!save_oauth_storage(auth_)) {
        error_.store(OnlineError::Internal);
        return false;
    }
    has_stored_auth_.store(true);
    apply_auth_identity();
    log_info(connect2, "oauth refresh stored user=%s access_len=%u refresh_len=%u expires_in=%lu",
        auth_.mqtt_username,
        static_cast<unsigned>(strlen(auth_.access_token)),
        static_cast<unsigned>(strlen(auth_.refresh_token)),
        static_cast<unsigned long>(auth_.expires_in_s));
    return true;
}

void Client::on_oauth_device_code_ready(const buddy::oauth::DeviceCode &device_code) {
    registration_info_valid_.store(false);
    strlcpy(verification_uri_, device_code.verification_uri, sizeof(verification_uri_));
    strlcpy(user_code_, device_code.user_code, sizeof(user_code_));
    build_verification_url_with_code(verification_uri_, user_code_, verification_url_with_code_, sizeof(verification_url_with_code_));
    registration_info_valid_.store(true);

    log_info(connect2, "oauth user_code=%s verification_uri=%s", device_code.user_code, device_code.verification_uri);
}

void Client::oauth_device_code_ready_cb(const buddy::oauth::DeviceCode &device_code, void *ctx) {
    auto *client = static_cast<Client *>(ctx);
    if (client == nullptr) {
        return;
    }
    client->on_oauth_device_code_ready(device_code);
}

bool Client::oauth_should_abort_cb(void *ctx) {
    auto *client = static_cast<Client *>(ctx);
    if (client == nullptr) {
        return false;
    }
    return client->registration_cancel_requested_.load();
}

void Client::load_auth_from_storage() {
    auth_ = {};
    (void)load_oauth_storage(auth_);
    has_stored_auth_.store(has_valid_auth());
}

void Client::apply_auth_identity() {
    if (has_valid_auth()) {
        telemetry_.set_identity(auth_.mqtt_username);
    }
}

bool Client::has_valid_auth() const {
    return auth_.access_token[0] != '\0' && auth_.refresh_token[0] != '\0' && auth_.mqtt_username[0] != '\0';
}

bool Client::should_refresh_token(uint32_t now_epoch_s) const {
    if (!has_valid_auth()) {
        return false;
    }
    if (auth_.expires_in_s == 0 || auth_.obtained_at_epoch_s == 0 || now_epoch_s == 0) {
        return false;
    }
    if (auth_.expires_at_epoch_s == 0 || now_epoch_s >= auth_.expires_at_epoch_s) {
        return true;
    }

    // Avoid refreshing aggressively at token half-life on resource-constrained FW.
    // For current HW bring-up we only refresh shortly before expiry.
    constexpr uint32_t refresh_margin_s = 5 * 60;
    const uint32_t remaining_s = auth_.expires_at_epoch_s - now_epoch_s;
    return remaining_s <= refresh_margin_s;
}

bool Client::is_idle_state(State state) {
    return state == State::Disabled || state == State::Disconnected || state == State::Backoff || state == State::RegistrationRequired;
}

bool Client::network_ready() {
    auto iface_ready = [](uint32_t id) {
        if (netdev_get_status(id) != NETDEV_NETIF_UP) {
            return false;
        }
        lan_t addrs {};
        netdev_get_ipv4_addresses(id, &addrs);
        return addrs.addr_ip4.addr != 0;
    };
    return iface_ready(NETDEV_ETH_ID) || iface_ready(NETDEV_ESP_ID);
}

bool Client::ensure_subscribe_topics() {
    if (!telemetry_.build_command_topic(command_topic_, sizeof(command_topic_))) {
        return false;
    }
    if (!telemetry_.build_gcode_topic(gcode_topic_, sizeof(gcode_topic_))) {
        return false;
    }
    if (!telemetry_.build_transfer_topic(transfer_topic_, sizeof(transfer_topic_))) {
        return false;
    }
    if (!telemetry_.build_debug_command_topic(debug_topic_, sizeof(debug_topic_))) {
        return false;
    }
    if (!telemetry_.build_event_topic(event_topic_, sizeof(event_topic_))) {
        return false;
    }

    bool ok = true;
    ok = ok && mqtt_client_.subscribe(command_topic_, 1);
    ok = ok && mqtt_client_.subscribe(gcode_topic_, 1);
    ok = ok && mqtt_client_.subscribe(transfer_topic_, 1);
    ok = ok && mqtt_client_.subscribe(debug_topic_, 1);
    if (ok) {
        log_info(connect2, "subscribed cmd topics");
    }
    return ok;
}

void Client::handle_publish(const char *topic, size_t topic_len, const uint8_t *payload, size_t payload_len) {
    if (topic == nullptr || topic_len == 0) {
        return;
    }

    const auto topic_equals = [topic, topic_len](const char *candidate) {
        const size_t candidate_len = strlen(candidate);
        return candidate_len == topic_len && memcmp(topic, candidate, topic_len) == 0;
    };

    if (topic_equals(gcode_topic_)) {
        log_info(connect2, "rx mqtt topic=gcode bytes=%u", static_cast<unsigned>(payload_len));
        handle_gcode_topic(reinterpret_cast<const uint8_t *>(payload), payload_len);
        return;
    }

    if (topic_equals(command_topic_)) {
        log_info(connect2, "rx mqtt topic=cmd bytes=%u", static_cast<unsigned>(payload_len));
        handle_command_topic(reinterpret_cast<const uint8_t *>(payload), payload_len);
        return;
    }

    if (topic_equals(transfer_topic_)
        || topic_equals(debug_topic_)) {
        log_info(connect2, "rx mqtt topic=%.*s bytes=%u", static_cast<int>(topic_len), topic, static_cast<unsigned>(payload_len));
    }
}

bool Client::publish_event(const void *payload_struct, const pb_msgdesc_t *fields, size_t max_size) {
    if (!mqtt_client_.is_connected() || event_topic_[0] == '\0') {
        return false;
    }

    uint8_t buffer[768] = {};
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
    if (!pb_encode(&stream, fields, payload_struct)) {
        log_warning(connect2, "event publish encode failed");
        return false;
    }
    if (stream.bytes_written > max_size) {
        log_warning(connect2, "event publish too large bytes=%u limit=%u",
            static_cast<unsigned>(stream.bytes_written), static_cast<unsigned>(max_size));
        return false;
    }
    if (payload_struct != nullptr && fields == connect2_EventEnvelope_fields) {
        const auto *env = static_cast<const connect2_EventEnvelope *>(payload_struct);
        log_info(connect2, "tx mqtt topic=event event=%s command_id=%lu bytes=%u",
            event_type_name(env->event),
            static_cast<unsigned long>(env->has_command_id ? env->command_id : 0),
            static_cast<unsigned>(stream.bytes_written));
    }
    return enqueue_event_bytes(buffer, stream.bytes_written);
}

bool Client::enqueue_event_bytes(const uint8_t *payload, size_t payload_len) {
    if (payload == nullptr || payload_len == 0 || payload_len > pending_event_max_size_) {
        return false;
    }

    PendingEvent &slot = pending_events_[pending_event_tail_];
    if (slot.used) {
        log_warning(connect2, "event queue full, dropping event bytes=%u",
            static_cast<unsigned>(payload_len));
        return false;
    }

    memcpy(slot.data, payload, payload_len);
    slot.size = payload_len;
    slot.used = true;
    pending_event_tail_ = (pending_event_tail_ + 1) % pending_event_queue_size_;
    return true;
}

void Client::flush_pending_events() {
    while (pending_events_[pending_event_head_].used) {
        PendingEvent &slot = pending_events_[pending_event_head_];
        if (!mqtt_client_.publish_raw(event_topic_, slot.data, slot.size, MQTT_PUBLISH_QOS_1)) {
            break;
        }
        log_info(connect2, "tx mqtt topic=event bytes=%u",
            static_cast<unsigned>(slot.size));
        slot.used = false;
        slot.size = 0;
        pending_event_head_ = (pending_event_head_ + 1) % pending_event_queue_size_;
    }
}

bool Client::publish_finished_event(uint32_t command_id) {
    connect2_EventEnvelope env = connect2_EventEnvelope_init_zero;
    const char *state = current_state_string();
    env.event = connect2_EventType_EVENT_TYPE_FINISHED;
    env.state.funcs.encode = encode_string_field;
    env.state.arg = const_cast<char *>(state);
    env.has_command_id = true;
    env.command_id = command_id;
    env.which_payload = connect2_EventEnvelope_finished_tag;
    env.payload.finished = connect2_FinishedPayload_init_zero;
    return publish_event(&env, connect2_EventEnvelope_fields, 256);
}

bool Client::publish_rejected_event(uint32_t command_id, const char *reason) {
    connect2_EventEnvelope env = connect2_EventEnvelope_init_zero;
    const char *state = current_state_string();
    env.event = connect2_EventType_EVENT_TYPE_REJECTED;
    env.state.funcs.encode = encode_string_field;
    env.state.arg = const_cast<char *>(state);
    env.has_command_id = true;
    env.command_id = command_id;
    env.has_reason = false;
    env.which_payload = connect2_EventEnvelope_rejected_tag;
    env.payload.rejected = connect2_RejectedPayload_init_zero;
    if (reason != nullptr && reason[0] != '\0') {
        env.payload.rejected.reason.funcs.encode = encode_string_field;
        env.payload.rejected.reason.arg = const_cast<char *>(reason);
    }
    return publish_event(&env, connect2_EventEnvelope_fields, 384);
}

bool Client::publish_state_changed_event(uint32_t command_id) {
    connect2_EventEnvelope env = connect2_EventEnvelope_init_zero;
    const char *state = current_state_string();
    env.event = connect2_EventType_EVENT_TYPE_STATE_CHANGED;
    env.state.funcs.encode = encode_string_field;
    env.state.arg = const_cast<char *>(state);
    env.has_command_id = true;
    env.command_id = command_id;
    env.which_payload = connect2_EventEnvelope_state_changed_tag;
    env.payload.state_changed = connect2_StateChangedPayload_init_zero;
    return publish_event(&env, connect2_EventEnvelope_fields, 256);
}

bool Client::publish_job_info_event(uint32_t command_id, uint32_t start_cmd_id) {
    connect2_EventEnvelope env = connect2_EventEnvelope_init_zero;
    const char *state = current_state_string();
    env.event = connect2_EventType_EVENT_TYPE_JOB_INFO;
    env.state.funcs.encode = encode_string_field;
    env.state.arg = const_cast<char *>(state);
    env.has_command_id = true;
    env.command_id = command_id;
    env.which_payload = connect2_EventEnvelope_job_info_tag;
    env.payload.job_info = connect2_JobInfoPayload_init_zero;
    // Keep JOB_INFO lightweight and safe; richer fields can be added once PP3 command coverage grows.
    env.payload.job_info.has_job_id = false;
    env.payload.job_info.job_id = 0;
    env.payload.job_info.state.funcs.encode = encode_string_field;
    env.payload.job_info.state.arg = const_cast<char *>(state);
    env.payload.job_info.start_cmd_id = start_cmd_id;
    return publish_event(&env, connect2_EventEnvelope_fields, 384);
}

bool Client::publish_info_event(uint32_t command_id) {
    connect2_EventEnvelope env = connect2_EventEnvelope_init_zero;
    const auto &info = printer_info_snapshot();
    const char *state = current_state_string();
    env.event = connect2_EventType_EVENT_TYPE_INFO;
    env.state.funcs.encode = encode_string_field;
    env.state.arg = const_cast<char *>(state);
    env.has_command_id = true;
    env.command_id = command_id;
    env.which_payload = connect2_EventEnvelope_info_tag;
    env.payload.info = connect2_InfoPayload_init_zero;
    env.payload.info.firmware.funcs.encode = encode_string_field;
    env.payload.info.firmware.arg = const_cast<char *>(info.firmware_version);
    env.payload.info.printer_type.funcs.encode = encode_string_field;
    env.payload.info.printer_type.arg = const_cast<char *>(printer_type_name());
    env.payload.info.sn.funcs.encode = encode_string_field;
    env.payload.info.sn.arg = const_cast<char *>(info.serial_number.begin());
    env.payload.info.appendix = info.appendix;
    env.payload.info.fingerprint.funcs.encode = encode_string_field;
    env.payload.info.fingerprint.arg = const_cast<char *>(info.fingerprint);
    env.payload.info.nozzle_diameter = 0.0f;
    env.payload.info.transfer_paused = false;
    env.payload.info.has_network_info = false;
    env.payload.info.network_info = connect2_NetworkInfo_init_zero;
    env.payload.info.slots = 1;
    return publish_event(&env, connect2_EventEnvelope_fields, 512);
}

void Client::handle_command_topic(const uint8_t *payload, size_t payload_len) {
    marlin_client::init_maybe();

    connect2_CommandEnvelope env = connect2_CommandEnvelope_init_zero;
    char start_print_path[105] = {};
    StringDecodeTarget start_print_path_target { start_print_path, sizeof(start_print_path) };
    env.payload.start_print.path.funcs.decode = decode_string_field;
    env.payload.start_print.path.arg = &start_print_path_target;

    pb_istream_t stream = pb_istream_from_buffer(payload, payload_len);
    if (!pb_decode(&stream, connect2_CommandEnvelope_fields, &env)) {
        log_warning(connect2, "rx cmd rejected: decode failed");
        return;
    }
    log_info(connect2, "rx cmd command_id=%lu name=%s",
        static_cast<unsigned long>(env.command_id), command_payload_name(env.which_payload));

    switch (env.which_payload) {
    case connect2_CommandEnvelope_send_info_tag:
        if (!publish_info_event(env.command_id)) {
            publish_rejected_event(env.command_id, "SEND_INFO publish failed");
        }
        break;
    case connect2_CommandEnvelope_send_file_info_tag:
        publish_rejected_event(env.command_id, "SEND_FILE_INFO not implemented yet");
        break;
    case connect2_CommandEnvelope_send_job_info_tag:
        if (!publish_job_info_event(env.command_id, env.command_id)) {
            publish_rejected_event(env.command_id, "SEND_JOB_INFO publish failed");
        }
        break;
    case connect2_CommandEnvelope_send_transfer_info_tag:
        publish_rejected_event(env.command_id, "SEND_TRANSFER_INFO not implemented yet");
        break;
    case connect2_CommandEnvelope_send_state_info_tag:
        if (!publish_state_changed_event(env.command_id)) {
            publish_rejected_event(env.command_id, "SEND_STATE_INFO publish failed");
        }
        break;
    case connect2_CommandEnvelope_reset_printer_tag:
        publish_rejected_event(env.command_id, "RESET_PRINTER not implemented yet");
        break;
    case connect2_CommandEnvelope_set_token_tag:
        publish_rejected_event(env.command_id, "SET_TOKEN not implemented yet");
        break;
    case connect2_CommandEnvelope_set_printer_ready_tag:
        if (connect_client::MarlinPrinter::set_printer_ready(true)) {
            publish_state_changed_event(env.command_id);
        } else {
            publish_rejected_event(env.command_id, "Can't set ready now");
        }
        break;
    case connect2_CommandEnvelope_cancel_printer_ready_tag:
        (void)connect_client::MarlinPrinter::set_printer_ready(false);
        publish_finished_event(env.command_id);
        break;
    case connect2_CommandEnvelope_set_idle_tag:
        if (const auto state = printer_state::get_state(false);
            state == printer_state::DeviceState::Finished || state == printer_state::DeviceState::Stopped) {
            marlin_client::print_exit();
            publish_finished_event(env.command_id);
        } else {
            publish_rejected_event(env.command_id, "Can't set idle now");
        }
        break;
    case connect2_CommandEnvelope_pause_print_tag:
        if (printer_state::get_state(false) == printer_state::DeviceState::Printing) {
            marlin_client::print_pause();
            publish_finished_event(env.command_id);
        } else {
            publish_rejected_event(env.command_id, "No print to pause");
        }
        break;
    case connect2_CommandEnvelope_resume_print_tag:
        if (printer_state::get_state(false) == printer_state::DeviceState::Paused) {
            marlin_client::print_resume();
            publish_finished_event(env.command_id);
        } else {
            publish_rejected_event(env.command_id, "No paused print");
        }
        break;
    case connect2_CommandEnvelope_stop_print_tag:
        if (const auto state = printer_state::get_state(false);
            state == printer_state::DeviceState::Paused
            || state == printer_state::DeviceState::Printing
            || state == printer_state::DeviceState::Attention) {
            marlin_client::print_abort();
            publish_finished_event(env.command_id);
        } else {
            publish_rejected_event(env.command_id, "No print to stop");
        }
        break;
    case connect2_CommandEnvelope_start_print_tag: {
        if (start_print_path[0] == '\0') {
            publish_rejected_event(env.command_id, "Forbidden path");
            break;
        }
        if (!printer_state::remote_print_ready(false)) {
            publish_rejected_event(env.command_id, "Can't print now");
        } else {
            print_begin(start_print_path, marlin_server::PreviewSkipIfAble::all);
            if (marlin_client::is_print_started()) {
                publish_finished_event(env.command_id);
            } else {
                publish_rejected_event(env.command_id, "Can't print now");
            }
        }
        break;
    }
    default:
        log_info(connect2, "rx cmd unsupported payload=%u", static_cast<unsigned>(env.which_payload));
        break;
    }
}

void Client::handle_gcode_topic(const uint8_t *payload, size_t payload_len) {
    marlin_client::init_maybe();

    if (payload == nullptr || payload_len == 0) {
        log_warning(connect2, "rx gcode rejected: empty payload");
        publish_rejected_event(0, "Empty gcode payload");
        return;
    }

    connect2_GcodeCommandEnvelope msg = connect2_GcodeCommandEnvelope_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload, payload_len);
    if (!pb_decode(&stream, connect2_GcodeCommandEnvelope_fields, &msg)) {
        log_warning(connect2, "rx gcode rejected: decode failed");
        publish_rejected_event(0, "Gcode decode failed");
        return;
    }
    log_info(connect2, "rx gcode command_id=%lu force=%d len=%u",
        static_cast<unsigned long>(msg.command_id), msg.force, static_cast<unsigned>(strlen(msg.gcode)));

    if (msg.gcode[0] == '\0') {
        log_warning(connect2, "rx gcode rejected: empty gcode command_id=%lu", static_cast<unsigned long>(msg.command_id));
        publish_rejected_event(msg.command_id, "Empty gcode");
        return;
    }

    static char force_gcode_slots[FORCE_GCODE_SLOTS][FORCE_GCODE_MAX_LEN + 1] = {};
    static size_t next_force_gcode_slot = 0;

    bool accepted = false;
    const char *cursor = msg.gcode;
    while (*cursor != '\0') {
        const char *line_end = cursor;
        while (*line_end != '\0' && *line_end != '\n') {
            ++line_end;
        }

        char line[MARLIN_MAX_REQUEST + 1] = {};
        if (!trim_line(cursor, line_end, line, sizeof(line))) {
            log_warning(connect2, "rx gcode rejected: line too long command_id=%lu force=%d",
                static_cast<unsigned long>(msg.command_id), msg.force);
            publish_rejected_event(msg.command_id, "Gcode line too long");
            return;
        }

        if (line[0] != '\0') {
            normalize_gcode_line(line);
            if (msg.force) {
                char *slot = force_gcode_slots[next_force_gcode_slot];
                next_force_gcode_slot = (next_force_gcode_slot + 1) % FORCE_GCODE_SLOTS;
                strlcpy(slot, line, FORCE_GCODE_MAX_LEN + 1);
                marlin_client::inject(ConstexprString::from_str_runtime_unsafe(slot));
                accepted = true;
            } else {
                switch (marlin_client::gcode_try(line)) {
                case marlin_client::GcodeTryResult::Submitted:
                    accepted = true;
                    break;
                case marlin_client::GcodeTryResult::QueueFull:
                    log_warning(connect2, "rx gcode rejected: queue full command_id=%lu line=%s",
                        static_cast<unsigned long>(msg.command_id), line);
                    publish_rejected_event(msg.command_id, "Gcode queue full");
                    return;
                case marlin_client::GcodeTryResult::GcodeTooLong:
                    log_warning(connect2, "rx gcode rejected: marlin request too long command_id=%lu line=%s",
                        static_cast<unsigned long>(msg.command_id), line);
                    publish_rejected_event(msg.command_id, "Gcode too long");
                    return;
                }
            }
        }

        cursor = *line_end == '\n' ? (line_end + 1) : line_end;
    }

    if (!accepted) {
        log_warning(connect2, "rx gcode rejected: no executable lines command_id=%lu force=%d",
            static_cast<unsigned long>(msg.command_id), msg.force);
        publish_rejected_event(msg.command_id, "No executable gcode lines");
        return;
    }

    log_info(connect2, "rx gcode accepted command_id=%lu force=%d",
        static_cast<unsigned long>(msg.command_id), msg.force);
    publish_finished_event(msg.command_id);
}

void Client::publish_callback(void *ctx, const char *topic, size_t topic_len,
    const uint8_t *payload, size_t payload_len, uint8_t, bool, bool) {
    auto *self = static_cast<Client *>(ctx);
    if (self == nullptr) {
        return;
    }
    self->handle_publish(topic, topic_len, payload, payload_len);
}

} // namespace connect2_client
