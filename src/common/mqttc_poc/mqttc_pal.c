#include "mqttc_pal.h"

#include <errno.h>

#include "lwip/sockets.h"
#include "mqtt.h"

volatile int mqttc_pal_last_send_errno = 0;
volatile int mqttc_pal_last_recv_errno = 0;
volatile int mqttc_pal_last_send_rv = 0;
volatile int mqttc_pal_last_recv_rv = 0;
volatile uint32_t mqttc_pal_total_sent = 0;
volatile uint32_t mqttc_pal_total_recv = 0;

ssize_t mqtt_pal_sendall(mqtt_pal_socket_handle fd, const void *buf, size_t len, int flags) {
    size_t sent = 0;
    while (sent < len) {
        int rv = lwip_send(fd, (const char *)buf + sent, len - sent, flags | MSG_DONTWAIT);
        mqttc_pal_last_send_rv = rv;
        if (rv < 0) {
            mqttc_pal_last_send_errno = errno;
            if (sent > 0) {
                mqttc_pal_total_sent += sent;
                return (ssize_t)sent;
            }
            if (mqttc_pal_last_send_errno == EWOULDBLOCK || mqttc_pal_last_send_errno == EAGAIN) {
                return 0;
            }
            return MQTT_ERROR_SOCKET_ERROR;
        }
        if (rv == 0) {
            break;
        }
        sent += (size_t)rv;
    }
    mqttc_pal_total_sent += sent;
    return (ssize_t)sent;
}

ssize_t mqtt_pal_recvall(mqtt_pal_socket_handle fd, void *buf, size_t bufsz, int flags) {
    int rv = lwip_recv(fd, buf, bufsz, flags | MSG_DONTWAIT);
    mqttc_pal_last_recv_rv = rv;
    if (rv < 0) {
        mqttc_pal_last_recv_errno = errno;
        if (mqttc_pal_last_recv_errno == EWOULDBLOCK || mqttc_pal_last_recv_errno == EAGAIN) {
            return 0;
        }
        return MQTT_ERROR_SOCKET_ERROR;
    }
    if (rv > 0) {
        mqttc_pal_total_recv += (uint32_t)rv;
    }
    return (ssize_t)rv;
}
