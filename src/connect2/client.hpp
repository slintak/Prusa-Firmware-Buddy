#pragma once

#include <atomic>
#include <cstdint>

#include <common/mqtt/mqtt_client.hpp>
#include <common/oauth/device_flow.hpp>
#include <common/utils/exponential_backoff.hpp>
#include "config.hpp"
#include "oauth_storage.hpp"
#include "run.hpp"
#include "telemetry.hpp"

namespace connect2_client {

class Client {
public:
    explicit Client(buddy::mqtt::Client &mqtt_client);
    void run();
    void request_registration();
    OnlineStatus last_status() const;
    RegistrationInfo registration_info() const;
    bool has_stored_auth() const;

private:
    enum class State : uint8_t {
        Disabled,
        Disconnected,
        Connecting,
        Connected,
        Backoff,
        RegistrationRequired,
    };

    void step();
    void sleep_idle(uint32_t ms);
    void refresh_config(uint32_t now_ms);
    void enter_backoff(uint32_t now_ms, uint32_t min_delay_ms = 0);
    bool run_oauth_device_flow();
    bool run_oauth_refresh();
    void load_auth_from_storage();
    void apply_auth_identity();
    bool has_valid_auth() const;
    bool should_refresh_token(uint32_t now_epoch_s) const;
    bool network_ready();

    static uint32_t config_hash(const Config &cfg);
    static bool is_idle_state(State state);

    buddy::mqtt::Client &mqtt_client_;
    Telemetry telemetry_;
    Config cfg_ {};
    uint32_t last_cfg_hash_ = 0;
    State state_ = State::Disabled;
    uint32_t next_cfg_check_ms_ = 0;
    uint32_t next_action_ms_ = 0;
    OAuthStorageData auth_ {};
    bool last_net_ready_ = false;
    buddy::ExponentialBackoff<uint32_t, 100, 60000> backoff_;
    std::atomic<bool> registration_requested_ { false };
    std::atomic<bool> registration_info_valid_ { false };
    char verification_uri_[192] = {};
    char user_code_[64] = {};
    char verification_url_with_code_[320] = {};
    std::atomic<ConnectionStatus> status_ { ConnectionStatus::Unknown };
    std::atomic<OnlineError> error_ { OnlineError::NoError };
    std::atomic<bool> has_stored_auth_ { false };
};

} // namespace connect2_client
