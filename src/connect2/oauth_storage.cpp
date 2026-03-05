#include "oauth_storage.hpp"

#include <logging/log.hpp>
#include <support_utils.h>
#include <unique_file_ptr.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

LOG_COMPONENT_REF(connect2);

namespace connect2_client {

namespace {
constexpr const char *OAUTH_TOKENS_PATH = "/internal/connect/oauth_tokens.cfg";
}

bool load_oauth_storage(OAuthStorageData &out) {
    out = {};
    unique_file_ptr f(fopen(OAUTH_TOKENS_PATH, "rb"));
    if (!f) {
        return false;
    }

    char line[2304] = {};
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

        if (strcmp(line, "access_token") == 0) {
            strlcpy(out.access_token, value, sizeof(out.access_token));
        } else if (strcmp(line, "refresh_token") == 0) {
            strlcpy(out.refresh_token, value, sizeof(out.refresh_token));
        } else if (strcmp(line, "mqtt_username") == 0) {
            strlcpy(out.mqtt_username, value, sizeof(out.mqtt_username));
        } else if (strcmp(line, "obtained_at_epoch_s") == 0) {
            out.obtained_at_epoch_s = static_cast<uint32_t>(strtoul(value, nullptr, 10));
        } else if (strcmp(line, "expires_at_epoch_s") == 0) {
            out.expires_at_epoch_s = static_cast<uint32_t>(strtoul(value, nullptr, 10));
        } else if (strcmp(line, "expires_in_s") == 0) {
            out.expires_in_s = static_cast<uint32_t>(strtoul(value, nullptr, 10));
        }
    }

    const bool valid = out.access_token[0] != '\0' && out.refresh_token[0] != '\0' && out.mqtt_username[0] != '\0';
    if (valid) {
        log_info(connect2, "oauth storage loaded user=%s exp_at=%lu ttl=%lu",
            out.mqtt_username,
            static_cast<unsigned long>(out.expires_at_epoch_s),
            static_cast<unsigned long>(out.expires_in_s));
    } else {
        log_info(connect2, "oauth storage invalid or incomplete");
    }
    return valid;
}

bool save_oauth_storage(const OAuthStorageData &in) {
    mkdir("/internal/connect", 0777);
    unique_file_ptr f(fopen(OAUTH_TOKENS_PATH, "wb"));
    if (!f) {
        log_info(connect2, "oauth storage open failed path=%s errno=%d", OAUTH_TOKENS_PATH, errno);
        return false;
    }

    const int written = fprintf(f.get(),
        "access_token=%s\n"
        "refresh_token=%s\n"
        "mqtt_username=%s\n"
        "obtained_at_epoch_s=%lu\n"
        "expires_at_epoch_s=%lu\n"
        "expires_in_s=%lu\n",
        in.access_token,
        in.refresh_token,
        in.mqtt_username,
        static_cast<unsigned long>(in.obtained_at_epoch_s),
        static_cast<unsigned long>(in.expires_at_epoch_s),
        static_cast<unsigned long>(in.expires_in_s));
    if (written <= 0 || ferror(f.get())) {
        log_info(connect2, "oauth storage write failed path=%s errno=%d", OAUTH_TOKENS_PATH, errno);
        return false;
    }

    log_info(connect2, "oauth storage saved user=%s access_len=%u refresh_len=%u exp_at=%lu",
        in.mqtt_username,
        static_cast<unsigned>(strlen(in.access_token)),
        static_cast<unsigned>(strlen(in.refresh_token)),
        static_cast<unsigned long>(in.expires_at_epoch_s));
    return true;
}

bool clear_oauth_storage() {
    const int rc = unlink(OAUTH_TOKENS_PATH);
    if (rc == 0 || errno == ENOENT) {
        log_info(connect2, "oauth storage cleared");
        return true;
    }
    log_info(connect2, "oauth storage clear failed path=%s errno=%d", OAUTH_TOKENS_PATH, errno);
    return false;
}

} // namespace connect2_client
