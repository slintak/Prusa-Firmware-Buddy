#include "mqtt_client.hpp"

namespace buddy::mqtt {

Client::Client() = default;

void Client::set_config(const Config &cfg) {
    cfg_ = cfg;
}

bool Client::connect(const char *host, uint16_t port, bool tls, bool custom_cert) {
    cfg_.host = host;
    cfg_.port = port;
    cfg_.tls = tls;
    cfg_.custom_cert = custom_cert;
    connected_ = true;
    return true;
}

void Client::disconnect() {
    connected_ = false;
}

void Client::step() {
    (void)cfg_;
}

bool Client::is_connected() const {
    return connected_;
}

} // namespace buddy::mqtt
