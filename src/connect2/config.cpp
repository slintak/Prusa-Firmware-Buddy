#include "config.hpp"

#include <config_store/store_instance.hpp>
#include <support_utils.h>

#include <ini.h>
#include <unique_file_ptr.hpp>

#include <logging/log.hpp>
#include <mbedtls/sha256.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>

LOG_COMPONENT_REF(connect2);

namespace connect2_client {

namespace {
constexpr const char *INI_SECTION = "service::connect";
constexpr const char *OAUTH_CFG_PATH = "/internal/connect/oauth.cfg";

struct IniConfig {
    char host[config_store_ns::connect_host_size + 1] = "";
    uint16_t port = 0;
    bool tls = true;
    bool custom_cert = false;
    bool loaded = false;
    char oauth_device_auth_url[128] = "";
    char oauth_token_url[128] = "";
    char oauth_url[96] = "";
    char oauth_device_auth_path[64] = "/oauth/device_authorization";
    char oauth_token_path[64] = "/oauth/token";
    bool oauth_loaded = false;
};

bool ini_string_match(const char *section, const char *section_var,
    const char *name, const char *name_var) {
    return strcmp(section_var, section) == 0 && strcmp(name_var, name) == 0;
}

bool compose_url(const char *base_url, const char *path, char *out, size_t out_size) {
    if (base_url == nullptr || path == nullptr || base_url[0] == '\0' || path[0] == '\0' || out_size == 0) {
        return false;
    }

    const size_t base_len = strlen(base_url);
    const bool base_has_slash = base_len > 0 && base_url[base_len - 1] == '/';
    const bool path_has_slash = path[0] == '/';
    const char *path_part = path;
    if (base_has_slash && path_has_slash) {
        path_part = path + 1;
    }
    const char *sep = (!base_has_slash && !path_has_slash) ? "/" : "";
    return snprintf(out, out_size, "%s%s%s", base_url, sep, path_part) > 0;
}

void fill_missing_oauth_urls(IniConfig &cfg) {
    if (cfg.oauth_device_auth_url[0] == '\0') {
        (void)compose_url(cfg.oauth_url, cfg.oauth_device_auth_path, cfg.oauth_device_auth_url, sizeof(cfg.oauth_device_auth_url));
    }
    if (cfg.oauth_token_url[0] == '\0') {
        (void)compose_url(cfg.oauth_url, cfg.oauth_token_path, cfg.oauth_token_url, sizeof(cfg.oauth_token_url));
    }
}

void fill_missing_oauth_urls(Config &cfg, const char *legacy_base_url, const char *legacy_auth_path, const char *legacy_token_path) {
    if (cfg.oauth_device_auth_url[0] == '\0') {
        (void)compose_url(legacy_base_url, legacy_auth_path, cfg.oauth_device_auth_url, sizeof(cfg.oauth_device_auth_url));
    }
    if (cfg.oauth_token_url[0] == '\0') {
        (void)compose_url(legacy_base_url, legacy_token_path, cfg.oauth_token_url, sizeof(cfg.oauth_token_url));
    }
}

int connect_ini_handler(void *user, const char *section, const char *name,
    const char *value) {
    if (user == nullptr || section == nullptr || name == nullptr || value == nullptr) {
        return 0;
    }

    auto *config = reinterpret_cast<IniConfig *>(user);
    if (ini_string_match(section, INI_SECTION, name, "hostname")) {
        char buffer[sizeof config->host];
        if (compress_host(value, buffer, sizeof buffer)) {
            strlcpy(config->host, buffer, sizeof config->host);
            config->loaded = true;
        } else {
            return 0;
        }
    } else if (ini_string_match(section, INI_SECTION, name, "port")) {
        char *endptr;
        long tmp = strtol(value, &endptr, 10);
        if (*endptr == '\0' && tmp >= 0 && tmp <= 65535) {
            config->port = static_cast<uint16_t>(tmp);
            config->loaded = true;
        } else {
            return 0;
        }
    } else if (ini_string_match(section, INI_SECTION, name, "tls")) {
        if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0) {
            config->tls = true;
            config->loaded = true;
        } else if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0) {
            config->tls = false;
            config->loaded = true;
        } else {
            return 0;
        }
    } else if (ini_string_match(section, INI_SECTION, name, "custom_cert")) {
        if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0) {
            config->custom_cert = true;
        } else if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0) {
            config->custom_cert = false;
        } else {
            return 0;
        }
    } else if (ini_string_match(section, INI_SECTION, name, "oauth_device_auth_url")) {
        strlcpy(config->oauth_device_auth_url, value, sizeof(config->oauth_device_auth_url));
        config->oauth_loaded = true;
    } else if (ini_string_match(section, INI_SECTION, name, "oauth_token_url")) {
        strlcpy(config->oauth_token_url, value, sizeof(config->oauth_token_url));
        config->oauth_loaded = true;
    } else if (ini_string_match(section, INI_SECTION, name, "oauth_url")) {
        strlcpy(config->oauth_url, value, sizeof(config->oauth_url));
        config->oauth_loaded = true;
    } else if (ini_string_match(section, INI_SECTION, name, "oauth_device_auth_path")) {
        strlcpy(config->oauth_device_auth_path, value, sizeof(config->oauth_device_auth_path));
        config->oauth_loaded = true;
    } else if (ini_string_match(section, INI_SECTION, name, "oauth_token_path")) {
        strlcpy(config->oauth_token_path, value, sizeof(config->oauth_token_path));
        config->oauth_loaded = true;
    }
    return 1;
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
        size_t read = fread(buffer, 1, block, s.get());
        if (read > 0 && fwrite(buffer, 1, read, d.get()) != read) {
            return false;
        }
    }

    return !ferror(s.get()) && !ferror(d.get());
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

bool save_oauth_cfg(const IniConfig &cfg) {
    mkdir("/internal/connect", 0777);
    unique_file_ptr f(fopen(OAUTH_CFG_PATH, "wb"));
    if (!f) {
        log_info(connect2, "oauth cfg open for write failed path=%s errno=%d", OAUTH_CFG_PATH, errno);
        return false;
    }
    const int written = fprintf(f.get(),
        "oauth_device_auth_url=%s\noauth_token_url=%s\n",
        cfg.oauth_device_auth_url, cfg.oauth_token_url);
    if (written <= 0 || ferror(f.get())) {
        log_info(connect2, "oauth cfg write failed path=%s errno=%d", OAUTH_CFG_PATH, errno);
        return false;
    }
    return true;
}

void load_oauth_cfg(Config &cfg) {
    unique_file_ptr f(fopen(OAUTH_CFG_PATH, "rb"));
    if (!f) {
        return;
    }
    char legacy_base_url[96] = {};
    char legacy_auth_path[64] = {};
    char legacy_token_path[64] = {};
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
        if (strcmp(line, "oauth_device_auth_url") == 0) {
            strlcpy(cfg.oauth_device_auth_url, value, sizeof(cfg.oauth_device_auth_url));
        } else if (strcmp(line, "oauth_token_url") == 0) {
            strlcpy(cfg.oauth_token_url, value, sizeof(cfg.oauth_token_url));
        } else if (strcmp(line, "oauth_url") == 0) {
            strlcpy(legacy_base_url, value, sizeof(legacy_base_url));
        } else if (strcmp(line, "oauth_device_auth_path") == 0) {
            strlcpy(legacy_auth_path, value, sizeof(legacy_auth_path));
        } else if (strcmp(line, "oauth_token_path") == 0) {
            strlcpy(legacy_token_path, value, sizeof(legacy_token_path));
        }
    }
    fill_missing_oauth_urls(cfg, legacy_base_url, legacy_auth_path, legacy_token_path);
}
} // namespace

Config load_config() {
    Config cfg = {};
    cfg.enabled = config_store().connect_enabled.get();
    strlcpy(cfg.host, config_store().connect_host.get().data(), sizeof(cfg.host));
    decompress_host(cfg.host, sizeof(cfg.host));
    cfg.tls = config_store().connect_tls.get();
    cfg.port = config_store().connect_port.get();
    cfg.custom_cert = config_store().connect_custom_tls_cert.get();
    load_oauth_cfg(cfg);
    return cfg;
}

bool load_cfg_from_ini() {
    IniConfig config;
    bool ok = ini_parse("/usb/prusa_printer_settings.ini", connect_ini_handler, &config) == 0;
    ok = ok && (config.loaded || config.oauth_loaded);
    fill_missing_oauth_urls(config);

    if (ok && config.loaded && config.custom_cert) {
        mkdir("/internal/connect", 0777);
        if (!copy_file("/usb/connect.der", "/internal/connect/connect.der")) {
            log_info(connect2, "copy /usb/connect.der -> /internal/connect/connect.der failed (errno=%d)", errno);
            ok = false;
        }
        log_file_sha256("usb_cert", "/usb/connect.der");
        log_file_sha256("internal_cert", "/internal/connect/connect.der");
    }

    if (ok && config.oauth_loaded) {
        ok = save_oauth_cfg(config);
    }

    if (ok && config.loaded) {
        auto &store = config_store();
        auto transaction = store.get_backend().transaction_guard();
        store.connect_host.set(config.host);
        store.connect_port.set(config.port);
        store.connect_tls.set(config.tls);
        store.connect_custom_tls_cert.set(config.custom_cert);
        // Note: enabled is controlled in the GUI
    }
    return ok;
}

} // namespace connect2_client
