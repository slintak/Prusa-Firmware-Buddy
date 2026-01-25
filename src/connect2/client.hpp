#pragma once

#include <cstdint>

#include <common/mqtt/mqtt_client.hpp>
#include "config.hpp"

namespace connect2_client {

class Client {
public:
    Client();
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

    static uint32_t config_hash(const Config &cfg);
    static bool is_idle_state(State state);

    buddy::mqtt::Client mqtt_client_;
    Config cfg_ {};
    uint32_t last_cfg_hash_ = 0;
    State state_ = State::Disabled;
    uint32_t next_cfg_check_ms_ = 0;
    uint32_t next_action_ms_ = 0;
};

} // namespace connect2_client
