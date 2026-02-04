#pragma once

#include <cstddef>
#include <cstdint>
#include <gui/file_list_defs.h>

namespace connect2_client {

enum class CommandType : uint8_t {
    Unknown = 0,
    SendInfo = 1,
    SendFileInfo = 2,
    StartPrint = 3,
    StopPrint = 4,
    PausePrint = 5,
    ResumePrint = 6,
    SendJobInfo = 7,
    ResetPrinter = 8,
};

struct DecodedCommand {
    CommandType type = CommandType::Unknown;
    uint32_t command_id = 0;
    char path[FILE_PATH_BUFFER_LEN] = {};
    uint32_t job_id = 0;
};

DecodedCommand decode_command(const uint8_t *payload, size_t payload_len);

} // namespace connect2_client
