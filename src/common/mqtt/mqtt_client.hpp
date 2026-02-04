#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>

extern "C" {
#include "mqtt.h"
}

namespace buddy::mqtt {

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool open(const char *host, uint16_t port, bool tls, bool custom_cert) = 0;
    virtual void close() = 0;
    virtual int send(const uint8_t *data, size_t size) = 0;
    virtual int recv(uint8_t *data, size_t size) = 0;
    virtual bool poll_readable(uint32_t timeout_ms) = 0;
};

class Client {
public:
    struct Config {
        const char *host = "";
        uint16_t port = 0;
        bool tls = false;
        bool custom_cert = false;
    };

    Client();
    void set_config(const Config &cfg);
    void set_transport(Transport *transport);
    bool connect(const char *host, uint16_t port, bool tls, bool custom_cert,
        const char *will_topic = nullptr, const char *will_payload = nullptr, size_t will_payload_len = 0,
        uint8_t will_qos = 0, bool will_retain = false);
    void disconnect();
    void step();
    bool is_connected() const;
    bool subscribe(const char *topic, uint8_t qos);
    bool publish(const char *topic, const char *payload, uint8_t publish_flags);
    bool publish_raw(const char *topic, const uint8_t *payload, size_t payload_len, uint8_t publish_flags);

    using PublishCallback = void (*)(void *ctx, const char *topic, size_t topic_len,
        const uint8_t *payload, size_t payload_len, uint8_t qos, bool retain, bool dup);
    void set_publish_callback(PublishCallback cb, void *ctx);

private:
    static void publish_callback_thunk(void **state, struct mqtt_response_publish *publish);
    Config cfg_;
    bool connected_ = false;
    bool connect_inflight_ = false;
    bool initialized_ = false;
    PublishCallback publish_cb_ = nullptr;
    void *publish_cb_ctx_ = nullptr;
    uint32_t last_sync_ms_ = 0;
    uint32_t last_ping_ms_ = 0;
    std::unique_ptr<Transport> owned_transport_;
    Transport *transport_ = nullptr;
    mqtt_client client_ = {};
    uint8_t sendbuf_[4096] = {};
    uint8_t recvbuf_[2048] = {};
};

} // namespace buddy::mqtt
