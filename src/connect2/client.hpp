#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <pb.h>

#include <common/mqtt/mqtt_client.hpp>
#include <common/oauth/device_flow.hpp>
#include <common/utils/exponential_backoff.hpp>
#include "config.hpp"
#include "oauth_storage.hpp"
#include "run.hpp"
#include "telemetry.hpp"

namespace connect2_client {

class Client {
public:
    explicit Client(buddy::mqtt::Client &mqtt_client);
    void run();
    void request_registration();
    void cancel_registration();
    OnlineStatus last_status() const;
    RegistrationInfo registration_info() const;
    bool has_stored_auth() const;

private:
    enum class State : uint8_t {
        Disabled,
        Disconnected,
        Connecting,
        Connected,
        Backoff,
        RegistrationRequired,
    };

    void step();
    void sleep_idle(uint32_t ms);
    void refresh_config(uint32_t now_ms);
    void enter_backoff(uint32_t now_ms, uint32_t min_delay_ms = 0);
    bool run_oauth_device_flow();
    bool run_oauth_refresh();
    void on_oauth_device_code_ready(const buddy::oauth::DeviceCode &device_code);
    void load_auth_from_storage();
    void apply_auth_identity();
    bool has_valid_auth() const;
    bool should_refresh_token(uint32_t now_epoch_s) const;
    bool network_ready();
    bool ensure_subscribe_topics();
    void handle_publish(const char *topic, size_t topic_len, const uint8_t *payload, size_t payload_len);
    void handle_command_topic(const uint8_t *payload, size_t payload_len);
    void handle_gcode_topic(const uint8_t *payload, size_t payload_len);
    bool enqueue_event_bytes(const uint8_t *payload, size_t payload_len);
    void flush_pending_events();
    bool publish_event(const void *payload_struct, const pb_msgdesc_t *fields, size_t max_size);
    bool publish_finished_event(uint32_t command_id);
    bool publish_rejected_event(uint32_t command_id, const char *reason);
    bool publish_state_changed_event(uint32_t command_id);
    bool publish_info_event(uint32_t command_id);
    bool publish_job_info_event(uint32_t command_id, uint32_t start_cmd_id);
    static void publish_callback(void *ctx, const char *topic, size_t topic_len,
        const uint8_t *payload, size_t payload_len, uint8_t qos, bool retain, bool dup);
    static void oauth_device_code_ready_cb(const buddy::oauth::DeviceCode &device_code, void *ctx);
    static bool oauth_should_abort_cb(void *ctx);

    static uint32_t config_hash(const Config &cfg);
    static bool is_idle_state(State state);

    buddy::mqtt::Client &mqtt_client_;
    Telemetry telemetry_;
    Config cfg_ {};
    uint32_t last_cfg_hash_ = 0;
    State state_ = State::Disabled;
    uint32_t next_cfg_check_ms_ = 0;
    uint32_t next_action_ms_ = 0;
    OAuthStorageData auth_ {};
    bool last_net_ready_ = false;
    buddy::ExponentialBackoff<uint32_t, 100, 60000> backoff_;
    std::atomic<bool> registration_requested_ { false };
    std::atomic<bool> registration_cancel_requested_ { false };
    bool registration_in_progress_ = false;
    std::atomic<bool> registration_info_valid_ { false };
    char verification_uri_[192] = {};
    char user_code_[64] = {};
    char verification_url_with_code_[320] = {};
    std::atomic<ConnectionStatus> status_ { ConnectionStatus::Unknown };
    std::atomic<OnlineError> error_ { OnlineError::NoError };
    std::atomic<bool> has_stored_auth_ { false };
    bool subscribed_ = false;
    char command_topic_[128] = {};
    char gcode_topic_[128] = {};
    char transfer_topic_[128] = {};
    char debug_topic_[128] = {};
    char event_topic_[128] = {};

    static constexpr size_t pending_event_max_size_ = 768;
    static constexpr size_t pending_event_queue_size_ = 4;
    struct PendingEvent {
        bool used = false;
        size_t size = 0;
        uint8_t data[pending_event_max_size_] = {};
    };
    PendingEvent pending_events_[pending_event_queue_size_] = {};
    size_t pending_event_head_ = 0;
    size_t pending_event_tail_ = 0;
};

} // namespace connect2_client
