#include "mqttc_poc.h"

#include "Marlin.h"
#include "netdev.h"

#include <connect/changes.hpp>
#include <connect/marlin_printer.hpp>
#include <marlin_server_shared.h>
#include <otp.hpp>
#include <state/printer_state.hpp>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>

extern "C" {
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mqtt.h"
}

namespace {

static void log_msg(const char *msg) {
    SERIAL_ECHO_START();
    SERIAL_ECHOPGM("[MQTT_C] ");
    SERIAL_ECHOLN(msg);
}

static void log_kv(const char *key, int v) {
    SERIAL_ECHO_START();
    SERIAL_ECHOPGM("[MQTT_C] ");
    SERIAL_ECHOPGM(key);
    SERIAL_ECHOPGM("=");
    SERIAL_ECHO(v);
    SERIAL_ECHOLN();
}

static void log_u32(const char *key, uint32_t v) {
    SERIAL_ECHO_START();
    SERIAL_ECHOPGM("[MQTT_C] ");
    SERIAL_ECHOPGM(key);
    SERIAL_ECHOPGM("=");
    SERIAL_ECHO(v);
    SERIAL_ECHOLN();
}

static constexpr const char *BROKER_IP = "192.168.1.112";
static constexpr uint16_t BROKER_PORT = 1883;

static constexpr millis_t WIFI_STABLE_DELAY_MS = 1500;
static constexpr millis_t RECONNECT_BACKOFF_MS = 5000;
static constexpr millis_t SYNC_PERIOD_MS = 20;
static constexpr millis_t CONNECT_TIMEOUT_MS = 5000;

static mqtt_client g_client;
static bool g_client_init = false;
static bool g_connect_inflight = false;
static int g_sock = -1;

static uint8_t g_sendbuf[4096];
static uint8_t g_recvbuf[2048];

static millis_t g_wifi_up_since_ms = 0;
static millis_t g_next_action_ms = 0;
static millis_t g_next_sync_ms = 0;
static millis_t g_connect_start_ms = 0;

static constexpr millis_t TELEMETRY_INTERVAL_MIN = 750;
static constexpr millis_t TELEMETRY_INTERVAL_LONG = 1000 * 4;
static constexpr millis_t TELEMETRY_INTERVAL_SHORT = 1000;
static constexpr millis_t TELEMETRY_INTERVAL_FULL = 1000 * 60 * 5;

static connect_client::Tracked g_telemetry_changes;
static millis_t g_last_telemetry_ms = 0;
static millis_t g_last_full_telemetry_ms = 0;
static bool g_online_published = false;

struct LastTelemetry {
    bool valid = false;
    char state[16] = {};
    float axis_z = 0.0f;
    uint32_t current_job = 0;
    float axis_x = 0.0f;
    float axis_y = 0.0f;
    float temp_nozzle = 0.0f;
    float target_nozzle = 0.0f;
    float temp_bed = 0.0f;
    float target_bed = 0.0f;
    int speed = 0;
    char material[filament_name_buffer_size] = {};
    bool material_valid = false;
    bool dialog_valid = false;
    uint32_t dialog_id = 0;

    bool job_valid = false;
    uint16_t job_id = 0;
    bool job_state_valid = false;
    char job_state[16] = {};
    bool job_progress_valid = false;
    uint8_t job_progress = 0;
    bool time_remaining_valid = false;
    uint32_t time_remaining = 0;
};

static LastTelemetry g_last_telemetry;

static void publish_callback(void **unused, struct mqtt_response_publish *published) {
    (void)unused;
    (void)published;
}

static const char *printer_id() {
    static char id[64] = {};
    static bool init = false;
    if (!init) {
        serial_nr_t serial {};
        const uint8_t serial_len = otp_get_serial_nr(serial);
        if (serial_len > 1 && serial[0] != '\0') {
            snprintf(id, sizeof(id), "%s", serial.data());
        } else {
            snprintf(id, sizeof(id), "%s", "DUMMY");
        }
        init = true;
    }
    return id;
}

static connect_client::MarlinPrinter &printer() {
    static connect_client::MarlinPrinter instance;
    return instance;
}

static void close_socket() {
    if (g_sock >= 0) {
        lwip_close(g_sock);
        g_sock = -1;
    }
}

static void reset_telemetry_state() {
    g_last_telemetry_ms = 0;
    g_last_full_telemetry_ms = 0;
    g_telemetry_changes.mark_dirty();
    g_online_published = false;
    g_last_telemetry = LastTelemetry {};
}

static void reset_client_state() {
    close_socket();
    g_client_init = false;
    g_connect_inflight = false;
    g_next_sync_ms = 0;
    g_connect_start_ms = 0;
    g_next_action_ms = millis() + RECONNECT_BACKOFF_MS;
    reset_telemetry_state();
}

static bool wifi_ready() {
    if (netdev_get_status(NETDEV_ESP_ID) != NETDEV_NETIF_UP) {
        g_wifi_up_since_ms = 0;
        return false;
    }

    const millis_t now = millis();
    if (g_wifi_up_since_ms == 0) {
        g_wifi_up_since_ms = now;
        log_msg("wifi_up_detected");
        return false;
    }

    if (!ELAPSED(now, g_wifi_up_since_ms + WIFI_STABLE_DELAY_MS)) {
        return false;
    }

    return true;
}

static int open_socket() {
    const int fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        log_msg("socket_create_failed");
        return -1;
    }

    const struct timeval timeout = { 1, 0 };
    (void)lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)lwip_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    ip4_addr_t ip4;
    if (!ip4addr_aton(BROKER_IP, &ip4)) {
        log_msg("broker_ip_parse_failed");
        lwip_close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(BROKER_PORT);
    addr.sin_addr.s_addr = ip4.addr;

    if (lwip_connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_msg("socket_connect_failed");
        lwip_close(fd);
        return -1;
    }

    return fd;
}

static uint8_t publish_flags(uint8_t qos, bool retain) {
    uint8_t flags = MQTT_PUBLISH_QOS_0;
    switch (qos) {
    case 1:
        flags = MQTT_PUBLISH_QOS_1;
        break;
    case 2:
        flags = MQTT_PUBLISH_QOS_2;
        break;
    default:
        flags = MQTT_PUBLISH_QOS_0;
        break;
    }
    if (retain) {
        flags |= MQTT_PUBLISH_RETAIN;
    }
    return flags;
}

static bool publish_topic(const char *topic, const char *payload, uint8_t qos, bool retain) {
    if (!g_client_init || g_connect_inflight) {
        return false;
    }

    const enum MQTTErrors err = mqtt_publish(&g_client, topic, payload, strlen(payload), publish_flags(qos, retain));
    if (err != MQTT_OK) {
        log_kv("mqtt_publish_err", (int)err);
        return false;
    }
    return true;
}

static bool make_printer_topic(char *buffer, size_t buffer_size, const char *suffix) {
    const int written = snprintf(buffer, buffer_size, "v1/devices/printers/%s/data%s", printer_id(), suffix);
    return written > 0 && static_cast<size_t>(written) < buffer_size;
}

static bool make_job_topic(char *buffer, size_t buffer_size, uint16_t job_id, const char *suffix) {
    const int written = snprintf(buffer, buffer_size, "v1/devices/printers/%s/jobs/%u/data%s", printer_id(), static_cast<unsigned int>(job_id), suffix);
    return written > 0 && static_cast<size_t>(written) < buffer_size;
}

static bool make_dialog_topic(char *buffer, size_t buffer_size) {
    const int written = snprintf(buffer, buffer_size, "v1/devices/printers/%s/dialog", printer_id());
    return written > 0 && static_cast<size_t>(written) < buffer_size;
}

static bool float_changed(float old_value, float new_value) {
    return std::fabs(old_value - new_value) > 0.01f;
}

static bool update_str(char *buffer, size_t buffer_size, const char *value) {
    if (strncmp(buffer, value, buffer_size) == 0) {
        return false;
    }
    strlcpy(buffer, value, buffer_size);
    return true;
}

static void publish_online(bool online) {
    char topic[128];
    if (!make_printer_topic(topic, sizeof(topic), "/online")) {
        return;
    }

    const char *payload = online ? "1" : "0";
    (void)publish_topic(topic, payload, 1, true);
}

static void publish_printer_value(const char *suffix, const char *payload, uint8_t qos, bool retain) {
    char topic[128];
    if (!make_printer_topic(topic, sizeof(topic), suffix)) {
        return;
    }
    (void)publish_topic(topic, payload, qos, retain);
}

static void publish_job_value(uint16_t job_id, const char *suffix, const char *payload, uint8_t qos, bool retain) {
    char topic[128];
    if (!make_job_topic(topic, sizeof(topic), job_id, suffix)) {
        return;
    }
    (void)publish_topic(topic, payload, qos, retain);
}

static void publish_printer_value_float(const char *suffix, float value, uint8_t precision, uint8_t qos, bool retain) {
    char payload[32];
    const char *fmt = (precision == 2) ? "%.2f" : "%.1f";
    snprintf(payload, sizeof(payload), fmt, static_cast<double>(value));
    publish_printer_value(suffix, payload, qos, retain);
}

static void publish_printer_value_int(const char *suffix, int value, uint8_t qos, bool retain) {
    char payload[32];
    snprintf(payload, sizeof(payload), "%d", value);
    publish_printer_value(suffix, payload, qos, retain);
}

static void publish_printer_value_u32(const char *suffix, uint32_t value, uint8_t qos, bool retain) {
    char payload[32];
    snprintf(payload, sizeof(payload), "%lu", static_cast<unsigned long>(value));
    publish_printer_value(suffix, payload, qos, retain);
}

static void publish_job_value_u32(uint16_t job_id, const char *suffix, uint32_t value, uint8_t qos, bool retain) {
    char payload[32];
    snprintf(payload, sizeof(payload), "%lu", static_cast<unsigned long>(value));
    publish_job_value(job_id, suffix, payload, qos, retain);
}

static void publish_dialog_id(const connect_client::Printer::Params &params) {
    if (!params.state.dialog.has_value()) {
        return;
    }

    char topic[128];
    if (!make_dialog_topic(topic, sizeof(topic))) {
        return;
    }

    const uint32_t dialog_id = params.state.dialog->dialog_id.to_uint32_t();
    if (!g_last_telemetry.dialog_valid || g_last_telemetry.dialog_id != dialog_id) {
        char payload[32];
        snprintf(payload, sizeof(payload), "%lu", static_cast<unsigned long>(dialog_id));
        (void)publish_topic(topic, payload, 2, true);
        g_last_telemetry.dialog_valid = true;
        g_last_telemetry.dialog_id = dialog_id;
    }
}

static bool telemetry_due(bool printing, const connect_client::Printer::Params &params, millis_t now, bool &want_full, bool &force_baseline, bool &force_full) {
    const bool changes = g_telemetry_changes.set_hash(params.telemetry_fingerprint(!printing));
    const millis_t since_telemetry = g_last_telemetry_ms == 0 ? (TELEMETRY_INTERVAL_FULL * 2) : (now - g_last_telemetry_ms);
    const millis_t since_full = g_last_full_telemetry_ms == 0 ? (TELEMETRY_INTERVAL_FULL * 2) : (now - g_last_full_telemetry_ms);
    const millis_t interval = printing ? TELEMETRY_INTERVAL_SHORT : TELEMETRY_INTERVAL_LONG;

    force_full = since_full >= TELEMETRY_INTERVAL_FULL;
    want_full = changes || force_full;
    const bool send = since_telemetry >= TELEMETRY_INTERVAL_MIN && (changes || since_telemetry >= interval);
    force_baseline = send;
    return send;
}

static void publish_telemetry(const connect_client::Printer::Params &params, bool full, bool force_baseline, bool force_full) {
    const auto &head = params.slots[params.preferred_head()];
    const char *state = printer_state::to_str(params.state.device_state);

    if (force_baseline || !g_last_telemetry.valid || update_str(g_last_telemetry.state, sizeof(g_last_telemetry.state), state)) {
        publish_printer_value("/state", state, 1, true);
    }

    if (force_baseline || !g_last_telemetry.valid || float_changed(g_last_telemetry.axis_z, params.pos[connect_client::Printer::Z_AXIS_POS])) {
        publish_printer_value_float("/axis-z", params.pos[connect_client::Printer::Z_AXIS_POS], 2, 0, true);
        g_last_telemetry.axis_z = params.pos[connect_client::Printer::Z_AXIS_POS];
    }

    const uint32_t current_job = params.has_job ? params.job_id : 0;
    if (force_baseline || !g_last_telemetry.valid || g_last_telemetry.current_job != current_job) {
        publish_printer_value_u32("/current-job", current_job, 1, true);
        g_last_telemetry.current_job = current_job;
    }

    if (params.has_job) {
        if (!g_last_telemetry.job_valid || g_last_telemetry.job_id != params.job_id) {
            g_last_telemetry.job_valid = true;
            g_last_telemetry.job_id = params.job_id;
            g_last_telemetry.job_state_valid = false;
            g_last_telemetry.job_progress_valid = false;
            g_last_telemetry.time_remaining_valid = false;
        }

        if (!g_last_telemetry.job_progress_valid || g_last_telemetry.job_progress != params.progress_percent) {
            publish_printer_value_u32("/job-progress", params.progress_percent, 0, true);
            publish_job_value_u32(params.job_id, "/progress", params.progress_percent, 0, true);
            g_last_telemetry.job_progress_valid = true;
            g_last_telemetry.job_progress = params.progress_percent;
        }

        if (!g_last_telemetry.job_state_valid || update_str(g_last_telemetry.job_state, sizeof(g_last_telemetry.job_state), state)) {
            publish_job_value(params.job_id, "/state", state, 1, true);
            g_last_telemetry.job_state_valid = true;
        }

        if (params.time_to_end != marlin_server::TIME_TO_END_INVALID) {
            if (!g_last_telemetry.time_remaining_valid || g_last_telemetry.time_remaining != params.time_to_end) {
                publish_job_value_u32(params.job_id, "/time-remaining", params.time_to_end, 0, true);
                g_last_telemetry.time_remaining_valid = true;
                g_last_telemetry.time_remaining = params.time_to_end;
            }
        }
    }

    if (full) {
        if (!params.has_job) {
            if (force_full || !g_last_telemetry.valid || float_changed(g_last_telemetry.axis_x, params.pos[connect_client::Printer::X_AXIS_POS])) {
                publish_printer_value_float("/axis-x", params.pos[connect_client::Printer::X_AXIS_POS], 2, 0, false);
                g_last_telemetry.axis_x = params.pos[connect_client::Printer::X_AXIS_POS];
            }
            if (force_full || !g_last_telemetry.valid || float_changed(g_last_telemetry.axis_y, params.pos[connect_client::Printer::Y_AXIS_POS])) {
                publish_printer_value_float("/axis-y", params.pos[connect_client::Printer::Y_AXIS_POS], 2, 0, false);
                g_last_telemetry.axis_y = params.pos[connect_client::Printer::Y_AXIS_POS];
            }
        }

        if (force_full || !g_last_telemetry.valid || float_changed(g_last_telemetry.temp_nozzle, head.temp_nozzle)) {
            publish_printer_value_float("/temp/nozzle/current", head.temp_nozzle, 1, 0, true);
            g_last_telemetry.temp_nozzle = head.temp_nozzle;
        }
        if (force_full || !g_last_telemetry.valid || float_changed(g_last_telemetry.target_nozzle, params.target_nozzle)) {
            publish_printer_value_float("/temp/nozzle/target", params.target_nozzle, 1, 1, true);
            g_last_telemetry.target_nozzle = params.target_nozzle;
        }
        if (force_full || !g_last_telemetry.valid || float_changed(g_last_telemetry.temp_bed, params.temp_bed)) {
            publish_printer_value_float("/temp/heatbed/current", params.temp_bed, 1, 0, true);
            g_last_telemetry.temp_bed = params.temp_bed;
        }
        if (force_full || !g_last_telemetry.valid || float_changed(g_last_telemetry.target_bed, params.target_bed)) {
            publish_printer_value_float("/temp/heatbed/target", params.target_bed, 1, 1, true);
            g_last_telemetry.target_bed = params.target_bed;
        }
        if (force_full || !g_last_telemetry.valid || g_last_telemetry.speed != params.print_speed) {
            publish_printer_value_int("/speed", params.print_speed, 1, true);
            g_last_telemetry.speed = params.print_speed;
        }

        if (params.slots[params.preferred_slot()].material.data()[0] != '\0') {
            if (!g_last_telemetry.material_valid
                || update_str(g_last_telemetry.material, sizeof(g_last_telemetry.material), params.slots[params.preferred_slot()].material.data())) {
                publish_printer_value("/material", params.slots[params.preferred_slot()].material.data(), 1, true);
                g_last_telemetry.material_valid = true;
            }
        }
    }

    publish_dialog_id(params);
    g_last_telemetry.valid = true;
}

static bool start_connection() {
    g_sock = open_socket();
    if (g_sock < 0) {
        return false;
    }

    mqtt_init(&g_client, g_sock, g_sendbuf, sizeof(g_sendbuf), g_recvbuf, sizeof(g_recvbuf), publish_callback);

    char will_topic[128];
    const char *will_payload = "0";
    const bool have_will = make_printer_topic(will_topic, sizeof(will_topic), "/online");
    const char *client_id = "mk4mqttc";
    uint8_t connect_flags = MQTT_CONNECT_CLEAN_SESSION;
    if (have_will) {
        connect_flags |= MQTT_CONNECT_WILL_RETAIN | MQTT_CONNECT_WILL_QOS_1;
    }
    mqtt_connect(&g_client, client_id, have_will ? will_topic : nullptr, have_will ? will_payload : nullptr,
        have_will ? strlen(will_payload) : 0, nullptr, nullptr, connect_flags, 60);

    if (g_client.error != MQTT_OK) {
        log_kv("mqtt_connect_err", (int)g_client.error);
        reset_client_state();
        return false;
    }

    g_client_init = true;
    g_connect_inflight = true;
    g_connect_start_ms = millis();
    log_msg("mqtt_connect_sent");
    if (mqtt_sync(&g_client) != MQTT_OK) {
        log_kv("mqtt_sync_err", (int)g_client.error);
        reset_client_state();
        return false;
    }
    return true;
}

} // namespace

namespace buddy::mqttc_poc {

void reset() {
    reset_client_state();
    g_wifi_up_since_ms = 0;
    log_msg("reset()");
}

void tick() {
    const millis_t now = millis();

    if (!wifi_ready()) {
        return;
    }

    if (!g_client_init) {
        if (g_next_action_ms == 0 || ELAPSED(now, g_next_action_ms)) {
            g_next_action_ms = now + RECONNECT_BACKOFF_MS;
            (void)start_connection();
        }
        return;
    }

    if (g_next_sync_ms == 0 || ELAPSED(now, g_next_sync_ms)) {
        g_next_sync_ms = now + SYNC_PERIOD_MS;
        const enum MQTTErrors err = mqtt_sync(&g_client);
        if (err != MQTT_OK) {
            log_kv("mqtt_sync_err", (int)err);
            reset_client_state();
            return;
        }
    }

    if (g_connect_inflight) {
        struct mqtt_queued_message *msg = mqtt_mq_find(&g_client.mq, MQTT_CONTROL_CONNECT, nullptr);
        if (msg == nullptr) {
            g_connect_inflight = false;
            log_msg("CONNECTED");
            publish_online(true);
            g_online_published = true;
        } else if (g_connect_start_ms != 0 && ELAPSED(now, g_connect_start_ms + CONNECT_TIMEOUT_MS)) {
            log_msg("connack_timeout");
            log_kv("last_send_rv", mqttc_pal_last_send_rv);
            log_kv("last_send_errno", mqttc_pal_last_send_errno);
            log_kv("last_recv_rv", mqttc_pal_last_recv_rv);
            log_kv("last_recv_errno", mqttc_pal_last_recv_errno);
            log_u32("total_sent", mqttc_pal_total_sent);
            log_u32("total_recv", mqttc_pal_total_recv);
            reset_client_state();
        }
        return;
    }

    if (!g_online_published) {
        publish_online(true);
        g_online_published = true;
    }

    bool want_full = false;
    bool force_full = false;
    const auto &client_printer = printer();
    const bool printing = client_printer.is_printing();
    const auto params = client_printer.params();
    bool force_baseline = false;
    if (telemetry_due(printing, params, now, want_full, force_baseline, force_full)) {
        publish_telemetry(params, want_full, force_baseline, force_full);
        g_last_telemetry_ms = now;
        if (want_full) {
            g_last_full_telemetry_ms = now;
            g_telemetry_changes.mark_clean();
        }
    }
}

} // namespace buddy::mqttc_poc
