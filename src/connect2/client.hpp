#pragma once

#include <cstdint>
#include <cstddef>
#include <gui/file_list_defs.h>

#include <common/mqtt/mqtt_client.hpp>
#include <common/utils/exponential_backoff.hpp>
#include "config.hpp"
#include "telemetry.hpp"

namespace connect2_client {

class Client {
public:
    explicit Client(buddy::mqtt::Client &mqtt_client);
    void run();

private:
    enum class State : uint8_t {
        Disabled,
        Disconnected,
        Connecting,
        Connected,
        Backoff,
    };

    void step();
    void sleep_idle(uint32_t ms);
    void refresh_config(uint32_t now_ms);
    void enter_backoff(uint32_t now_ms);
    bool network_ready();
    bool ensure_command_topic();
    void handle_publish(const char *topic, size_t topic_len, const uint8_t *payload, size_t payload_len);
    static void publish_callback(void *ctx, const char *topic, size_t topic_len,
        const uint8_t *payload, size_t payload_len, uint8_t qos, bool retain, bool dup);
    static uint32_t config_hash(const Config &cfg);
    static bool is_idle_state(State state);

    buddy::mqtt::Client &mqtt_client_;
    Telemetry telemetry_;
    Config cfg_ {};
    uint32_t last_cfg_hash_ = 0;
    State state_ = State::Disabled;
    uint32_t next_cfg_check_ms_ = 0;
    uint32_t next_action_ms_ = 0;
    bool last_net_ready_ = false;
    bool subscribed_ = false;
    char command_topic_[128] = {};
    size_t command_topic_len_ = 0;
    buddy::ExponentialBackoff<uint32_t, 100, 60000> backoff_;
};

} // namespace connect2_client
