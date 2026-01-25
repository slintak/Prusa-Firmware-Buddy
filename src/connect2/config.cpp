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

struct IniConfig {
    char host[config_store_ns::connect_host_size + 1] = "";
    uint16_t port = 0;
    bool tls = true;
    bool custom_cert = false;
    bool loaded = false;
};

bool ini_string_match(const char *section, const char *section_var,
    const char *name, const char *name_var) {
    return strcmp(section_var, section) == 0 && strcmp(name_var, name) == 0;
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
} // namespace

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

bool load_cfg_from_ini() {
    IniConfig config;
    bool ok = ini_parse("/usb/prusa_printer_settings.ini", connect_ini_handler, &config) == 0;
    ok = ok && config.loaded;

    if (ok && config.custom_cert) {
        mkdir("/internal/connect", 0777);
        if (!copy_file("/usb/connect.der", "/internal/connect/connect.der")) {
            log_info(connect2, "copy /usb/connect.der -> /internal/connect/connect.der failed (errno=%d)", errno);
            ok = false;
        }
        log_file_sha256("usb_cert", "/usb/connect.der");
        log_file_sha256("internal_cert", "/internal/connect/connect.der");
    }

    if (ok) {
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
