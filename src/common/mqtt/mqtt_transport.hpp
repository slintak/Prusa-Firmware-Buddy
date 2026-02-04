#pragma once

#include <common/mqtt/mqtt_client.hpp>
#include <common/http/socket.hpp>

#include "tls/tls.hpp"

#include <memory>

namespace buddy::mqtt {

class MqttTransport final : public Transport {
public:
    bool open(const char *host, uint16_t port, bool tls, bool custom_cert) override;
    void close() override;
    int send(const uint8_t *data, size_t size) override;
    int recv(uint8_t *data, size_t size) override;
    bool poll_readable(uint32_t timeout_ms) override;

private:
    enum class Kind : uint8_t {
        None,
        Plain,
        Tls,
    };

    Kind kind_ = Kind::None;
    std::unique_ptr<tls> tls_conn_;
    std::unique_ptr<http::socket_con> plain_conn_;
};

} // namespace buddy::mqtt
