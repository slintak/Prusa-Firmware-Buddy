#pragma once

#include <cstddef>
#include <cstdint>

namespace connect2_client {

struct OAuthStorageData {
    char access_token[1536] = {};
    char refresh_token[1536] = {};
    char mqtt_username[64] = {};
    uint32_t obtained_at_epoch_s = 0;
    uint32_t expires_at_epoch_s = 0;
    uint32_t expires_in_s = 0;
};

bool load_oauth_storage(OAuthStorageData &out);
bool save_oauth_storage(const OAuthStorageData &in);
bool clear_oauth_storage();

} // namespace connect2_client
