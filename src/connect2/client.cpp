#include "client.hpp"

#include <cmsis_os.h>

#include <logging/log.hpp>

LOG_COMPONENT_REF(connect2);

namespace connect2_client {

namespace {
constexpr uint32_t IDLE_DELAY_MS = 50;
} // namespace

Client::Client() = default;

void Client::run() {
    for (;;) {
        step();
        sleep_idle(IDLE_DELAY_MS);
    }
}

void Client::step() {
    mqtt_client_.step();
}

void Client::sleep_idle(uint32_t ms) {
    osDelay(ms);
}

} // namespace connect2_client
