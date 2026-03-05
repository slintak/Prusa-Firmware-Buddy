/**
 * @file screen_menu_connect.cpp
 */

#include "screen_menu_connect.hpp"
#include "printers.h"
#include <window_msgbox.hpp>
#include <config_store/store_instance.hpp>
#include <option/buddy_enable_connect.h>
#include <option/buddy_enable_connect2.h>

#if BUDDY_ENABLE_CONNECT()
    #include <connect/connect.hpp>
    #include <connect/hostname.hpp>
    #include <connect/marlin_printer.hpp>
    #include "dialogs/DialogConnectReg.hpp"
#endif

#if BUDDY_ENABLE_CONNECT2()
    #include <connect2/hostname.hpp>
    #include <connect2/run.hpp>
    #include <connect2/config.hpp>
    #include "dialogs/DialogConnect2Reg.hpp"
#endif

MI_CONNECT_ENABLED::MI_CONNECT_ENABLED()
    : WI_ICON_SWITCH_OFF_ON_t(config_store().connect_enabled.get(), _(label), nullptr, is_enabled_t::yes, is_hidden_t::no) {}

void MI_CONNECT_ENABLED::OnChange([[maybe_unused]] size_t old_index) {
    config_store().connect_enabled.set(static_cast<bool>(value()));
    // Connect will catch up with new config in its next iteration
}

void MI_CONNECT_ENABLED::Loop() {
    // Make sure that if the connect is enabled as part of the wizard, this
    // gets reflected on the toggle.
    //
    // It's kind of stupid to do this repeatedly even though this changes only
    // as a result of the MI_CONNECT_REGISTER::click. But that one doesn't have
    // access to our items (AFAIK).
    //
    // It should be cheap anyway - both the eeprom access is cached in RAM and
    // the set_value checks it is different before doing anything.
    set_value(config_store().connect_enabled.get());
}

MI_CONNECT_STATUS::MI_CONNECT_STATUS()
    : WI_INFO_t(_(label), nullptr, is_enabled_t::yes, is_hidden_t::no) {
}

void MI_CONNECT_STATUS::Loop() {
#if BUDDY_ENABLE_CONNECT2()
    using S = connect2_client::ConnectionStatus;
    static constexpr EnumArray<S, const char *, 8> strings {
        { S::Unknown, N_("Unknown") },
        { S::Off, N_("Off") },
        { S::NoConfig, N_("No Config") },
        { S::Authorizing, N_("Registering") },
        { S::AuthRequired, N_("Reg. required") },
        { S::Connecting, N_("Connecting") },
        { S::Online, N_("Online") },
        { S::Error, N_("Error") },
    };
    const auto status = connect2_client::last_status();
    ChangeInformation(_(strings.get_fallback(status.status, S::Unknown)));
#elif BUDDY_ENABLE_CONNECT()
    using S = connect_client::ConnectionStatus;
    static constexpr EnumArray<S, const char *, connect_client::connection_status_cnt> strings {
        { S::Unknown, N_("Unknown") },
        { S::Off, N_("Off") },
        { S::NoConfig, N_("No Config") },
        { S::Ok, N_("Online") },
        { S::Connecting, N_("Connecting") },
        { S::Error, N_("Error") },
        { S::RegistrationRequesting, N_("Registering") },
        { S::RegistrationCode, N_("Reg. code") },
        { S::RegistrationDone, N_("Reg. done") },
        { S::RegistrationError, N_("Reg. error") },
    };

    ChangeInformation(_(strings.get_fallback(std::get<0>(connect_client::last_status()), S::Unknown)));
#else
    ChangeInformation(_("Unknown"));
#endif
}

MI_CONNECT_ERROR::MI_CONNECT_ERROR()
    : WI_INFO_t(_(label), nullptr, is_enabled_t::yes, is_hidden_t::no) {
}

void MI_CONNECT_ERROR::Loop() {
#if BUDDY_ENABLE_CONNECT2()
    using S = connect2_client::OnlineError;
    static constexpr EnumArray<S, const char *, 9> strings {
        { S::NoError, N_("---") },
        { S::Dns, N_("DNS error") },
        { S::Connection, N_("Refused") },
        { S::Tls, N_("TLS error") },
        { S::Auth, N_("Unauthorized") },
        { S::Server, N_("Srv error") },
        { S::Internal, N_("Bug") },
        { S::Network, N_("Net fail") },
        { S::Protocol, N_("Protocol err") },
    };
    const auto status = connect2_client::last_status();
    ChangeInformation(_(strings.get_fallback(status.error, S::Protocol)));
#elif BUDDY_ENABLE_CONNECT()
    using S = connect_client::OnlineError;
    static constexpr EnumArray<S, const char *, connect_client::online_error_cnt> strings {
        { S::NoError, N_("---") },
        { S::Dns, N_("DNS error") },
        { S::Connection, N_("Refused") },
        { S::Tls, N_("TLS error") },
        { S::Auth, N_("Unauthorized") },
        { S::Server, N_("Srv error") },
        { S::Internal, N_("Bug") },
        { S::Network, N_("Net fail") },
        { S::Confused, N_("Protocol err") },
    };

    ChangeInformation(_(strings.get_fallback(std::get<1>(connect_client::last_status()), S::Confused)));
#else
    ChangeInformation(_("---"));
#endif
}

MI_CONNECT_HOST::MI_CONNECT_HOST()
    : WiInfo(_(label)) {
}

void MI_CONNECT_HOST::Loop() {
    std::array<char, GetInfoLen()> hostname;
    strlcpy(hostname.data(), config_store().connect_host.get_c_str(), hostname.size());
#if BUDDY_ENABLE_CONNECT2()
    connect2_client::decompress_host(hostname.data(), hostname.size());
#elif BUDDY_ENABLE_CONNECT()
    connect_client::decompress_host(hostname.data(), hostname.size());
#endif
    ChangeInformation(hostname.data());
}

MI_CONNECT_LOAD_SETTINGS::MI_CONNECT_LOAD_SETTINGS()
    : IWindowMenuItem(_(label), nullptr, is_enabled_t::yes, is_hidden_t::no, expands_t::no) {}

void MI_CONNECT_LOAD_SETTINGS::click([[maybe_unused]] IWindowMenu &window_menu) {
#if BUDDY_ENABLE_CONNECT2()
    if (connect2_client::load_cfg_from_ini()) {
#elif BUDDY_ENABLE_CONNECT()
    if (connect_client::MarlinPrinter::load_cfg_from_ini()) {
#else
    if (false) {
#endif
        if (config_store().connect_enabled.get()) {
            MsgBoxInfo(_("Loaded successfully. Connect will activate shortly."), Responses_Ok);
        } else {
            MsgBoxInfo(_("Loaded successfully. Enable Connect to activate."), Responses_Ok);
        }
    } else {
        MsgBoxError(_("Failed to load config. Make sure the ini file downloaded from Connect is on the USB drive and try again."), Responses_Ok);
    }
}

MI_CONNECT_REGISTER::MI_CONNECT_REGISTER()
    : IWindowMenuItem(_(label), nullptr, is_enabled_t::yes, is_hidden_t::no, expands_t::no) {
}

void MI_CONNECT_REGISTER::click([[maybe_unused]] IWindowMenu &window_menu) {
#if BUDDY_ENABLE_CONNECT2()
    const bool already_registered = connect2_client::has_stored_auth();
    if (!already_registered || MsgBoxQuestion(_("Previous registration to Connect will be forgotten. Proceed?"), Responses_YesNo) == Response::Yes) {
        connect2_client::request_registration();
        DialogConnect2Register::Show();
    }
#elif BUDDY_ENABLE_CONNECT()
    const bool already_registered = strlen(config_store().connect_token.get().data()) > 0;
    if (!already_registered || MsgBoxQuestion(_("Previous registration to Connect will be forgotten. Proceed?"), Responses_YesNo) == Response::Yes) {
        DialogConnectRegister::Show();
    }
#else
    MsgBoxError(_("Connect is not available in this firmware build."), Responses_Ok);
#endif
}

ScreenMenuConnect::ScreenMenuConnect()
    : ScreenMenuConnect__(_(label)) {
}
