#include "mqtt_client.hpp"

extern "C" {
#include "mqtt.h"
}

using buddy::mqtt::Transport;

extern "C" ssize_t mqtt_pal_sendall(mqtt_pal_socket_handle fd, const void *buf, size_t len, int flags) {
    (void)flags;
    auto *transport = reinterpret_cast<Transport *>(fd);
    if (transport == nullptr) {
        return MQTT_ERROR_SOCKET_ERROR;
    }
    return transport->send(static_cast<const uint8_t *>(buf), len);
}

extern "C" ssize_t mqtt_pal_recvall(mqtt_pal_socket_handle fd, void *buf, size_t bufsz, int flags) {
    (void)flags;
    auto *transport = reinterpret_cast<Transport *>(fd);
    if (transport == nullptr) {
        return MQTT_ERROR_SOCKET_ERROR;
    }
    return transport->recv(static_cast<uint8_t *>(buf), bufsz);
}
