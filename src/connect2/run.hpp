#pragma once

#include <cstdint>

namespace connect2_client {

enum class ConnectionStatus : uint8_t {
    Unknown,
    Off,
    NoConfig,
    Authorizing,
    AuthRequired,
    Connecting,
    Online,
    Error,
};

enum class OnlineError : uint8_t {
    NoError,
    Dns,
    Connection,
    Tls,
    Auth,
    Server,
    Internal,
    Network,
    Protocol,
};

struct OnlineStatus {
    ConnectionStatus status;
    OnlineError error;
};

void run();
OnlineStatus last_status();
void request_registration();
bool has_stored_auth();

} // namespace connect2_client
