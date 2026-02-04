#pragma once

#include <cstddef>
#include <cstdint>

#include <connect/printer.hpp>

namespace connect2_client {

bool encode_info_event(uint8_t *buffer, size_t buffer_size,
    const connect_client::Printer &printer,
    const connect_client::Printer::Params &params,
    size_t &out_size);

} // namespace connect2_client
