#include "lwip_mqtt_poc.h"

#include "Marlin.h"
#include "netdev.h"


#include "mqtt_ca_cert.h"

#include <cstring>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string.h>

extern "C" {
#include "lwip/apps/mqtt.h"
#include "lwip/apps/mqtt_priv.h"
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"
#include "lwip/inet.h"
#include "lwip/altcp_tls.h"
}

#if !(LWIP_ALTCP && LWIP_ALTCP_TLS)
#error "LWIP_ALTCP_TLS not enabled in this TU"
#endif

namespace {

// ===== Logging (always visible on USB serial) =====
static void log_msg(const char *msg) {
    SERIAL_ECHO_START();
    SERIAL_ECHOPGM("[LWIP_MQTT] ");
    SERIAL_ECHOLN(msg);
}

static void log_kv(const char *key, int v) {
    SERIAL_ECHO_START();
    SERIAL_ECHOPGM("[LWIP_MQTT] ");
    SERIAL_ECHOPGM(key);
    SERIAL_ECHOPGM("=");
    SERIAL_ECHO(v);
    SERIAL_ECHOLN();
}

// ===== Static MQTT client instance (no dynamic allocation) =====
static mqtt_client_s g_mqtt_client;
static bool g_mqtt_client_in_use = false;

static mqtt_client_t *mqtt_client_new_static() {
    if (g_mqtt_client_in_use) {
        return nullptr;
    }
    memset(&g_mqtt_client, 0, sizeof(g_mqtt_client));
    g_mqtt_client_in_use = true;
    return (mqtt_client_t *)&g_mqtt_client;
}

// ===== Config =====
static struct altcp_tls_config* g_tls = nullptr;


static constexpr const char *BROKER_IP = "192.168.1.112";
static constexpr uint16_t BROKER_PORT = 8883;

static constexpr const char *TOPIC = "mk4/poc";
static constexpr const char *PAYLOAD = "hello world";

static constexpr millis_t WIFI_STABLE_DELAY_MS = 1500;
static constexpr millis_t PUBLISH_PERIOD_MS = 10000;
static constexpr millis_t RECONNECT_BACKOFF_MS = 5000;

// ===== Runtime state =====
static mqtt_client_t *g_client = nullptr;
static bool g_connect_inflight = false;
static bool g_connected = false;

static millis_t g_wifi_up_since_ms = 0;
static millis_t g_next_action_ms = 0;
static millis_t g_next_publish_ms = 0;

static ip_addr_t g_broker_addr;
static bool g_broker_addr_ok = false;

// Forward declarations of tcpip-thread callbacks
static void tcpip_do_connect(void *arg);
static void tcpip_do_publish(void *arg);
static void tcpip_do_disconnect(void *arg);

// ===== lwIP MQTT callbacks =====
static void connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status) {
    (void)client;
    (void)arg;

    // This callback runs in tcpip thread context.
    log_kv("conn_status", (int)status);

    g_connect_inflight = false;

    if (status == MQTT_CONNECT_ACCEPTED) {
        g_connected = true;
        g_next_publish_ms = millis();
        log_msg("CONNECTED");
    } else {
        g_connected = false;
        g_next_action_ms = millis() + RECONNECT_BACKOFF_MS;
        log_msg("CONNECT FAILED");
    }
}

static void request_cb(void *arg, err_t err) {
    (void)arg;
    if (err != ERR_OK) {
        log_kv("req_err", (int)err);
    }
}

// ===== Helpers =====
static bool wifi_ready() {
    if (netdev_get_status(NETDEV_ESP_ID) != NETDEV_NETIF_UP) {
        g_wifi_up_since_ms = 0;
        return false;
    }

    const millis_t now = millis();
    if (g_wifi_up_since_ms == 0) {
        g_wifi_up_since_ms = now;
        log_msg("wifi_up_detected");
        return false; // wait stability delay
    }

    if (!ELAPSED(now, g_wifi_up_since_ms + WIFI_STABLE_DELAY_MS)) {
        return false;
    }

    return true;
}

static void ensure_broker_addr_parsed() {
    if (g_broker_addr_ok) return;

    ip4_addr_t ip4;
    if (ip4addr_aton(BROKER_IP, &ip4)) {
        ip_addr_set_ip4_u32(&g_broker_addr, ip4.addr);
        g_broker_addr_ok = true;
        log_msg("broker_ip_parsed");
    } else {
        g_broker_addr_ok = false;
        log_msg("broker_ip_parse_failed");
    }
}

// ===== tcpip-thread callbacks =====

static void tcpip_do_connect(void *arg) {
    (void)arg;

    if (!g_tls) {
      g_tls = altcp_tls_create_config_client(
            (const u8_t*)MQTT_CA_CERT_PEM,
            strlen(MQTT_CA_CERT_PEM)
        );
      if (!g_tls) { log_msg("tls_cfg_fail"); return; }
      log_msg("tls_cfg_ok");
    }

    ensure_broker_addr_parsed();
    if (!g_broker_addr_ok) {
        g_next_action_ms = millis() + RECONNECT_BACKOFF_MS;
        return;
    }

    if (!g_client) {
        g_client = mqtt_client_new_static();
        if (!g_client) {
            log_msg("mqtt_client_new_failed");
            g_next_action_ms = millis() + RECONNECT_BACKOFF_MS;
            return;
        }
        log_msg("mqtt_client_new_ok");
    }

    if (mqtt_client_is_connected(g_client)) {
        g_connected = true;
        return;
    }

    // Prepare client info (MQTT 3.1.1)
    mqtt_connect_client_info_t ci;
    memset(&ci, 0, sizeof(ci));
    ci.tls_config = g_tls;
    ci.client_id = "mk4poc";
    ci.keep_alive = 60;

    g_connect_inflight = true;
    g_connected = false;

    const err_t e = mqtt_client_connect(g_client, &g_broker_addr, BROKER_PORT, connection_cb, nullptr, &ci);
    log_kv("mqtt_client_connect", (int)e);

    if (e != ERR_OK) {
        g_connect_inflight = false;
        g_connected = false;
        g_next_action_ms = millis() + RECONNECT_BACKOFF_MS;
    }
}

static void tcpip_do_publish(void *arg) {
    (void)arg;

    if (!g_client || !mqtt_client_is_connected(g_client)) {
        g_connected = false;
        return;
    }

    const err_t e = mqtt_publish(
        g_client,
        TOPIC,
        PAYLOAD,
        (u16_t)strlen(PAYLOAD),
        /*qos=*/0,
        /*retain=*/0,
        request_cb,
        nullptr);

    log_kv("mqtt_publish", (int)e);

    if (e != ERR_OK) {
        g_connected = false;
        g_next_action_ms = millis() + RECONNECT_BACKOFF_MS;
    }
}

static void tcpip_do_disconnect(void *arg) {
    (void)arg;

    if (g_client) {
        mqtt_disconnect(g_client);
    }

    // Reset runtime state
    g_connected = false;
    g_connect_inflight = false;
    g_next_action_ms = millis() + RECONNECT_BACKOFF_MS;
    g_next_publish_ms = 0;

    // Release static instance
    g_client = nullptr;
    g_mqtt_client_in_use = false;

    log_msg("disconnected");
}

} // namespace

namespace buddy::lwip_mqtt_poc {

void reset() {
    // Schedule disconnect in tcpip thread
    tcpip_callback(tcpip_do_disconnect, nullptr);

    g_wifi_up_since_ms = 0;
    g_next_action_ms = 0;
    g_next_publish_ms = 0;
    log_msg("reset()");
}

void tick() {
    const millis_t now = millis();

    if (!wifi_ready()) {
        return;
    }

    // Connection management
    if (!g_client || (!g_connected && !g_connect_inflight)) {
        if (g_next_action_ms == 0 || ELAPSED(now, g_next_action_ms)) {
            g_next_action_ms = now + RECONNECT_BACKOFF_MS;
            tcpip_callback(tcpip_do_connect, nullptr);
        }
        return;
    }

    // Periodic publish when connected
    if (g_connected && (g_next_publish_ms == 0 || ELAPSED(now, g_next_publish_ms))) {
        g_next_publish_ms = now + PUBLISH_PERIOD_MS;
        tcpip_callback(tcpip_do_publish, nullptr);
    }
}

} // namespace buddy::lwip_mqtt_poc
