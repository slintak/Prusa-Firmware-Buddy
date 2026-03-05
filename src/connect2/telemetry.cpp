#include "telemetry.hpp"

#include <connect/marlin_printer.hpp>
#include <connect/printer_common.hpp>
#include <marlin_server_shared.h>
#include <otp.hpp>
#include <state/printer_state.hpp>

#include <common/mqtt/mqtt_client.hpp>
#include <support_utils.h>

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include "mqtt.h"
}

namespace connect2_client {

namespace {
constexpr uint32_t TELEMETRY_INTERVAL_MIN = 750;
constexpr uint32_t TELEMETRY_INTERVAL_LONG = 1000 * 4;
constexpr uint32_t TELEMETRY_INTERVAL_SHORT = 1000;
constexpr uint32_t TELEMETRY_INTERVAL_FULL = 1000 * 60 * 5;

connect_client::MarlinPrinter &printer() {
    static connect_client::MarlinPrinter instance;
    return instance;
}

bool float_changed(float old_value, float new_value) {
    return std::fabs(old_value - new_value) > 0.01f;
}

bool update_str(char *buffer, size_t buffer_size, const char *value) {
    if (strncmp(buffer, value, buffer_size) == 0) {
        return false;
    }
    strlcpy(buffer, value, buffer_size);
    return true;
}

void default_printer_id(char *out, size_t out_size) {
    serial_nr_t serial {};
    const uint8_t serial_len = otp_get_serial_nr(serial);
    if (serial_len > 1 && serial[0] != '\0') {
        snprintf(out, out_size, "%s", serial.data());
    } else {
        snprintf(out, out_size, "%s", "DUMMY");
    }
}

bool make_printer_topic(char *buffer, size_t buffer_size, const char *printer_id, const char *suffix) {
    const int written = snprintf(buffer, buffer_size, "v1/devices/printers/%s/data%s", printer_id, suffix);
    return written > 0 && static_cast<size_t>(written) < buffer_size;
}

bool make_job_topic(char *buffer, size_t buffer_size, const char *printer_id, uint16_t job_id, const char *suffix) {
    const int written = snprintf(buffer, buffer_size, "v1/devices/printers/%s/jobs/%u/data%s", printer_id,
        static_cast<unsigned int>(job_id), suffix);
    return written > 0 && static_cast<size_t>(written) < buffer_size;
}

bool make_dialog_topic(char *buffer, size_t buffer_size, const char *printer_id) {
    const int written = snprintf(buffer, buffer_size, "v1/devices/printers/%s/dialog", printer_id);
    return written > 0 && static_cast<size_t>(written) < buffer_size;
}

uint8_t publish_flags(uint8_t qos, bool retain) {
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

bool publish_topic(buddy::mqtt::Client &mqtt_client, const char *topic, const char *payload, uint8_t qos, bool retain) {
    return mqtt_client.publish(topic, payload, publish_flags(qos, retain));
}

void publish_printer_value(buddy::mqtt::Client &mqtt_client, const char *printer_id, const char *suffix, const char *payload, uint8_t qos, bool retain) {
    char topic[128];
    if (!make_printer_topic(topic, sizeof(topic), printer_id, suffix)) {
        return;
    }
    (void)publish_topic(mqtt_client, topic, payload, qos, retain);
}

void publish_job_value(buddy::mqtt::Client &mqtt_client, const char *printer_id, uint16_t job_id, const char *suffix, const char *payload, uint8_t qos, bool retain) {
    char topic[128];
    if (!make_job_topic(topic, sizeof(topic), printer_id, job_id, suffix)) {
        return;
    }
    (void)publish_topic(mqtt_client, topic, payload, qos, retain);
}

void publish_printer_value_float(buddy::mqtt::Client &mqtt_client, const char *printer_id, const char *suffix, float value, uint8_t precision, uint8_t qos, bool retain) {
    char payload[32];
    const char *fmt = (precision == 2) ? "%.2f" : "%.1f";
    snprintf(payload, sizeof(payload), fmt, static_cast<double>(value));
    publish_printer_value(mqtt_client, printer_id, suffix, payload, qos, retain);
}

void publish_printer_value_int(buddy::mqtt::Client &mqtt_client, const char *printer_id, const char *suffix, int value, uint8_t qos, bool retain) {
    char payload[32];
    snprintf(payload, sizeof(payload), "%d", value);
    publish_printer_value(mqtt_client, printer_id, suffix, payload, qos, retain);
}

void publish_printer_value_u32(buddy::mqtt::Client &mqtt_client, const char *printer_id, const char *suffix, uint32_t value, uint8_t qos, bool retain) {
    char payload[32];
    snprintf(payload, sizeof(payload), "%lu", static_cast<unsigned long>(value));
    publish_printer_value(mqtt_client, printer_id, suffix, payload, qos, retain);
}

void publish_job_value_u32(buddy::mqtt::Client &mqtt_client, const char *printer_id, uint16_t job_id, const char *suffix, uint32_t value, uint8_t qos, bool retain) {
    char payload[32];
    snprintf(payload, sizeof(payload), "%lu", static_cast<unsigned long>(value));
    publish_job_value(mqtt_client, printer_id, job_id, suffix, payload, qos, retain);
}

} // namespace

Telemetry::Telemetry() {
    default_printer_id(printer_id_, sizeof(printer_id_));
    telemetry_changes_.mark_dirty();
}

bool Telemetry::build_online_topic(char *buffer, size_t buffer_size) const {
    return make_printer_topic(buffer, buffer_size, printer_id_, "/online");
}

void Telemetry::set_identity(const char *identity) {
    if (identity == nullptr || identity[0] == '\0') {
        return;
    }
    if (strncmp(printer_id_, identity, sizeof(printer_id_)) == 0) {
        return;
    }
    strlcpy(printer_id_, identity, sizeof(printer_id_));
    reset();
}

void Telemetry::reset() {
    last_telemetry_ms_ = 0;
    last_full_telemetry_ms_ = 0;
    telemetry_changes_.mark_dirty();
    online_published_ = false;
    last_ = LastTelemetry {};
}

bool Telemetry::telemetry_due(bool printing, const connect_client::Printer::Params &params, uint32_t now_ms,
    bool &want_full, bool &force_baseline, bool &force_full) {
    const bool changes = telemetry_changes_.set_hash(params.telemetry_fingerprint(!printing));
    const uint32_t since_telemetry = last_telemetry_ms_ == 0 ? (TELEMETRY_INTERVAL_FULL * 2) : (now_ms - last_telemetry_ms_);
    const uint32_t since_full = last_full_telemetry_ms_ == 0 ? (TELEMETRY_INTERVAL_FULL * 2) : (now_ms - last_full_telemetry_ms_);
    const uint32_t interval = printing ? TELEMETRY_INTERVAL_SHORT : TELEMETRY_INTERVAL_LONG;

    force_full = since_full >= TELEMETRY_INTERVAL_FULL;
    want_full = changes || force_full;
    const bool send = since_telemetry >= TELEMETRY_INTERVAL_MIN && (changes || since_telemetry >= interval);
    force_baseline = send;
    return send;
}

bool Telemetry::publish_online(bool online, buddy::mqtt::Client &mqtt_client) {
    char topic[128];
    if (!make_printer_topic(topic, sizeof(topic), printer_id_, "/online")) {
        return false;
    }
    const char *payload = online ? "1" : "0";
    return publish_topic(mqtt_client, topic, payload, 1, true);
}

void Telemetry::publish_telemetry(const connect_client::Printer::Params &params, bool full,
    bool force_full, buddy::mqtt::Client &mqtt_client) {
    const auto &head = params.slots[params.preferred_head()];
    const char *state = printer_state::to_str(params.state.device_state);

    if (!last_.valid || update_str(last_.state, sizeof(last_.state), state)) {
        publish_printer_value(mqtt_client, printer_id_, "/state", state, 1, true);
    }

    if (!last_.valid || float_changed(last_.axis_z, params.pos[connect_client::Printer::Z_AXIS_POS])) {
        publish_printer_value_float(mqtt_client, printer_id_, "/axis-z", params.pos[connect_client::Printer::Z_AXIS_POS], 2, 0, true);
        last_.axis_z = params.pos[connect_client::Printer::Z_AXIS_POS];
    }

    const uint32_t current_job = params.has_job ? params.job_id : 0;
    if (!last_.valid || last_.current_job != current_job) {
        publish_printer_value_u32(mqtt_client, printer_id_, "/current-job", current_job, 1, true);
        last_.current_job = current_job;
    }

    if (params.has_job) {
        if (!last_.job_valid || last_.job_id != params.job_id) {
            last_.job_valid = true;
            last_.job_id = params.job_id;
            last_.job_state_valid = false;
            last_.job_progress_valid = false;
            last_.time_remaining_valid = false;
        }

        if (!last_.job_progress_valid || last_.job_progress != params.progress_percent) {
            publish_printer_value_u32(mqtt_client, printer_id_, "/job-progress", params.progress_percent, 0, true);
            publish_job_value_u32(mqtt_client, printer_id_, params.job_id, "/progress", params.progress_percent, 0, true);
            last_.job_progress_valid = true;
            last_.job_progress = params.progress_percent;
        }

        if (!last_.job_state_valid || update_str(last_.job_state, sizeof(last_.job_state), state)) {
            publish_job_value(mqtt_client, printer_id_, params.job_id, "/state", state, 1, true);
            last_.job_state_valid = true;
        }

        if (params.time_to_end != marlin_server::TIME_TO_END_INVALID) {
            if (!last_.time_remaining_valid || last_.time_remaining != params.time_to_end) {
                publish_job_value_u32(mqtt_client, printer_id_, params.job_id, "/time-remaining", params.time_to_end, 0, true);
                last_.time_remaining_valid = true;
                last_.time_remaining = params.time_to_end;
            }
        }
    }

    if (!params.has_job) {
        if (!last_.valid || float_changed(last_.axis_x, params.pos[connect_client::Printer::X_AXIS_POS])) {
            publish_printer_value_float(mqtt_client, printer_id_, "/axis-x", params.pos[connect_client::Printer::X_AXIS_POS], 2, 0, false);
            last_.axis_x = params.pos[connect_client::Printer::X_AXIS_POS];
        }
        if (!last_.valid || float_changed(last_.axis_y, params.pos[connect_client::Printer::Y_AXIS_POS])) {
            publish_printer_value_float(mqtt_client, printer_id_, "/axis-y", params.pos[connect_client::Printer::Y_AXIS_POS], 2, 0, false);
            last_.axis_y = params.pos[connect_client::Printer::Y_AXIS_POS];
        }
    }

    if (full) {
        if (force_full || !last_.valid || float_changed(last_.temp_nozzle, head.temp_nozzle)) {
            publish_printer_value_float(mqtt_client, printer_id_, "/temp/nozzle/current", head.temp_nozzle, 1, 0, true);
            last_.temp_nozzle = head.temp_nozzle;
        }
        if (force_full || !last_.valid || float_changed(last_.target_nozzle, params.target_nozzle)) {
            publish_printer_value_float(mqtt_client, printer_id_, "/temp/nozzle/target", params.target_nozzle, 1, 1, true);
            last_.target_nozzle = params.target_nozzle;
        }
        if (force_full || !last_.valid || float_changed(last_.temp_bed, params.temp_bed)) {
            publish_printer_value_float(mqtt_client, printer_id_, "/temp/heatbed/current", params.temp_bed, 1, 0, true);
            last_.temp_bed = params.temp_bed;
        }
        if (force_full || !last_.valid || float_changed(last_.target_bed, params.target_bed)) {
            publish_printer_value_float(mqtt_client, printer_id_, "/temp/heatbed/target", params.target_bed, 1, 1, true);
            last_.target_bed = params.target_bed;
        }
        if (force_full || !last_.valid || last_.speed != params.print_speed) {
            publish_printer_value_int(mqtt_client, printer_id_, "/speed", params.print_speed, 1, true);
            last_.speed = params.print_speed;
        }

        if (params.slots[params.preferred_slot()].material.data()[0] != '\0') {
            if (!last_.material_valid || update_str(last_.material, sizeof(last_.material),
                    params.slots[params.preferred_slot()].material.data())) {
                publish_printer_value(mqtt_client, printer_id_, "/material", params.slots[params.preferred_slot()].material.data(), 1, true);
                last_.material_valid = true;
            }
        }
    }

    if (params.state.dialog.has_value()) {
        char topic[128];
        if (make_dialog_topic(topic, sizeof(topic), printer_id_)) {
            const uint32_t dialog_id = params.state.dialog->dialog_id.to_uint32_t();
            if (!last_.dialog_valid || last_.dialog_id != dialog_id) {
                char payload[32];
                snprintf(payload, sizeof(payload), "%lu", static_cast<unsigned long>(dialog_id));
                (void)publish_topic(mqtt_client, topic, payload, 2, true);
                last_.dialog_valid = true;
                last_.dialog_id = dialog_id;
            }
        }
    }

    last_.valid = true;
}

void Telemetry::tick(uint32_t now_ms, buddy::mqtt::Client &mqtt_client) {
    if (!mqtt_client.is_connected()) {
        return;
    }

    if (!online_published_) {
        (void)publish_online(true, mqtt_client);
        online_published_ = true;
    }

    const auto &client_printer = printer();
    const bool printing = client_printer.is_printing();
    const auto params = client_printer.params();

    bool want_full = false;
    bool force_full = false;
    bool force_baseline = false;
    if (telemetry_due(printing, params, now_ms, want_full, force_baseline, force_full)) {
        publish_telemetry(params, want_full, force_full, mqtt_client);
        last_telemetry_ms_ = now_ms;
        if (want_full) {
            last_full_telemetry_ms_ = now_ms;
            telemetry_changes_.mark_clean();
        }
    }
}

} // namespace connect2_client
