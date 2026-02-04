#pragma once

#include <cstddef>
#include <cstdint>

#include <connect/printer.hpp>

namespace connect2_client {

bool encode_info_event(uint8_t *buffer, size_t buffer_size,
    const connect_client::Printer &printer,
    const connect_client::Printer::Params &params,
    size_t &out_size,
    uint32_t command_id);

bool encode_file_info_event(uint8_t *buffer, size_t buffer_size,
    const connect_client::Printer &printer,
    const connect_client::Printer::Params &params,
    const char *path,
    size_t &out_size,
    uint32_t command_id);

bool encode_file_changed_event(uint8_t *buffer, size_t buffer_size,
    const connect_client::Printer &printer,
    const connect_client::Printer::Params &params,
    const char *path,
    bool is_file,
    int incident,
    size_t &out_size,
    uint32_t command_id);

bool encode_rejected_event(uint8_t *buffer, size_t buffer_size,
    const connect_client::Printer &printer,
    const connect_client::Printer::Params &params,
    const char *reason,
    size_t &out_size,
    uint32_t command_id);


} // namespace connect2_client
