#pragma once

#include <cstdint>

#include <common/mqtt/mqtt_client.hpp>

namespace connect2_client {

class Client {
public:
    Client();
    void run();

private:
    void step();
    void sleep_idle(uint32_t ms);

    buddy::mqtt::Client mqtt_client_;
};

} // namespace connect2_client
