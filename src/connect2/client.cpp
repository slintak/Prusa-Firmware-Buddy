#include "client.hpp"

#include <cmsis_os.h>
#include <common/timing.h>
#include <cstring>
#include <common/crc32.h>

#include <logging/log.hpp>

LOG_COMPONENT_REF(connect2);

namespace connect2_client {

namespace {
constexpr uint32_t IDLE_DELAY_MS = 50;
constexpr uint32_t CONFIG_REFRESH_MS = 10000;
constexpr uint32_t RECONNECT_BACKOFF_MS = 5000;
constexpr uint32_t CONNECT_TIMEOUT_MS = 5000;
constexpr uint16_t DEFAULT_PORT_PLAIN = 1883;
constexpr uint16_t DEFAULT_PORT_TLS = 8883;
} // namespace

Client::Client() = default;

void Client::run() {
    for (;;) {
        step();
        sleep_idle(IDLE_DELAY_MS);
    }
}

void Client::step() {
    const uint32_t now = ticks_ms();
    refresh_config(now);

    switch (state_) {
    case State::Disabled:
        return;
    case State::Disconnected:
        if (next_action_ms_ != 0 && ticks_diff(now, next_action_ms_) < 0) {
            return;
        }
        {
            const uint16_t port = cfg_.port != 0 ? cfg_.port : (cfg_.tls ? DEFAULT_PORT_TLS : DEFAULT_PORT_PLAIN);
            if (!mqtt_client_.connect(cfg_.host, port, cfg_.tls, cfg_.custom_cert)) {
                enter_backoff(now);
                return;
            }
        }
        state_ = State::Connecting;
        next_action_ms_ = now + CONNECT_TIMEOUT_MS;
        return;
    case State::Connecting:
        mqtt_client_.step();
        if (mqtt_client_.is_connected()) {
            state_ = State::Connected;
            next_action_ms_ = 0;
            return;
        }
        if (next_action_ms_ != 0 && ticks_diff(now, next_action_ms_) >= 0) {
            enter_backoff(now);
        }
        return;
    case State::Connected:
        mqtt_client_.step();
        if (!mqtt_client_.is_connected()) {
            enter_backoff(now);
        }
        return;
    case State::Backoff:
        if (next_action_ms_ != 0 && ticks_diff(now, next_action_ms_) >= 0) {
            state_ = State::Disconnected;
        }
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

    mqtt_client_.disconnect();
    if (!cfg_.enabled || cfg_.host[0] == '\0') {
        state_ = State::Disabled;
        next_action_ms_ = 0;
    } else {
        state_ = State::Disconnected;
        next_action_ms_ = 0;
    }
}

void Client::enter_backoff(uint32_t now_ms) {
    mqtt_client_.disconnect();
    state_ = State::Backoff;
    next_action_ms_ = now_ms + RECONNECT_BACKOFF_MS;
}

uint32_t Client::config_hash(const Config &cfg) {
    uint32_t crc = 0;
    crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(cfg.host), strlen(cfg.host));
    crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(&cfg.port), sizeof(cfg.port));
    crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(&cfg.tls), sizeof(cfg.tls));
    crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(&cfg.custom_cert), sizeof(cfg.custom_cert));
    crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(&cfg.enabled), sizeof(cfg.enabled));
    return crc;
}

bool Client::is_idle_state(State state) {
    return state == State::Disabled || state == State::Disconnected || state == State::Backoff;
}

} // namespace connect2_client
