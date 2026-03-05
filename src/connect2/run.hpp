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

struct RegistrationInfo {
    bool available;
    char verification_uri[192];
    char user_code[64];
    char verification_url_with_code[320];
};

void run();
OnlineStatus last_status();
RegistrationInfo registration_info();
void request_registration();
bool has_stored_auth();

} // namespace connect2_client
