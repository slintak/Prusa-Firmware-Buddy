#include "config.hpp"

#include <config_store/store_instance.hpp>
#include <support_utils.h>

namespace connect2_client {

Config load_config() {
    Config cfg = {};
    cfg.enabled = config_store().connect_enabled.get();
    strlcpy(cfg.host, config_store().connect_host.get().data(), sizeof(cfg.host));
    decompress_host(cfg.host, sizeof(cfg.host));
    cfg.tls = config_store().connect_tls.get();
    cfg.port = config_store().connect_port.get();
    cfg.custom_cert = config_store().connect_custom_tls_cert.get();
    return cfg;
}

} // namespace connect2_client
