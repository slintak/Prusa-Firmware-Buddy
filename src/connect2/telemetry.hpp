#pragma once

#include <cstddef>
#include <cstdint>

#include <connect/changes.hpp>
#include <connect/printer_common.hpp>
#include <connect/marlin_printer.hpp>

namespace buddy::mqtt {
class Client;
}

namespace connect2_client {

connect_client::MarlinPrinter &shared_printer();

class Telemetry {
public:
    Telemetry();
    void reset();
    void tick(uint32_t now_ms, buddy::mqtt::Client &mqtt_client);
    bool build_online_topic(char *buffer, size_t buffer_size) const;
    bool build_command_topic(char *buffer, size_t buffer_size) const;
    void publish_info_now(buddy::mqtt::Client &mqtt_client, uint32_t command_id);
    void publish_file_info_now(buddy::mqtt::Client &mqtt_client, const char *path, uint32_t command_id);
    void publish_rejected_now(buddy::mqtt::Client &mqtt_client, const connect_client::Printer &printer,
        const connect_client::Printer::Params &params, const char *reason, uint32_t command_id);

private:
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

    bool publish_online(bool online, buddy::mqtt::Client &mqtt_client);
    bool telemetry_due(bool printing, const connect_client::Printer::Params &params, uint32_t now_ms,
        bool &want_full, bool &force_baseline, bool &force_full);
    void publish_telemetry(const connect_client::Printer::Params &params, bool full,
        bool force_baseline, bool force_full, buddy::mqtt::Client &mqtt_client);
    void publish_info_event(const connect_client::Printer &printer, const connect_client::Printer::Params &params,
        buddy::mqtt::Client &mqtt_client, uint32_t command_id);
    void publish_file_info_event(const connect_client::Printer &printer, const connect_client::Printer::Params &params,
        buddy::mqtt::Client &mqtt_client, const char *path, uint32_t command_id);
    void publish_file_changed_event(const connect_client::Printer &printer, const connect_client::Printer::Params &params,
        buddy::mqtt::Client &mqtt_client, const char *path, bool is_file, int incident, uint32_t command_id);
    void publish_rejected_event(const connect_client::Printer &printer, const connect_client::Printer::Params &params,
        buddy::mqtt::Client &mqtt_client, const char *reason, uint32_t command_id);
    // transfer events removed for now
    void process_changed_paths(const connect_client::Printer &printer, const connect_client::Printer::Params &params,
        buddy::mqtt::Client &mqtt_client);

    bool online_published_ = false;
    uint32_t last_telemetry_ms_ = 0;
    uint32_t last_full_telemetry_ms_ = 0;
    connect_client::Tracked telemetry_changes_;
    connect_client::Tracked info_changes_;
    LastTelemetry last_;
};

} // namespace connect2_client
