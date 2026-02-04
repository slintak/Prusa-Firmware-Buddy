#include "command.hpp"

extern "C" {
#include "pb_decode.h"
}

#include "command.pb.h"

#include <cstring>
#include <cstdio>

namespace connect2_client {

DecodedCommand decode_command(const uint8_t *payload, size_t payload_len) {
    Command msg = Command_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload, payload_len);
    if (!pb_decode(&stream, Command_fields, &msg)) {
        return {};
    }

    DecodedCommand decoded {};
    switch (msg.type) {
    case Command_Type_SEND_INFO:
        decoded.type = CommandType::SendInfo;
        break;
    case Command_Type_SEND_FILE_INFO:
        decoded.type = CommandType::SendFileInfo;
        break;
    case Command_Type_SEND_JOB_INFO:
        decoded.type = CommandType::SendJobInfo;
        break;
    case Command_Type_START_PRINT:
        decoded.type = CommandType::StartPrint;
        break;
    case Command_Type_STOP_PRINT:
        decoded.type = CommandType::StopPrint;
        break;
    case Command_Type_PAUSE_PRINT:
        decoded.type = CommandType::PausePrint;
        break;
    case Command_Type_RESUME_PRINT:
        decoded.type = CommandType::ResumePrint;
        break;
    case Command_Type_RESET_PRINTER:
    case Command_Type_RESET:
        decoded.type = CommandType::ResetPrinter;
        break;
    default:
        decoded.type = CommandType::Unknown;
        break;
    }
    decoded.command_id = msg.command_id;
    decoded.job_id = msg.job_id;
    if (msg.path[0] != '\0') {
        std::snprintf(decoded.path, sizeof(decoded.path), "%s", msg.path);
    }
    return decoded;
}

} // namespace connect2_client
