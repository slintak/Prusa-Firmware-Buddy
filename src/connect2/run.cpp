#include "run.hpp"
#include "client.hpp"

#include <common/mqtt/mqtt_client.hpp>

namespace connect2_client {

void run() {
    buddy::mqtt::Client mqtt_client;
    Client client(mqtt_client);
    client.run();
}

} // namespace connect2_client
