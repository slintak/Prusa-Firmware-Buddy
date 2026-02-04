#include "telemetry.hpp"

#include <connect/marlin_printer.hpp>
#include <connect/printer_common.hpp>
#include <marlin_server_shared.h>
#include <otp.hpp>
#include <state/printer_state.hpp>

#include <common/mqtt/mqtt_client.hpp>
#include "info_event.hpp"
#include <transfers/changed_path.hpp>
#include <gui/file_list_defs.h>

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include "mqtt.h"
}

namespace connect2_client {

connect_client::MarlinPrinter &shared_printer() {
    static connect_client::MarlinPrinter instance;
    return instance;
}

namespace {
constexpr uint32_t TELEMETRY_INTERVAL_MIN = 750;
constexpr uint32_t TELEMETRY_INTERVAL_LONG = 1000 * 4;
constexpr uint32_t TELEMETRY_INTERVAL_SHORT = 1000;
constexpr uint32_t TELEMETRY_INTERVAL_FULL = 1000 * 60 * 5;

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

const char *printer_id() {
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

bool make_printer_topic(char *buffer, size_t buffer_size, const char *suffix) {
    const int written = snprintf(buffer, buffer_size, "v1/devices/printers/%s/data%s", printer_id(), suffix);
    return written > 0 && static_cast<size_t>(written) < buffer_size;
}

bool make_job_topic(char *buffer, size_t buffer_size, uint16_t job_id, const char *suffix) {
    const int written = snprintf(buffer, buffer_size, "v1/devices/printers/%s/jobs/%u/data%s", printer_id(),
        static_cast<unsigned int>(job_id), suffix);
    return written > 0 && static_cast<size_t>(written) < buffer_size;
}

bool make_dialog_topic(char *buffer, size_t buffer_size) {
    const int written = snprintf(buffer, buffer_size, "v1/devices/printers/%s/dialog", printer_id());
    return written > 0 && static_cast<size_t>(written) < buffer_size;
}

bool make_command_topic(char *buffer, size_t buffer_size) {
    const int written = snprintf(buffer, buffer_size, "v1/devices/printers/%s/cmd", printer_id());
    return written > 0 && static_cast<size_t>(written) < buffer_size;
}

bool make_event_topic(char *buffer, size_t buffer_size) {
    const int written = snprintf(buffer, buffer_size, "v1/devices/printers/%s/event", printer_id());
    return written > 0 && static_cast<size_t>(written) < buffer_size;
}

bool path_allowed(const char *path) {
    constexpr const char *const usb = "/usb/";
    const bool is_on_usb = strncmp(path, usb, strlen(usb)) == 0 || strcmp(path, "/usb") == 0;
    const bool contains_upper = strstr(path, "/../") != nullptr;
    return is_on_usb && !contains_upper;
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

bool publish_topic_raw(buddy::mqtt::Client &mqtt_client, const char *topic, const uint8_t *payload, size_t payload_len, uint8_t qos, bool retain) {
    return mqtt_client.publish_raw(topic, payload, payload_len, publish_flags(qos, retain));
}

void publish_printer_value(buddy::mqtt::Client &mqtt_client, const char *suffix, const char *payload, uint8_t qos, bool retain) {
    char topic[128];
    if (!make_printer_topic(topic, sizeof(topic), suffix)) {
        return;
    }
    (void)publish_topic(mqtt_client, topic, payload, qos, retain);
}

void publish_job_value(buddy::mqtt::Client &mqtt_client, uint16_t job_id, const char *suffix, const char *payload, uint8_t qos, bool retain) {
    char topic[128];
    if (!make_job_topic(topic, sizeof(topic), job_id, suffix)) {
        return;
    }
    (void)publish_topic(mqtt_client, topic, payload, qos, retain);
}

void publish_printer_value_float(buddy::mqtt::Client &mqtt_client, const char *suffix, float value, uint8_t precision, uint8_t qos, bool retain) {
    char payload[32];
    const char *fmt = (precision == 2) ? "%.2f" : "%.1f";
    snprintf(payload, sizeof(payload), fmt, static_cast<double>(value));
    publish_printer_value(mqtt_client, suffix, payload, qos, retain);
}

void publish_printer_value_int(buddy::mqtt::Client &mqtt_client, const char *suffix, int value, uint8_t qos, bool retain) {
    char payload[32];
    snprintf(payload, sizeof(payload), "%d", value);
    publish_printer_value(mqtt_client, suffix, payload, qos, retain);
}

void publish_printer_value_u32(buddy::mqtt::Client &mqtt_client, const char *suffix, uint32_t value, uint8_t qos, bool retain) {
    char payload[32];
    snprintf(payload, sizeof(payload), "%lu", static_cast<unsigned long>(value));
    publish_printer_value(mqtt_client, suffix, payload, qos, retain);
}

void publish_job_value_u32(buddy::mqtt::Client &mqtt_client, uint16_t job_id, const char *suffix, uint32_t value, uint8_t qos, bool retain) {
    char payload[32];
    snprintf(payload, sizeof(payload), "%lu", static_cast<unsigned long>(value));
    publish_job_value(mqtt_client, job_id, suffix, payload, qos, retain);
}

} // namespace

Telemetry::Telemetry() {
    telemetry_changes_.mark_dirty();
    info_changes_.mark_dirty();
}

bool Telemetry::build_online_topic(char *buffer, size_t buffer_size) const {
    return make_printer_topic(buffer, buffer_size, "/online");
}

bool Telemetry::build_command_topic(char *buffer, size_t buffer_size) const {
    return make_command_topic(buffer, buffer_size);
}

void Telemetry::reset() {
    last_telemetry_ms_ = 0;
    last_full_telemetry_ms_ = 0;
    telemetry_changes_.mark_dirty();
    info_changes_.mark_dirty();
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
    if (!make_printer_topic(topic, sizeof(topic), "/online")) {
        return false;
    }
    const char *payload = online ? "1" : "0";
    return publish_topic(mqtt_client, topic, payload, 1, true);
}

void Telemetry::publish_telemetry(const connect_client::Printer::Params &params, bool full, bool force_baseline,
    bool force_full, buddy::mqtt::Client &mqtt_client) {
    const auto &head = params.slots[params.preferred_head()];
    const char *state = printer_state::to_str(params.state.device_state);

    if (force_baseline || !last_.valid || update_str(last_.state, sizeof(last_.state), state)) {
        publish_printer_value(mqtt_client, "/state", state, 1, true);
    }

    if (force_baseline || !last_.valid || float_changed(last_.axis_z, params.pos[connect_client::Printer::Z_AXIS_POS])) {
        publish_printer_value_float(mqtt_client, "/axis-z", params.pos[connect_client::Printer::Z_AXIS_POS], 2, 0, true);
        last_.axis_z = params.pos[connect_client::Printer::Z_AXIS_POS];
    }

    const uint32_t current_job = params.has_job ? params.job_id : 0;
    if (force_baseline || !last_.valid || last_.current_job != current_job) {
        publish_printer_value_u32(mqtt_client, "/current-job", current_job, 1, true);
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
            publish_printer_value_u32(mqtt_client, "/job-progress", params.progress_percent, 0, true);
            publish_job_value_u32(mqtt_client, params.job_id, "/progress", params.progress_percent, 0, true);
            last_.job_progress_valid = true;
            last_.job_progress = params.progress_percent;
        }

        if (!last_.job_state_valid || update_str(last_.job_state, sizeof(last_.job_state), state)) {
            publish_job_value(mqtt_client, params.job_id, "/state", state, 1, true);
            last_.job_state_valid = true;
        }

        if (params.time_to_end != marlin_server::TIME_TO_END_INVALID) {
            if (!last_.time_remaining_valid || last_.time_remaining != params.time_to_end) {
                publish_job_value_u32(mqtt_client, params.job_id, "/time-remaining", params.time_to_end, 0, true);
                last_.time_remaining_valid = true;
                last_.time_remaining = params.time_to_end;
            }
        }
    }

    if (full) {
        if (!params.has_job) {
            if (force_full || !last_.valid || float_changed(last_.axis_x, params.pos[connect_client::Printer::X_AXIS_POS])) {
                publish_printer_value_float(mqtt_client, "/axis-x", params.pos[connect_client::Printer::X_AXIS_POS], 2, 0, false);
                last_.axis_x = params.pos[connect_client::Printer::X_AXIS_POS];
            }
            if (force_full || !last_.valid || float_changed(last_.axis_y, params.pos[connect_client::Printer::Y_AXIS_POS])) {
                publish_printer_value_float(mqtt_client, "/axis-y", params.pos[connect_client::Printer::Y_AXIS_POS], 2, 0, false);
                last_.axis_y = params.pos[connect_client::Printer::Y_AXIS_POS];
            }
        }

        if (force_full || !last_.valid || float_changed(last_.temp_nozzle, head.temp_nozzle)) {
            publish_printer_value_float(mqtt_client, "/temp/nozzle/current", head.temp_nozzle, 1, 0, true);
            last_.temp_nozzle = head.temp_nozzle;
        }
        if (force_full || !last_.valid || float_changed(last_.target_nozzle, params.target_nozzle)) {
            publish_printer_value_float(mqtt_client, "/temp/nozzle/target", params.target_nozzle, 1, 1, true);
            last_.target_nozzle = params.target_nozzle;
        }
        if (force_full || !last_.valid || float_changed(last_.temp_bed, params.temp_bed)) {
            publish_printer_value_float(mqtt_client, "/temp/heatbed/current", params.temp_bed, 1, 0, true);
            last_.temp_bed = params.temp_bed;
        }
        if (force_full || !last_.valid || float_changed(last_.target_bed, params.target_bed)) {
            publish_printer_value_float(mqtt_client, "/temp/heatbed/target", params.target_bed, 1, 1, true);
            last_.target_bed = params.target_bed;
        }
        if (force_full || !last_.valid || last_.speed != params.print_speed) {
            publish_printer_value_int(mqtt_client, "/speed", params.print_speed, 1, true);
            last_.speed = params.print_speed;
        }

        if (params.slots[params.preferred_slot()].material.data()[0] != '\0') {
            if (!last_.material_valid || update_str(last_.material, sizeof(last_.material),
                    params.slots[params.preferred_slot()].material.data())) {
                publish_printer_value(mqtt_client, "/material", params.slots[params.preferred_slot()].material.data(), 1, true);
                last_.material_valid = true;
            }
        }
    }

    if (params.state.dialog.has_value()) {
        char topic[128];
        if (make_dialog_topic(topic, sizeof(topic))) {
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

void Telemetry::publish_info_event(const connect_client::Printer &printer, const connect_client::Printer::Params &params,
    buddy::mqtt::Client &mqtt_client, uint32_t command_id) {
    char topic[128];
    if (!make_event_topic(topic, sizeof(topic))) {
        return;
    }

    static uint8_t payload[2048];
    size_t payload_len = 0;
    if (!encode_info_event(payload, sizeof(payload), printer, params, payload_len, command_id)) {
        return;
    }

    (void)publish_topic_raw(mqtt_client, topic, payload, payload_len, 1, false);
}

void Telemetry::publish_info_now(buddy::mqtt::Client &mqtt_client, uint32_t command_id) {
    const auto &client_printer = shared_printer();
    const auto params = client_printer.params();
    publish_info_event(client_printer, params, mqtt_client, command_id);
    info_changes_.set_hash(client_printer.info_fingerprint());
    info_changes_.mark_clean();
}

void Telemetry::publish_job_info_event(const connect_client::Printer &printer, const connect_client::Printer::Params &params,
    buddy::mqtt::Client &mqtt_client, uint32_t start_cmd_id, uint32_t command_id, std::optional<uint32_t> job_id) {
    char topic[128];
    if (!make_event_topic(topic, sizeof(topic))) {
        return;
    }

    static uint8_t payload[2048];
    size_t payload_len = 0;
    if (!encode_job_info_event(payload, sizeof(payload), printer, params, start_cmd_id, payload_len, command_id, job_id)) {
        const char *reason = (job_id.has_value() && params.has_job) ? "Job ID doesn't match" : "No job in progress";
        publish_rejected_event(printer, params, mqtt_client, reason, command_id);
        return;
    }

    (void)publish_topic_raw(mqtt_client, topic, payload, payload_len, 1, false);
}

void Telemetry::publish_job_info_now(buddy::mqtt::Client &mqtt_client, uint32_t start_cmd_id, uint32_t command_id,
    std::optional<uint32_t> job_id) {
    const auto &client_printer = shared_printer();
    const auto params = client_printer.params();
    publish_job_info_event(client_printer, params, mqtt_client, start_cmd_id, command_id, job_id);
}

void Telemetry::publish_finished_event(const connect_client::Printer &printer, const connect_client::Printer::Params &params,
    buddy::mqtt::Client &mqtt_client, uint32_t command_id) {
    char topic[128];
    if (!make_event_topic(topic, sizeof(topic))) {
        return;
    }

    static uint8_t payload[512];
    size_t payload_len = 0;
    if (!encode_finished_event(payload, sizeof(payload), printer, params, payload_len, command_id)) {
        return;
    }

    (void)publish_topic_raw(mqtt_client, topic, payload, payload_len, 1, false);
}

void Telemetry::publish_finished_now(buddy::mqtt::Client &mqtt_client, uint32_t command_id) {
    const auto &client_printer = shared_printer();
    const auto params = client_printer.params();
    publish_finished_event(client_printer, params, mqtt_client, command_id);
}

void Telemetry::publish_failed_event(const connect_client::Printer &printer, const connect_client::Printer::Params &params,
    buddy::mqtt::Client &mqtt_client, uint32_t command_id) {
    char topic[128];
    if (!make_event_topic(topic, sizeof(topic))) {
        return;
    }

    static uint8_t payload[512];
    size_t payload_len = 0;
    if (!encode_failed_event(payload, sizeof(payload), printer, params, payload_len, command_id)) {
        return;
    }

    (void)publish_topic_raw(mqtt_client, topic, payload, payload_len, 1, false);
}

void Telemetry::publish_failed_now(buddy::mqtt::Client &mqtt_client, uint32_t command_id) {
    const auto &client_printer = shared_printer();
    const auto params = client_printer.params();
    publish_failed_event(client_printer, params, mqtt_client, command_id);
}

void Telemetry::publish_state_changed_event(const connect_client::Printer &printer, const connect_client::Printer::Params &params,
    buddy::mqtt::Client &mqtt_client, uint32_t command_id) {
    char topic[128];
    if (!make_event_topic(topic, sizeof(topic))) {
        return;
    }

    static uint8_t payload[512];
    size_t payload_len = 0;
    if (!encode_state_changed_event(payload, sizeof(payload), printer, params, payload_len, command_id)) {
        return;
    }

    (void)publish_topic_raw(mqtt_client, topic, payload, payload_len, 1, false);
}

void Telemetry::publish_file_info_event(const connect_client::Printer &printer, const connect_client::Printer::Params &params,
    buddy::mqtt::Client &mqtt_client, const char *path, uint32_t command_id) {
    char topic[128];
    if (!make_event_topic(topic, sizeof(topic))) {
        return;
    }

    static uint8_t payload[2048];
    size_t payload_len = 0;
    if (!encode_file_info_event(payload, sizeof(payload), printer, params, path, payload_len, command_id)) {
        publish_rejected_event(printer, params, mqtt_client, "File not found", command_id);
        return;
    }

    (void)publish_topic_raw(mqtt_client, topic, payload, payload_len, 1, false);
}

void Telemetry::publish_file_info_now(buddy::mqtt::Client &mqtt_client, const char *path, uint32_t command_id) {
    const auto &client_printer = shared_printer();
    const auto params = client_printer.params();
    if (!path_allowed(path)) {
        publish_rejected_event(client_printer, params, mqtt_client, "Forbidden path", command_id);
        return;
    }
    publish_file_info_event(client_printer, params, mqtt_client, path, command_id);
}

void Telemetry::publish_file_changed_event(const connect_client::Printer &printer, const connect_client::Printer::Params &params,
    buddy::mqtt::Client &mqtt_client, const char *path, bool is_file, int incident, uint32_t command_id) {
    char topic[128];
    if (!make_event_topic(topic, sizeof(topic))) {
        return;
    }

    static uint8_t payload[2048];
    size_t payload_len = 0;
    if (!encode_file_changed_event(payload, sizeof(payload), printer, params, path, is_file, incident, payload_len, command_id)) {
        return;
    }

    (void)publish_topic_raw(mqtt_client, topic, payload, payload_len, 1, false);
}

void Telemetry::publish_rejected_event(const connect_client::Printer &printer, const connect_client::Printer::Params &params,
    buddy::mqtt::Client &mqtt_client, const char *reason, uint32_t command_id) {
    char topic[128];
    if (!make_event_topic(topic, sizeof(topic))) {
        return;
    }

    static uint8_t payload[1024];
    size_t payload_len = 0;
    if (!encode_rejected_event(payload, sizeof(payload), printer, params, reason, payload_len, command_id)) {
        return;
    }

    (void)publish_topic_raw(mqtt_client, topic, payload, payload_len, 1, false);
}

void Telemetry::publish_rejected_now(buddy::mqtt::Client &mqtt_client, const connect_client::Printer &printer,
    const connect_client::Printer::Params &params, const char *reason, uint32_t command_id) {
    publish_rejected_event(printer, params, mqtt_client, reason, command_id);
}

void Telemetry::process_changed_paths(const connect_client::Printer &printer, const connect_client::Printer::Params &params,
    buddy::mqtt::Client &mqtt_client) {
    transfers::ChangedPath::instance.media_inserted(params.has_usb);
    char path_buf[FILE_PATH_BUFFER_LEN + FILE_NAME_MAX_LEN] = {};
    bool is_file = false;
    int incident = 0;
    uint32_t cmd_id = 0;
    {
        auto status = transfers::ChangedPath::instance.status();
        if (!status.has_value()) {
            return;
        }
        if (!status->consume(path_buf, sizeof(path_buf))) {
            return;
        }
        is_file = status->is_file();
        incident = static_cast<int>(status->what_happend());
        cmd_id = status->triggered_command_id().value_or(0);
    }

    if (is_file && static_cast<transfers::ChangedPath::Incident>(incident) == transfers::ChangedPath::Incident::Created) {
        publish_file_info_event(printer, params, mqtt_client, path_buf, cmd_id);
    } else {
        publish_file_changed_event(printer, params, mqtt_client, path_buf, is_file, incident, cmd_id);
    }
}

void Telemetry::tick(uint32_t now_ms, buddy::mqtt::Client &mqtt_client) {
    if (!mqtt_client.is_connected()) {
        return;
    }

    if (!online_published_) {
        (void)publish_online(true, mqtt_client);
        online_published_ = true;
    }

    const auto &client_printer = shared_printer();
    const bool printing = client_printer.is_printing();
    const auto params = client_printer.params();

    bool want_full = false;
    bool force_full = false;
    bool force_baseline = false;
    if (telemetry_due(printing, params, now_ms, want_full, force_baseline, force_full)) {
        publish_telemetry(params, want_full, force_baseline, force_full, mqtt_client);
        last_telemetry_ms_ = now_ms;
        if (want_full) {
            last_full_telemetry_ms_ = now_ms;
            telemetry_changes_.mark_clean();
        }
    }

    if (info_changes_.set_hash(client_printer.info_fingerprint())) {
        publish_info_event(client_printer, params, mqtt_client, 0);
        info_changes_.mark_clean();
    }

    if (state_changes_.set_hash(params.state_fingerprint())) {
        publish_state_changed_event(client_printer, params, mqtt_client, 0);
    }

    process_changed_paths(client_printer, params, mqtt_client);
}

} // namespace connect2_client
