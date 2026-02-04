#include "client.hpp"

#include <cmsis_os.h>
#include <common/timing.h>
#include <cstring>
#include <cstdio>
#include <common/crc32.h>
#include <netdev.h>
#include <netif_settings.h>

#include "command.hpp"
#include <logging/log.hpp>
#include <common/mutable_path.hpp>
#include <transfers/transfer_file_check.hpp>

LOG_COMPONENT_REF(connect2);

namespace connect2_client {

namespace {
constexpr uint32_t IDLE_DELAY_MS = 50;
constexpr uint32_t CONFIG_REFRESH_MS = 10000;
constexpr uint32_t CONNECT_TIMEOUT_MS = 60000;
constexpr uint16_t DEFAULT_PORT_PLAIN = 1883;
constexpr uint16_t DEFAULT_PORT_TLS = 8883;

bool path_allowed(const char *path) {
    constexpr const char *const usb = "/usb/";
    const bool is_on_usb = strncmp(path, usb, strlen(usb)) == 0 || strcmp(path, "/usb") == 0;
    const bool contains_upper = strstr(path, "/../") != nullptr;
    return is_on_usb && !contains_upper;
}

bool path_valid_file_or_transfer(const char *path) {
    MutablePath file(path);
    return transfers::is_valid_file_or_transfer(file);
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
            char will_topic[128];
            const bool have_will = telemetry_.build_online_topic(will_topic, sizeof(will_topic));
            const char *will_payload = "0";
            const size_t will_payload_len = strlen(will_payload);
            if (!mqtt_client_.connect(cfg_.host, port, cfg_.tls, cfg_.custom_cert,
                    have_will ? will_topic : nullptr,
                    have_will ? will_payload : nullptr,
                    have_will ? will_payload_len : 0,
                    1, true)) {
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
            backoff_.reset();
            subscribed_ = false;
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
            telemetry_.reset();
            subscribed_ = false;
            return;
        }
        if (!subscribed_ && ensure_command_topic()) {
            bool ok = true;
            ok = ok && mqtt_client_.subscribe(command_topic_, 1);
            if (ok) {
                subscribed_ = true;
            }
        }
        telemetry_.tick(now, mqtt_client_);
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
    telemetry_.reset();
    backoff_.reset();
    subscribed_ = false;
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
    telemetry_.reset();
    subscribed_ = false;
    state_ = State::Backoff;
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

bool Client::ensure_command_topic() {
    if (command_topic_[0] != '\0') {
        return true;
    }
    if (!telemetry_.build_command_topic(command_topic_, sizeof(command_topic_))) {
        return false;
    }
    command_topic_len_ = strlen(command_topic_);
    return true;
}

void Client::handle_publish(const char *topic, size_t topic_len, const uint8_t *payload, size_t payload_len) {
    if (!ensure_command_topic()) {
        return;
    }
    if (topic_len != command_topic_len_) {
        return;
    }
    if (memcmp(topic, command_topic_, topic_len) != 0) {
        return;
    }

    const DecodedCommand cmd = decode_command(payload, payload_len);
    if (cmd.type == CommandType::SendInfo) {
        telemetry_.publish_info_now(mqtt_client_, cmd.command_id);
    } else if (cmd.type == CommandType::SendFileInfo) {
        if (cmd.path[0] != '\0') {
            telemetry_.publish_file_info_now(mqtt_client_, cmd.path, cmd.command_id);
        }
    } else if (cmd.type == CommandType::StartPrint) {
        auto &printer = shared_printer();
        const auto params = printer.params();
        const char *reason = nullptr;
        if (cmd.path[0] == '\0' || !path_allowed(cmd.path)) {
            reason = "Forbidden path";
        } else if (!path_valid_file_or_transfer(cmd.path)) {
            reason = "File not found";
        } else if (const char *error = printer.start_print(cmd.path, std::nullopt); error != nullptr) {
            reason = error;
        }

        if (reason == nullptr) {
            telemetry_.publish_job_info_now(mqtt_client_, cmd.command_id, cmd.command_id, std::nullopt);
        } else {
            telemetry_.publish_rejected_now(mqtt_client_, printer, params, reason, cmd.command_id);
        }
    } else if (cmd.type == CommandType::SendJobInfo) {
        telemetry_.publish_job_info_now(mqtt_client_, cmd.command_id, cmd.command_id,
            cmd.job_id != 0 ? std::optional<uint32_t>(cmd.job_id) : std::nullopt);
    } else if (cmd.type == CommandType::PausePrint) {
        auto &printer = shared_printer();
        const auto params = printer.params();
        if (printer.job_control(connect_client::Printer::JobControl::Pause)) {
            telemetry_.publish_finished_now(mqtt_client_, cmd.command_id);
        } else {
            telemetry_.publish_rejected_now(mqtt_client_, printer, params, "No print to pause", cmd.command_id);
        }
    } else if (cmd.type == CommandType::ResumePrint) {
        auto &printer = shared_printer();
        const auto params = printer.params();
        if (printer.job_control(connect_client::Printer::JobControl::Resume)) {
            telemetry_.publish_finished_now(mqtt_client_, cmd.command_id);
        } else {
            telemetry_.publish_rejected_now(mqtt_client_, printer, params, "No paused print to resume", cmd.command_id);
        }
    } else if (cmd.type == CommandType::StopPrint) {
        auto &printer = shared_printer();
        const auto params = printer.params();
        if (printer.job_control(connect_client::Printer::JobControl::Stop)) {
            telemetry_.publish_finished_now(mqtt_client_, cmd.command_id);
        } else {
            telemetry_.publish_rejected_now(mqtt_client_, printer, params, "No print to stop", cmd.command_id);
        }
    } else if (cmd.type == CommandType::ResetPrinter) {
        auto &printer = shared_printer();
        const auto params = printer.params();
        printer.reset_printer();
        telemetry_.publish_rejected_now(mqtt_client_, printer, params, "Failed to reset", cmd.command_id);
    }
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
