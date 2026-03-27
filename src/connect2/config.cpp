#include "config.hpp"

#include <config_store/store_instance.hpp>
#include <support_utils.h>

#include <ini.h>
#include <unique_file_ptr.hpp>

#include <logging/log.hpp>
#include <mbedtls/sha256.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>

LOG_COMPONENT_REF(connect2);

namespace connect2_client {

namespace {
constexpr const char *INI_SECTION = "service::connect2";
constexpr const char *CONNECT2_CFG_PATH = "/internal/connect2/config.cfg";

struct IniConfig {
    bool mqtt_loaded = false;
    bool oauth_loaded = false;

    char mqtt_host[max_host_buf_len] = "";
    uint16_t mqtt_port = 0;
    bool mqtt_tls = true;
    bool mqtt_custom_cert = false;

    char oauth_host[max_host_buf_len] = "";
    uint16_t oauth_port = 0;
    bool oauth_tls = true;
    bool oauth_custom_cert = false;

    char oauth_device_auth_url[128] = "";
    char oauth_token_url[128] = "";
    char oauth_device_auth_path[64] = "/oauth/device_authorization";
    char oauth_token_path[64] = "/oauth/token";
};

bool ini_string_match(const char *section, const char *section_var,
    const char *name, const char *name_var) {
    return strcmp(section_var, section) == 0 && strcmp(name_var, name) == 0;
}

bool parse_bool_value(const char *value, bool &out) {
    if (value == nullptr) {
        return false;
    }
    if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0) {
        out = true;
        return true;
    }
    if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

bool parse_u16_value(const char *value, uint16_t &out) {
    if (value == nullptr) {
        return false;
    }
    char *endptr = nullptr;
    const long tmp = strtol(value, &endptr, 10);
    if (endptr == nullptr || *endptr != '\0' || tmp < 0 || tmp > 65535) {
        return false;
    }
    out = static_cast<uint16_t>(tmp);
    return true;
}

bool has_url_scheme(const char *value) {
    return value != nullptr
        && (strncasecmp(value, "https://", 8) == 0 || strncasecmp(value, "http://", 7) == 0);
}

bool parse_host_port(const char *value, uint16_t default_port, char *host_out, size_t host_out_size, uint16_t &port_out) {
    if (value == nullptr || host_out == nullptr || host_out_size == 0) {
        return false;
    }

    const char *start = value;
    const char *scheme = strstr(value, "://");
    if (scheme != nullptr) {
        start = scheme + 3;
    }

    const char *end = start;
    while (*end != '\0' && *end != '/') {
        ++end;
    }

    if (end <= start) {
        return false;
    }

    const char *colon = nullptr;
    for (const char *p = start; p < end; ++p) {
        if (*p == ':') {
            colon = p;
        }
    }

    const char *host_end = end;
    uint16_t parsed_port = default_port;
    if (colon != nullptr) {
        host_end = colon;
        char port_buf[8] = {};
        const size_t port_len = static_cast<size_t>(end - colon - 1);
        if (port_len == 0 || port_len >= sizeof(port_buf)) {
            return false;
        }
        memcpy(port_buf, colon + 1, port_len);
        uint16_t tmp = 0;
        if (!parse_u16_value(port_buf, tmp) || tmp == 0) {
            return false;
        }
        parsed_port = tmp;
    }

    const size_t host_len = static_cast<size_t>(host_end - start);
    if (host_len == 0 || host_len >= host_out_size) {
        return false;
    }

    memcpy(host_out, start, host_len);
    host_out[host_len] = '\0';
    port_out = parsed_port;
    return true;
}

bool compose_url_from_host_path(const char *host, uint16_t port, bool tls, const char *path, char *out, size_t out_size) {
    if (host == nullptr || host[0] == '\0' || path == nullptr || path[0] == '\0' || out_size == 0) {
        return false;
    }

    const char *path_part = path;
    char path_buf[96] = {};
    if (path[0] != '/') {
        const int n = snprintf(path_buf, sizeof(path_buf), "/%s", path);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(path_buf)) {
            return false;
        }
        path_part = path_buf;
    }

    const int n = snprintf(out, out_size, "%s://%s:%u%s", tls ? "https" : "http", host, static_cast<unsigned>(port), path_part);
    return n > 0 && static_cast<size_t>(n) < out_size;
}

void fill_missing_oauth_urls(IniConfig &cfg) {
    if (cfg.oauth_host[0] == '\0' && cfg.mqtt_host[0] != '\0') {
        strlcpy(cfg.oauth_host, cfg.mqtt_host, sizeof(cfg.oauth_host));
    }
    if (cfg.oauth_port == 0) {
        cfg.oauth_port = cfg.mqtt_port != 0 ? cfg.mqtt_port : 443;
    }

    if (cfg.oauth_device_auth_url[0] == '\0') {
        (void)compose_url_from_host_path(cfg.oauth_host, cfg.oauth_port, cfg.oauth_tls,
            cfg.oauth_device_auth_path, cfg.oauth_device_auth_url, sizeof(cfg.oauth_device_auth_url));
    }
    if (cfg.oauth_token_url[0] == '\0') {
        (void)compose_url_from_host_path(cfg.oauth_host, cfg.oauth_port, cfg.oauth_tls,
            cfg.oauth_token_path, cfg.oauth_token_url, sizeof(cfg.oauth_token_url));
    }
}

void bytes_to_hex(const uint8_t *data, size_t size, char *out, size_t out_len) {
    static constexpr char kHex[] = "0123456789abcdef";
    if (out_len == 0) {
        return;
    }
    size_t i = 0;
    for (; i < size && (i * 2 + 1) < out_len; ++i) {
        out[i * 2] = kHex[(data[i] >> 4) & 0x0f];
        out[i * 2 + 1] = kHex[data[i] & 0x0f];
    }
    if (i * 2 < out_len) {
        out[i * 2] = '\0';
    } else {
        out[out_len - 1] = '\0';
    }
}

void log_file_sha256(const char *label, const char *path) {
    unique_file_ptr f(fopen(path, "rb"));
    if (!f) {
        log_info(connect2, "%s %s open failed (errno=%d)", label, path, errno);
        return;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);

    std::array<uint8_t, 128> buffer {};
    size_t total = 0;
    while (!feof(f.get()) && !ferror(f.get())) {
        const size_t read = fread(buffer.data(), 1, buffer.size(), f.get());
        if (read > 0) {
            mbedtls_sha256_update_ret(&sha, buffer.data(), read);
            total += read;
        }
    }

    uint8_t digest[32] = {};
    mbedtls_sha256_finish_ret(&sha, digest);
    mbedtls_sha256_free(&sha);

    char hex[65] = {};
    bytes_to_hex(digest, sizeof(digest), hex, sizeof(hex));
    log_info(connect2, "%s %s sha256=%s size=%u", label, path, hex, static_cast<unsigned>(total));
}

bool copy_file(const char *src, const char *dst) {
    unique_file_ptr s(fopen(src, "rb"));
    if (!s) {
        return false;
    }
    unique_file_ptr d(fopen(dst, "wb"));
    if (!d) {
        return false;
    }

    while (!feof(s.get()) && !ferror(s.get()) && !ferror(d.get())) {
        constexpr size_t block = 128;
        uint8_t buffer[block];
        const size_t read = fread(buffer, 1, block, s.get());
        if (read > 0 && fwrite(buffer, 1, read, d.get()) != read) {
            return false;
        }
    }

    return !ferror(s.get()) && !ferror(d.get());
}

bool save_connect2_cfg(const Config &cfg) {
    mkdir("/internal/connect2", 0777);
    unique_file_ptr f(fopen(CONNECT2_CFG_PATH, "wb"));
    if (!f) {
        log_info(connect2, "connect2 cfg open for write failed path=%s errno=%d", CONNECT2_CFG_PATH, errno);
        return false;
    }

    const int written = fprintf(f.get(),
        "mqtt_host=%s\n"
        "mqtt_port=%u\n"
        "mqtt_tls=%d\n"
        "mqtt_custom_cert=%d\n"
        "oauth_tls=%d\n"
        "oauth_custom_cert=%d\n"
        "oauth_device_auth_url=%s\n"
        "oauth_token_url=%s\n",
        cfg.host,
        static_cast<unsigned>(cfg.port),
        cfg.tls ? 1 : 0,
        cfg.custom_cert ? 1 : 0,
        cfg.oauth_tls ? 1 : 0,
        cfg.oauth_custom_cert ? 1 : 0,
        cfg.oauth_device_auth_url,
        cfg.oauth_token_url);

    if (written <= 0 || ferror(f.get())) {
        log_info(connect2, "connect2 cfg write failed path=%s errno=%d", CONNECT2_CFG_PATH, errno);
        return false;
    }

    return true;
}

bool load_connect2_cfg(Config &cfg) {
    unique_file_ptr f(fopen(CONNECT2_CFG_PATH, "rb"));
    if (!f) {
        return false;
    }

    bool loaded = false;
    char line[256] = {};
    while (fgets(line, sizeof(line), f.get()) != nullptr) {
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *value = eq + 1;
        char *nl = strchr(value, '\n');
        if (nl) {
            *nl = '\0';
        }

        if (strcmp(line, "mqtt_host") == 0) {
            strlcpy(cfg.host, value, sizeof(cfg.host));
            loaded = true;
        } else if (strcmp(line, "mqtt_port") == 0) {
            uint16_t port = 0;
            if (parse_u16_value(value, port)) {
                cfg.port = port;
                loaded = true;
            }
        } else if (strcmp(line, "mqtt_tls") == 0) {
            bool v = true;
            if (parse_bool_value(value, v)) {
                cfg.tls = v;
                loaded = true;
            }
        } else if (strcmp(line, "mqtt_custom_cert") == 0) {
            bool v = false;
            if (parse_bool_value(value, v)) {
                cfg.custom_cert = v;
                loaded = true;
            }
        } else if (strcmp(line, "oauth_tls") == 0) {
            bool v = true;
            if (parse_bool_value(value, v)) {
                cfg.oauth_tls = v;
                loaded = true;
            }
        } else if (strcmp(line, "oauth_custom_cert") == 0) {
            bool v = false;
            if (parse_bool_value(value, v)) {
                cfg.oauth_custom_cert = v;
                loaded = true;
            }
        } else if (strcmp(line, "oauth_device_auth_url") == 0) {
            strlcpy(cfg.oauth_device_auth_url, value, sizeof(cfg.oauth_device_auth_url));
            loaded = true;
        } else if (strcmp(line, "oauth_token_url") == 0) {
            strlcpy(cfg.oauth_token_url, value, sizeof(cfg.oauth_token_url));
            loaded = true;
        }
    }

    return loaded;
}

int connect_ini_handler(void *user, const char *section, const char *name, const char *value) {
    if (user == nullptr || section == nullptr || name == nullptr || value == nullptr) {
        return 0;
    }

    auto *config = reinterpret_cast<IniConfig *>(user);

    if (ini_string_match(section, INI_SECTION, name, "mqtt_host")) {
        if (!parse_host_port(value, 8883, config->mqtt_host, sizeof(config->mqtt_host), config->mqtt_port)) {
            return 0;
        }
        config->mqtt_loaded = true;
        return 1;
    }
    if (ini_string_match(section, INI_SECTION, name, "mqtt_tls")) {
        if (!parse_bool_value(value, config->mqtt_tls)) {
            return 0;
        }
        config->mqtt_loaded = true;
        return 1;
    }
    if (ini_string_match(section, INI_SECTION, name, "mqtt_custom_cert")) {
        if (!parse_bool_value(value, config->mqtt_custom_cert)) {
            return 0;
        }
        config->mqtt_loaded = true;
        return 1;
    }

    if (ini_string_match(section, INI_SECTION, name, "oauth_host")) {
        if (!parse_host_port(value, 443, config->oauth_host, sizeof(config->oauth_host), config->oauth_port)) {
            return 0;
        }
        config->oauth_loaded = true;
        return 1;
    }
    if (ini_string_match(section, INI_SECTION, name, "oauth_tls")) {
        if (!parse_bool_value(value, config->oauth_tls)) {
            return 0;
        }
        config->oauth_loaded = true;
        return 1;
    }
    if (ini_string_match(section, INI_SECTION, name, "oauth_custom_cert")) {
        if (!parse_bool_value(value, config->oauth_custom_cert)) {
            return 0;
        }
        config->oauth_loaded = true;
        return 1;
    }
    if (ini_string_match(section, INI_SECTION, name, "oauth_auth_url")) {
        if (has_url_scheme(value)) {
            strlcpy(config->oauth_device_auth_url, value, sizeof(config->oauth_device_auth_url));
        } else {
            strlcpy(config->oauth_device_auth_path, value, sizeof(config->oauth_device_auth_path));
        }
        config->oauth_loaded = true;
        return 1;
    }
    if (ini_string_match(section, INI_SECTION, name, "oauth_token_url")) {
        if (has_url_scheme(value)) {
            strlcpy(config->oauth_token_url, value, sizeof(config->oauth_token_url));
        } else {
            strlcpy(config->oauth_token_path, value, sizeof(config->oauth_token_path));
        }
        config->oauth_loaded = true;
        return 1;
    }

    return 1;
}

} // namespace

Config load_config() {
    Config cfg = {};
    cfg.enabled = config_store().connect_enabled.get();

    (void)load_connect2_cfg(cfg);
    return cfg;
}

bool load_cfg_from_ini() {
    IniConfig ini_cfg;
    bool ok = ini_parse("/usb/prusa_printer_settings.ini", connect_ini_handler, &ini_cfg) == 0;
    ok = ok && (ini_cfg.mqtt_loaded || ini_cfg.oauth_loaded);
    if (!ok) {
        return false;
    }

    fill_missing_oauth_urls(ini_cfg);

    Config cfg = load_config();
    if (ini_cfg.mqtt_loaded) {
        if (ini_cfg.mqtt_host[0] != '\0') {
            strlcpy(cfg.host, ini_cfg.mqtt_host, sizeof(cfg.host));
        }
        cfg.port = ini_cfg.mqtt_port;
        cfg.tls = ini_cfg.mqtt_tls;
        cfg.custom_cert = ini_cfg.mqtt_custom_cert;
    }

    if (ini_cfg.oauth_loaded) {
        cfg.oauth_tls = ini_cfg.oauth_tls;
        cfg.oauth_custom_cert = ini_cfg.oauth_custom_cert;
        strlcpy(cfg.oauth_device_auth_url, ini_cfg.oauth_device_auth_url, sizeof(cfg.oauth_device_auth_url));
        strlcpy(cfg.oauth_token_url, ini_cfg.oauth_token_url, sizeof(cfg.oauth_token_url));
    }

    if (cfg.custom_cert) {
        mkdir("/internal/connect", 0777);
        if (!copy_file("/usb/connect.der", "/internal/connect/connect.der")) {
            log_info(connect2, "copy /usb/connect.der -> /internal/connect/connect.der failed (errno=%d)", errno);
            return false;
        }
        log_file_sha256("usb_cert", "/usb/connect.der");
        log_file_sha256("internal_cert", "/internal/connect/connect.der");
    }

    return save_connect2_cfg(cfg);
}

} // namespace connect2_client
