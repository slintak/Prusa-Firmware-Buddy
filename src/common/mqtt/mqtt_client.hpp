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
    void step();

private:
    Config cfg_;
};

} // namespace buddy::mqtt
