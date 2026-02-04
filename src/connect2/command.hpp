#pragma once

#include <cstddef>
#include <cstdint>
#include <gui/file_list_defs.h>

namespace connect2_client {

enum class CommandType : uint8_t {
    Unknown = 0,
    SendInfo = 1,
    SendFileInfo = 2,
};

struct DecodedCommand {
    CommandType type = CommandType::Unknown;
    uint32_t command_id = 0;
    char path[FILE_PATH_BUFFER_LEN] = {};
};

DecodedCommand decode_command(const uint8_t *payload, size_t payload_len);

} // namespace connect2_client
