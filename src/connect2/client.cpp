#include "client.hpp"

#include <cmsis_os.h>
#include <common/timing.h>
#include <cstring>
#include <common/crc32.h>
#include <netdev.h>
#include <netif_settings.h>

#include <logging/log.hpp>

LOG_COMPONENT_REF(connect2);

namespace connect2_client {

namespace {
constexpr uint32_t IDLE_DELAY_MS = 50;
constexpr uint32_t CONFIG_REFRESH_MS = 10000;
constexpr uint32_t CONNECT_TIMEOUT_MS = 60000;
constexpr uint16_t DEFAULT_PORT_PLAIN = 1883;
constexpr uint16_t DEFAULT_PORT_TLS = 8883;
constexpr uint32_t QUICK_RETRY_DELAY_MS = 1000;
constexpr uint8_t QUICK_RETRY_MAX = 3;
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
            state_ = State::Disconnected;
            next_action_ms_ = 0;
        }
        return;
    }

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
                enter_backoff(now, false);
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
            backoff_.reset();
            quick_retries_ = 0;
            return;
        }
        if (next_action_ms_ != 0 && ticks_diff(now, next_action_ms_) >= 0) {
            enter_backoff(now, false);
        }
        return;
    case State::Connected:
        mqtt_client_.step();
        if (!mqtt_client_.is_connected()) {
            enter_backoff(now, true);
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

    log_info(connect2, "cfg enabled=%d host=%s port=%u tls=%d custom_cert=%d",
        cfg_.enabled,
        cfg_.host,
        static_cast<unsigned>(cfg_.port),
        cfg_.tls,
        cfg_.custom_cert);

    mqtt_client_.disconnect();
    backoff_.reset();
    quick_retries_ = 0;
    if (!cfg_.enabled || cfg_.host[0] == '\0') {
        state_ = State::Disabled;
        next_action_ms_ = 0;
    } else {
        state_ = State::Disconnected;
        next_action_ms_ = 0;
    }
}

void Client::enter_backoff(uint32_t now_ms, bool fast_retry) {
    mqtt_client_.disconnect();
    state_ = State::Backoff;
    if (fast_retry && quick_retries_ < QUICK_RETRY_MAX) {
        ++quick_retries_;
        next_action_ms_ = now_ms + QUICK_RETRY_DELAY_MS;
        return;
    }
    quick_retries_ = 0;
    next_action_ms_ = now_ms + backoff_.fail();
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
