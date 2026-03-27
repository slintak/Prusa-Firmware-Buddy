#include "DialogConnect2Reg.hpp"

#include "RAII.hpp"
#include "../img_resources.hpp"
#include "../ScreenHandler.hpp"
#include "../lang/i18n.h"
#include <window_msgbox.hpp>

#include <guiconfig/wizard_config.hpp>

namespace {
constexpr const char *HEADER_LABEL = N_("PRUSA CONNECT");

const PhaseResponses dlg_responses = { Response::Continue, Response::_none, Response::_none, Response::_none };
const PhaseTexts dlg_texts_cancel = { { N_("Cancel") } };
const PhaseTexts dlg_texts_ok = { { N_("OK") } };

constexpr Rect16 qr_rect() {
    return Rect16 { GuiDefaults::ScreenWidth - WizardDefaults::MarginRight - 140, WizardDefaults::row_1 + 5, 140, 140 };
}

constexpr Rect16 phone_rect() {
    const Rect16 qr = qr_rect();
    return Rect16 { static_cast<int16_t>(qr.Left() - 64), static_cast<int16_t>((qr.Top() + qr.Bottom()) / 2 - 41), 64, 82 };
}

constexpr Rect16 state_rect() {
    return Rect16 { WizardDefaults::col_0, static_cast<int16_t>(WizardDefaults::row_0 + WizardDefaults::row_h * 2), static_cast<uint16_t>(phone_rect().Left() - WizardDefaults::col_0), static_cast<uint16_t>(WizardDefaults::row_h * 4) };
}

constexpr Rect16 detail_rect() {
    return Rect16 { WizardDefaults::col_0, static_cast<int16_t>(state_rect().Bottom()), static_cast<uint16_t>(phone_rect().Left() - WizardDefaults::col_0), static_cast<uint16_t>(WizardDefaults::row_h * 2) };
}
} // namespace

DialogConnect2Register::DialogConnect2Register()
    : IDialog(WizardDefaults::RectSelftestFrame)
    , header_(this, _(HEADER_LABEL))
    , icon_phone_(this, phone_rect(), &img::hand_qr_59x72)
    , qr_(this, qr_rect(), Align_t::Center())
    , text_state_(this, state_rect(), is_multiline::yes)
    , text_detail_(this, detail_rect(), is_multiline::yes)
    , button_(this, WizardDefaults::RectRadioButton(0), dlg_responses, &dlg_texts_cancel) {
    text_state_.SetAlignment(Align_t::Left());
    text_detail_.SetAlignment(Align_t::Left());

    hide_qr();
    text_state_.SetText(_("Waiting for pairing code..."));
    text_detail_.Hide();
    CaptureNormalWindow(button_);
}

void DialogConnect2Register::Show() {
    DialogConnect2Register dialog;
    Screens::Access()->gui_loop_until_dialog_closed();
}

void DialogConnect2Register::set_state_text(const char *text) {
    snprintf(state_buffer_, sizeof(state_buffer_), "%s", text ? text : "");
    text_state_.SetText(string_view_utf8::MakeRAM(state_buffer_));
    text_state_.Invalidate();
}

void DialogConnect2Register::set_code_text(const char *code) {
    if (code == nullptr || code[0] == '\0') {
        text_detail_.Hide();
        return;
    }
    snprintf(detail_buffer_, sizeof(detail_buffer_), "Code: %s", code);
    text_detail_.SetText(string_view_utf8::MakeRAM(detail_buffer_));
    text_detail_.Show();
    text_detail_.Invalidate();
}

void DialogConnect2Register::hide_qr() {
    qr_.Hide();
    icon_phone_.Hide();
    qr_visible_ = false;
}

void DialogConnect2Register::show_qr(const char *url_with_code) {
    if (url_with_code == nullptr || url_with_code[0] == '\0') {
        return;
    }
    qr_.get_string_builder().append_string(url_with_code);
    qr_.Invalidate();
    if (!qr_visible_) {
        qr_.Show();
        icon_phone_.Show();
        qr_visible_ = true;
    }
}

const char *DialogConnect2Register::to_error_text(connect2_client::OnlineError error) {
    switch (error) {
    case connect2_client::OnlineError::Dns:
        return "DNS error";
    case connect2_client::OnlineError::Connection:
        return "Connection error";
    case connect2_client::OnlineError::Tls:
        return "TLS error";
    case connect2_client::OnlineError::Auth:
        return "Unauthorized";
    case connect2_client::OnlineError::Server:
        return "Server error";
    case connect2_client::OnlineError::Internal:
        return "Internal error";
    case connect2_client::OnlineError::Network:
        return "Network error";
    case connect2_client::OnlineError::Protocol:
        return "Protocol error";
    case connect2_client::OnlineError::NoError:
    default:
        return "";
    }
}

void DialogConnect2Register::windowEvent(window_t *sender, GUI_event_t event, void *param) {
    if (event_in_progress_) {
        return;
    }
    AutoRestore avoid_recursion(event_in_progress_, true);

    switch (event) {
    case GUI_event_t::CHILD_CLICK: {
        bool close = true;
        if (!registration_done_
            && (last_status_.status == connect2_client::ConnectionStatus::Authorizing
                || last_status_.status == connect2_client::ConnectionStatus::Connecting)) {
            close = MsgBoxWarning(_("Prusa Connect setup is not finished. Do you want to exit and abort the process?"), Responses_YesNo)
                == Response::Yes;
            if (close) {
                connect2_client::cancel_registration();
            }
        }
        if (!close) {
            return;
        }
        Screens::Access()->Close();
        return;
    }
    case GUI_event_t::LOOP: {
        const auto status = connect2_client::last_status();
        const auto reg = connect2_client::registration_info();
        const auto set_button_mode = [this](bool terminal) {
            if (terminal_button_mode_ == terminal) {
                return;
            }
            terminal_button_mode_ = terminal;
            button_.Change(dlg_responses, terminal ? &dlg_texts_ok : &dlg_texts_cancel);
        };

        if (status.status == connect2_client::ConnectionStatus::Authorizing
            || status.status == connect2_client::ConnectionStatus::Connecting) {
            registration_started_ = true;
        }

        if (registration_started_ && status.status == connect2_client::ConnectionStatus::Online) {
            registration_done_ = true;
            set_button_mode(true);
            hide_qr();
            text_state_.SetText(_("Registration successful. Printer is now connected."));
            text_state_.Invalidate();
            text_detail_.Hide();
        } else if (registration_done_) {
            // Keep successful final screen stable even if connection state flips briefly.
        } else if (registration_started_ && status.status == connect2_client::ConnectionStatus::Authorizing && reg.available) {
            set_button_mode(false);
            code_received_ = true;
            show_qr(reg.verification_url_with_code);
            text_state_.SetText(_("Scan the QR code and complete sign-in."));
            text_state_.Invalidate();
            set_code_text(reg.user_code);
        } else if (registration_started_ && status.status == connect2_client::ConnectionStatus::Authorizing) {
            set_button_mode(false);
            if (code_received_) {
                text_state_.SetText(_("Waiting for confirmation in browser..."));
            } else {
                hide_qr();
                text_state_.SetText(_("Waiting for pairing code..."));
            }
            text_state_.Invalidate();
            if (!code_received_) {
                text_detail_.Hide();
            }
        } else if (registration_started_ && status.status == connect2_client::ConnectionStatus::Connecting) {
            set_button_mode(false);
            hide_qr();
            text_state_.SetText(_("Code accepted. Finalizing registration..."));
            text_state_.Invalidate();
            text_detail_.Hide();
        } else if (status.status == connect2_client::ConnectionStatus::AuthRequired) {
            set_button_mode(true);
            hide_qr();
            text_state_.SetText(_("Authorization required. Start registration again."));
            text_state_.Invalidate();
            text_detail_.Hide();
        } else if (status.status == connect2_client::ConnectionStatus::Error) {
            set_button_mode(true);
            hide_qr();
            snprintf(state_buffer_, sizeof(state_buffer_), "Registration failed: %s", to_error_text(status.error));
            text_state_.SetText(string_view_utf8::MakeRAM(state_buffer_));
            text_state_.Invalidate();
            text_detail_.Hide();
        }

        last_status_ = status;
        return;
    }
    default:
        IDialog::windowEvent(sender, event, param);
        return;
    }
}
