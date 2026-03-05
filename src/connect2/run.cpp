#include "run.hpp"
#include "client.hpp"

#include <common/mqtt/mqtt_client.hpp>

namespace connect2_client {

namespace {
buddy::mqtt::Client g_mqtt_client;
Client g_client(g_mqtt_client);
} // namespace

void run() {
    g_client.run();
}

OnlineStatus last_status() {
    return g_client.last_status();
}

void request_registration() {
    g_client.request_registration();
}

bool has_stored_auth() {
    return g_client.has_stored_auth();
}

} // namespace connect2_client
