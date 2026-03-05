#include "mqtt_client.hpp"
#include "mqtt_transport.hpp"

#include <common/timing.h>
#include <logging/log.hpp>
#include <cstring>

LOG_COMPONENT_REF(mqtt);

namespace buddy::mqtt {

namespace {
constexpr uint32_t SYNC_PERIOD_MS = 50;
} // namespace

Client::Client() {
    owned_transport_ = std::make_unique<MqttTransport>();
    transport_ = owned_transport_.get();
}

void Client::set_config(const Config &cfg) {
    cfg_ = cfg;
}

void Client::set_transport(Transport *transport) {
    transport_ = transport;
    owned_transport_.reset();
}

bool Client::connect(const char *host, uint16_t port, bool tls, bool custom_cert,
    const char *username, const char *password,
    const char *will_topic, const char *will_payload, size_t will_payload_len,
    uint8_t will_qos, bool will_retain) {
    cfg_.host = host;
    cfg_.port = port;
    cfg_.tls = tls;
    cfg_.custom_cert = custom_cert;

    if (transport_ == nullptr) {
        return false;
    }

    if (!transport_->open(host, port, tls, custom_cert)) {
        return false;
    }

    mqtt_init(&client_, reinterpret_cast<mqtt_pal_socket_handle>(transport_), sendbuf_, sizeof(sendbuf_),
        recvbuf_, sizeof(recvbuf_), nullptr);

    const char *client_id = "connect2";
    uint8_t connect_flags = MQTT_CONNECT_CLEAN_SESSION;
    const bool have_will = will_topic != nullptr && will_payload != nullptr;
    if (have_will) {
        switch (will_qos) {
        case 1:
            connect_flags |= MQTT_CONNECT_WILL_QOS_1;
            break;
        case 2:
            connect_flags |= MQTT_CONNECT_WILL_QOS_2;
            break;
        default:
            connect_flags |= MQTT_CONNECT_WILL_QOS_0;
            break;
        }
        if (will_retain) {
            connect_flags |= MQTT_CONNECT_WILL_RETAIN;
        }
    }
    const enum MQTTErrors err =
        mqtt_connect(&client_, client_id,
            have_will ? will_topic : nullptr,
            have_will ? will_payload : nullptr,
            have_will ? will_payload_len : 0,
            username, password, connect_flags, 60);

    last_error_ = client_.error;
    if (err != MQTT_OK || client_.error != MQTT_OK) {
        log_info(mqtt, "mqtt_connect failed: %s (%d)", mqtt_error_str(client_.error), static_cast<int>(client_.error));
        transport_->close();
        return false;
    }

    initialized_ = true;
    connect_inflight_ = true;
    connected_ = false;
    last_sync_ms_ = 0;
    last_ping_ms_ = ticks_ms();
    return true;
}

void Client::disconnect() {
    if (transport_ != nullptr) {
        transport_->close();
    }
    connected_ = false;
    connect_inflight_ = false;
    initialized_ = false;
    last_ping_ms_ = 0;
}

void Client::step() {
    if (!initialized_ || transport_ == nullptr) {
        return;
    }

    const uint32_t now = ticks_ms();
    const bool need_sync = transport_->poll_readable(0) || last_sync_ms_ == 0 ||
        ticks_diff(now, last_sync_ms_) >= static_cast<int32_t>(SYNC_PERIOD_MS);
    if (!need_sync) {
        return;
    }
    last_sync_ms_ = now;

    const enum MQTTErrors err = mqtt_sync(&client_);
    last_error_ = client_.error;
    if (err != MQTT_OK) {
        log_info(mqtt, "mqtt_sync failed: %s (%d)", mqtt_error_str(client_.error), static_cast<int>(client_.error));
        disconnect();
        return;
    }

    if (connect_inflight_) {
        struct mqtt_queued_message *msg = mqtt_mq_find(&client_.mq, MQTT_CONTROL_CONNECT, nullptr);
        if (msg == nullptr) {
            connect_inflight_ = false;
            connected_ = true;
            last_ping_ms_ = now;
            log_info(mqtt, "mqtt connected");
        }
    }

    if (connected_) {
        const uint32_t keep_alive_s = client_.keep_alive;
        const uint32_t ping_interval_ms = keep_alive_s > 1 ? (keep_alive_s * 1000U) / 2U : 1000U;
        if (last_ping_ms_ != 0 && ticks_diff(now, last_ping_ms_) >= static_cast<int32_t>(ping_interval_ms)) {
            if (mqtt_mq_find(&client_.mq, MQTT_CONTROL_PINGREQ, nullptr) == nullptr) {
                const enum MQTTErrors ping_err = mqtt_ping(&client_);
                if (ping_err != MQTT_OK) {
                    log_info(mqtt, "mqtt_ping failed: %s (%d)", mqtt_error_str(client_.error), static_cast<int>(client_.error));
                } else {
                    last_ping_ms_ = now;
                }
            }
        }
    }
}

bool Client::is_connected() const {
    return connected_;
}

bool Client::publish(const char *topic, const char *payload, uint8_t publish_flags) {
    if (!initialized_ || !connected_ || connect_inflight_) {
        return false;
    }
    const enum MQTTErrors err = mqtt_publish(&client_, topic, payload, strlen(payload), publish_flags);
    return err == MQTT_OK;
}

enum MQTTErrors Client::last_error() const {
    return last_error_;
}

} // namespace buddy::mqtt
