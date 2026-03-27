#pragma once

#include "IDialog.hpp"
#include "radio_button.hpp"

#include <gui/qr.hpp>
#include <window_header.hpp>
#include <connect2/run.hpp>

class DialogConnect2Register : public IDialog {
public:
    static void Show();

private:
    DialogConnect2Register();
    void windowEvent(window_t *sender, GUI_event_t event, void *param) override;
    void set_state_text(const char *text);
    void set_code_text(const char *code);
    void hide_qr();
    void show_qr(const char *url_with_code);
    static const char *to_error_text(connect2_client::OnlineError error);

    bool event_in_progress_ = false;
    bool qr_visible_ = false;
    bool registration_started_ = false;
    bool code_received_ = false;
    bool registration_done_ = false;
    bool terminal_button_mode_ = false;
    connect2_client::OnlineStatus last_status_ { connect2_client::ConnectionStatus::Unknown, connect2_client::OnlineError::NoError };

    window_header_t header_;
    window_icon_t icon_phone_;
    QRDynamicStringWindow<320> qr_;
    window_text_t text_state_;
    window_text_t text_detail_;
    RadioButton button_;
    char state_buffer_[96] = {};
    char detail_buffer_[96] = {};
};
