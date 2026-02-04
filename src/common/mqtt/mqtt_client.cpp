#include "mqtt_client.hpp"
#include "mqtt_transport.hpp"

#include <common/timing.h>

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

bool Client::connect(const char *host, uint16_t port, bool tls, bool custom_cert) {
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
    const enum MQTTErrors err =
        mqtt_connect(&client_, client_id, nullptr, nullptr, 0, nullptr, nullptr, connect_flags, 60);

    if (err != MQTT_OK || client_.error != MQTT_OK) {
        transport_->close();
        return false;
    }

    initialized_ = true;
    connect_inflight_ = true;
    connected_ = false;
    last_sync_ms_ = 0;
    return true;
}

void Client::disconnect() {
    if (transport_ != nullptr) {
        transport_->close();
    }
    connected_ = false;
    connect_inflight_ = false;
    initialized_ = false;
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
    if (err != MQTT_OK) {
        disconnect();
        return;
    }

    if (connect_inflight_) {
        struct mqtt_queued_message *msg = mqtt_mq_find(&client_.mq, MQTT_CONTROL_CONNECT, nullptr);
        if (msg == nullptr) {
            connect_inflight_ = false;
            connected_ = true;
        }
    }
}

bool Client::is_connected() const {
    return connected_;
}

} // namespace buddy::mqtt
