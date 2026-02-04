#include "mqtt_client.hpp"

namespace buddy::mqtt {

Client::Client() = default;

void Client::set_config(const Config &cfg) {
    cfg_ = cfg;
}

void Client::step() {
    (void)cfg_;
}

} // namespace buddy::mqtt
