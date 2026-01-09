#include "mqtt_poc.h"

#include "Marlin.h"

// Use existing HTTP socket wrapper
#include "common/http/socket.hpp"

// Network status
#include "netdev.h"

// NOTE: We intentionally use SERIAL_ECHO* for logging so it always appears on the USB console.

namespace {

static void mqtt_poc_msg(const char *msg) {
    SERIAL_ECHO_START();
    SERIAL_ECHOPGM("[MQTT_POC] ");
    SERIAL_ECHOLN(msg);
}

static void mqtt_poc_msg_kv(const char *key, int value) {
    SERIAL_ECHO_START();
    SERIAL_ECHOPGM("[MQTT_POC] ");
    SERIAL_ECHOPGM(key);
    SERIAL_ECHOPGM("=");
    SERIAL_ECHO(value);
    SERIAL_ECHOLN();
}

static void mqtt_poc_msg_text_u16(const char *msg, uint16_t v) {
    SERIAL_ECHO_START();
    SERIAL_ECHOPGM("[MQTT_POC] ");
    SERIAL_ECHOPGM(msg);
    SERIAL_ECHO((unsigned)v);
    SERIAL_ECHOLN();
}

static constexpr const char *BROKER_HOST = "192.168.1.2";
static constexpr uint16_t BROKER_PORT = 1883;

// Connection timeout (seconds) for underlying socket wrapper
static constexpr uint8_t CONNECT_TIMEOUT_S = 3;

// Wait this long after NETIF_UP before attempting connect (ms)
static constexpr millis_t WIFI_STABLE_DELAY_MS = 1500;

// Periodic MQTT publish interval (ms)
static constexpr millis_t PUBLISH_PERIOD_MS = 10000; // 10 s

// Backoff after a failure (ms)
static constexpr millis_t FAIL_BACKOFF_MS = 10000; // 10 s

// Minimal MQTT 3.1.1 CONNECT (clean session, keepalive 60, clientId "mk4poc")
static constexpr uint8_t mqtt_connect_pkt[] = {
    0x10, 0x12,                 // CONNECT, remaining length = 0x12 (18)
    0x00, 0x04, 'M','Q','T','T', // Protocol name
    0x04,                       // Protocol level 4 (MQTT 3.1.1)
    0x02,                       // Connect flags: Clean Session
    0x00, 0x3C,                 // Keepalive 60s
    0x00, 0x06, 'm','k','4','p','o','c' // Client ID
};

// QoS0 PUBLISH to topic "mk4/poc" payload "hello"
static constexpr uint8_t mqtt_publish_hello_pkt[] = {
    0x30, 0x0E,                               // PUBLISH, remaining length 14
    0x00, 0x07, 'm','k','4','/','p','o','c',  // Topic length + topic
    'h','e','l','l','o'                       // Payload
};

// DISCONNECT
static constexpr uint8_t mqtt_disconnect_pkt[] = { 0xE0, 0x00 };

static millis_t g_wifi_up_since_ms = 0;
static millis_t g_next_attempt_ms = 0;
static bool g_wifi_seen_up = false;

static bool mqtt_tx_all(http::socket_con &sock, const uint8_t *buf, size_t len, const char *tag) {
    const auto txr = sock.tx(buf, len);
    if (std::holds_alternative<http::Error>(txr)) {
        const int err = (int)std::get<http::Error>(txr);
        SERIAL_ECHO_START();
        SERIAL_ECHOPGM("[MQTT_POC] ");
        SERIAL_ECHOPGM(tag);
        SERIAL_ECHOPGM("_err=");
        SERIAL_ECHO(err);
        SERIAL_ECHOLN();
        return false;
    }

    const size_t sent = std::get<size_t>(txr);
    SERIAL_ECHO_START();
    SERIAL_ECHOPGM("[MQTT_POC] ");
    SERIAL_ECHOPGM(tag);
    SERIAL_ECHOPGM("_sent=");
    SERIAL_ECHO((unsigned)sent);
    SERIAL_ECHOLN();

    return sent == len;
}

static bool mqtt_send_connect(http::socket_con &sock) {
    return mqtt_tx_all(sock, mqtt_connect_pkt, sizeof(mqtt_connect_pkt), "connect");
}

static bool mqtt_wait_connack(http::socket_con &sock) {
    // Expected CONNACK: 0x20 0x02 0x00 0x00
    uint8_t resp[4] = {0, 0, 0, 0};
    size_t got = 0;

    while (got < sizeof(resp)) {
        auto rxr = sock.rx(resp + got, sizeof(resp) - got, /*nonblock=*/false);
        if (std::holds_alternative<http::Error>(rxr)) {
            const int err = (int)std::get<http::Error>(rxr);
            mqtt_poc_msg_kv("connack_rx_err", err);
            return false;
        }

        const size_t n = std::get<size_t>(rxr);
        if (n == 0) {
            mqtt_poc_msg("connack_rx_eof");
            return false;
        }
        got += n;
    }

    SERIAL_ECHO_START();
    SERIAL_ECHOPGM("[MQTT_POC] connack=");
    for (size_t i = 0; i < sizeof(resp); i++) {
        SERIAL_ECHO((unsigned)resp[i]);
        if (i + 1 < sizeof(resp)) SERIAL_CHAR(' ');
    }
    SERIAL_ECHOLN();

    if (resp[0] != 0x20 || resp[1] != 0x02) {
        mqtt_poc_msg("connack_bad_header");
        return false;
    }
    if (resp[3] != 0x00) {
        mqtt_poc_msg_kv("connack_rc", (int)resp[3]);
        return false;
    }

    mqtt_poc_msg("MQTT CONNACK OK");
    return true;
}

static bool mqtt_publish_hello(http::socket_con &sock) {
    mqtt_poc_msg("publishing hello...");
    if (!mqtt_tx_all(sock, mqtt_publish_hello_pkt, sizeof(mqtt_publish_hello_pkt), "publish")) {
        mqtt_poc_msg("PUBLISH failed");
        return false;
    }
    mqtt_poc_msg("PUBLISH OK");
    return true;
}

static void mqtt_disconnect(http::socket_con &sock) {
    mqtt_poc_msg("disconnecting...");
    (void)mqtt_tx_all(sock, mqtt_disconnect_pkt, sizeof(mqtt_disconnect_pkt), "disconnect");
    mqtt_poc_msg("DISCONNECT sent");
}

static void schedule_next_attempt_ok() {
    g_next_attempt_ms = millis() + PUBLISH_PERIOD_MS;
}

static void schedule_next_attempt_fail() {
    g_next_attempt_ms = millis() + FAIL_BACKOFF_MS;
}

static void do_one_publish_cycle() {
    mqtt_poc_msg("starting TCP connect...");
    mqtt_poc_msg_text_u16("broker_port=", BROKER_PORT);

    http::socket_con sock(CONNECT_TIMEOUT_S);
    const auto err = sock.connection(BROKER_HOST, BROKER_PORT);

    if (err.has_value()) {
        mqtt_poc_msg_kv("tcp_connect_err", (int)*err);
        schedule_next_attempt_fail();
        return;
    }

    mqtt_poc_msg("TCP connect OK");

    if (!mqtt_send_connect(sock)) {
        mqtt_poc_msg("MQTT CONNECT send failed");
        schedule_next_attempt_fail();
        return;
    }

    if (!mqtt_wait_connack(sock)) {
        mqtt_poc_msg("MQTT CONNACK failed");
        schedule_next_attempt_fail();
        return;
    }

    (void)mqtt_publish_hello(sock);
    mqtt_disconnect(sock);

    mqtt_poc_msg("cycle done");
    schedule_next_attempt_ok();
}

} // namespace

namespace buddy::mqtt_poc {

void reset() {
    g_wifi_up_since_ms = 0;
    g_next_attempt_ms = 0;
    g_wifi_seen_up = false;
    mqtt_poc_msg("reset()");
}

void tick() {
    const millis_t now = millis();

    // Gate on Wi-Fi UP
    const auto st = netdev_get_status(NETDEV_ESP_ID);
    if (st != NETDEV_NETIF_UP) {
        // Not up yet; keep waiting, reset the "stable" timer
        g_wifi_up_since_ms = 0;
        g_wifi_seen_up = false;
        return;
    }

    // First time we see UP, start stability timer
    if (!g_wifi_seen_up) {
        g_wifi_seen_up = true;
        g_wifi_up_since_ms = now;
        mqtt_poc_msg("wifi_up_detected");
        return;
    }

    // Wait for DHCP/routing settle
    if (!ELAPSED(now, g_wifi_up_since_ms + WIFI_STABLE_DELAY_MS)) {
        return;
    }

    // If we don't have a scheduled attempt yet, schedule immediate
    if (g_next_attempt_ms == 0) {
        g_next_attempt_ms = now; // immediate
    }

    // Not time yet
    if (!ELAPSED(now, g_next_attempt_ms)) {
        return;
    }

    // One connect+publish+disconnect cycle
    do_one_publish_cycle();
}

} // namespace buddy::mqtt_poc
