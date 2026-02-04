#pragma once

#include <cstdint>

namespace buddy::mqtt {

class Client {
public:
    struct Config {
        const char *host = "";
        uint16_t port = 0;
        bool tls = false;
        bool custom_cert = false;
    };

    Client();
    void set_config(const Config &cfg);
    bool connect(const char *host, uint16_t port, bool tls, bool custom_cert);
    void disconnect();
    void step();
    bool is_connected() const;

private:
    Config cfg_;
    bool connected_ = false;
};

} // namespace buddy::mqtt
