#include "mqtt_transport.hpp"

#include <http/connect_error.h>

namespace buddy::mqtt {

namespace {
constexpr uint8_t HANDSHAKE_TIMEOUT_S = 60;
constexpr uint8_t IO_TIMEOUT_S = 5;
} // namespace

bool MqttTransport::open(const char *host, uint16_t port, bool tls, bool custom_cert) {
    close();
    if (tls) {
        tls_conn_ = std::make_unique<buddy::mqtt::tls>(HANDSHAKE_TIMEOUT_S, custom_cert);
        if (!tls_conn_) {
            return false;
        }
        const auto err = tls_conn_->connection(host, port, host, port);
        if (err.has_value()) {
            tls_conn_.reset();
            return false;
        }
        tls_conn_->set_io_timeout_s(IO_TIMEOUT_S);
        kind_ = Kind::Tls;
        return true;
    }

    plain_conn_ = std::make_unique<buddy::mqtt::socket_con>(HANDSHAKE_TIMEOUT_S);
    if (!plain_conn_) {
        return false;
    }
    const auto err = plain_conn_->connection(host, port);
    if (err.has_value()) {
        plain_conn_.reset();
        return false;
    }
    plain_conn_->set_timeout_s(IO_TIMEOUT_S);
    kind_ = Kind::Plain;
    return true;
}

void MqttTransport::close() {
    tls_conn_.reset();
    plain_conn_.reset();
    kind_ = Kind::None;
}

int MqttTransport::send(const uint8_t *data, size_t size) {
    if (kind_ == Kind::Tls && tls_conn_) {
        auto result = tls_conn_->tx(data, size);
        if (auto *amt = std::get_if<size_t>(&result); amt != nullptr) {
            return static_cast<int>(*amt);
        }
        auto err = std::get<http::Error>(result);
        if (err == http::Error::Timeout) {
            return 0;
        }
        close();
        return MQTT_ERROR_SOCKET_ERROR;
    }
    if (kind_ == Kind::Plain && plain_conn_) {
        auto result = plain_conn_->tx(data, size);
        if (auto *amt = std::get_if<size_t>(&result); amt != nullptr) {
            return static_cast<int>(*amt);
        }
        auto err = std::get<http::Error>(result);
        if (err == http::Error::Timeout) {
            return 0;
        }
        close();
        return MQTT_ERROR_SOCKET_ERROR;
    }
    return MQTT_ERROR_SOCKET_ERROR;
}

int MqttTransport::recv(uint8_t *data, size_t size) {
    if (kind_ == Kind::Tls && tls_conn_) {
        auto result = tls_conn_->rx(data, size, false);
        if (auto *amt = std::get_if<size_t>(&result); amt != nullptr) {
            return static_cast<int>(*amt);
        }
        auto err = std::get<http::Error>(result);
        if (err == http::Error::Timeout) {
            return 0;
        }
        close();
        return MQTT_ERROR_SOCKET_ERROR;
    }
    if (kind_ == Kind::Plain && plain_conn_) {
        auto result = plain_conn_->rx(data, size, false);
        if (auto *amt = std::get_if<size_t>(&result); amt != nullptr) {
            return static_cast<int>(*amt);
        }
        auto err = std::get<http::Error>(result);
        if (err == http::Error::Timeout) {
            return 0;
        }
        close();
        return MQTT_ERROR_SOCKET_ERROR;
    }
    return MQTT_ERROR_SOCKET_ERROR;
}

bool MqttTransport::poll_readable(uint32_t timeout_ms) {
    if (kind_ == Kind::Tls && tls_conn_) {
        return tls_conn_->poll_readable(timeout_ms);
    }
    if (kind_ == Kind::Plain && plain_conn_) {
        return plain_conn_->poll_readable(timeout_ms);
    }
    return false;
}

} // namespace buddy::mqtt
