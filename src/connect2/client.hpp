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

    static bool config_equal(const Config &a, const Config &b);

    buddy::mqtt::Client mqtt_client_;
    Config cfg_ {};
    Config last_cfg_ {};
    State state_ = State::Disabled;
    uint32_t next_cfg_check_ms_ = 0;
    uint32_t next_action_ms_ = 0;
};

} // namespace connect2_client
