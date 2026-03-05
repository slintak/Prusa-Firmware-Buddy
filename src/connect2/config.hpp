#pragma once

#include "hostname.hpp"

#include <cstdint>

namespace connect2_client {

struct Config {
    char host[max_host_buf_len] = "";
    uint16_t port = 0;
    bool tls = true;
    bool enabled = false;
    bool custom_cert = false;
    char oauth_device_auth_url[128] = "";
    char oauth_token_url[128] = "";
};

Config load_config();
bool load_cfg_from_ini();

} // namespace connect2_client
