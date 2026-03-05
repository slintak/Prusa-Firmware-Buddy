#include "client.hpp"

#include <cmsis_os.h>
#include <common/timing.h>
#include <cstring>
#include <common/crc32.h>
#include <algorithm>
#include <netdev.h>
#include <netif_settings.h>
#include <otp.hpp>
#include <common/oauth/jwt.hpp>
#include <support_utils.h>

#include <logging/log.hpp>
#include <ctime>

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
} // namespace

Client::Client(buddy::mqtt::Client &mqtt_client)
    : mqtt_client_(mqtt_client) {
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

OnlineStatus Client::last_status() const {
    return OnlineStatus { status_.load(), error_.load() };
}

bool Client::has_stored_auth() const {
    return has_stored_auth_.load();
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
        auth_ = {};
        (void)clear_oauth_storage();
        has_stored_auth_.store(false);
        state_ = State::Disconnected;
        next_action_ms_ = 0;
        backoff_.reset();
        log_info(connect2, "oauth manual registration requested");
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
            const uint32_t now_epoch = static_cast<uint32_t>(time(nullptr));
            if (!has_valid_auth()) {
                log_info(connect2, "oauth no stored credentials, starting device flow");
                if (!run_oauth_device_flow()) {
                    log_info(connect2, "oauth device flow failed");
                    status_.store(ConnectionStatus::Error);
                    enter_backoff(now, OAUTH_FAILURE_MIN_BACKOFF_MS);
                    return;
                }
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
            return;
        }
        if (mqtt_client_.last_error() == MQTT_ERROR_CONNECTION_REFUSED) {
            log_info(connect2, "mqtt auth rejected, registration required");
            status_.store(ConnectionStatus::AuthRequired);
            error_.store(OnlineError::Auth);
            state_ = State::RegistrationRequired;
            mqtt_client_.disconnect();
            telemetry_.reset();
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
            return;
        }
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

    log_info(connect2, "cfg enabled=%d host=%s port=%u tls=%d custom_cert=%d oauth_device_auth_url=%s oauth_token_url=%s",
        cfg_.enabled,
        cfg_.host,
        static_cast<unsigned>(cfg_.port),
        cfg_.tls,
        cfg_.custom_cert,
        cfg_.oauth_device_auth_url,
        cfg_.oauth_token_url);

    mqtt_client_.disconnect();
    load_auth_from_storage();
    apply_auth_identity();
    telemetry_.reset();
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
    oauth_cfg.custom_cert = cfg_.custom_cert;

    static buddy::oauth::DeviceCode device_code {};
    device_code = {};
    buddy::oauth::Error error = buddy::oauth::Error::None;
    if (!buddy::oauth::request_device_code(oauth_cfg, device_code, &error)) {
        log_info(connect2, "oauth device code failed: %s", buddy::oauth::to_str(error));
        error_.store(oauth_error_to_online_error(error));
        return false;
    }

    log_info(connect2, "oauth user_code=%s verification_uri=%s", device_code.user_code, device_code.verification_uri);

    static buddy::oauth::Tokens tokens {};
    tokens = {};
    if (!buddy::oauth::poll_tokens(oauth_cfg, device_code, tokens, &error)) {
        log_info(connect2, "oauth token polling failed: %s", buddy::oauth::to_str(error));
        error_.store(oauth_error_to_online_error(error));
        return false;
    }

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
    oauth_cfg.custom_cert = cfg_.custom_cert;

    buddy::oauth::Tokens refreshed {};
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
    const uint32_t refresh_at = auth_.obtained_at_epoch_s + (auth_.expires_in_s / 2);
    return now_epoch_s >= refresh_at;
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

} // namespace connect2_client
