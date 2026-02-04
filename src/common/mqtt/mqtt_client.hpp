#pragma once

#include <cstdint>
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
    bool connect(const char *host, uint16_t port, bool tls, bool custom_cert);
    void disconnect();
    void step();
    bool is_connected() const;

private:
    Config cfg_;
    bool connected_ = false;
    bool connect_inflight_ = false;
    bool initialized_ = false;
    uint32_t last_sync_ms_ = 0;
    std::unique_ptr<Transport> owned_transport_;
    Transport *transport_ = nullptr;
    mqtt_client client_ = {};
    uint8_t sendbuf_[4096] = {};
    uint8_t recvbuf_[2048] = {};
};

} // namespace buddy::mqtt
