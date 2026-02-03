/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <lvgl.h>
#include <smooth_ui_toolkit.h>
#include <smooth_lvgl.h>
#include <memory>
#include <string>

namespace radio_view {

class Keyboard;

/**
 * @brief WiFi configuration dialog with SSID/password input
 */
class WifiConfigDialog {
public:
    WifiConfigDialog(lv_obj_t* parent);
    ~WifiConfigDialog();

    void show();
    void hide();
    void update();
    bool isClosed() const { return _closed; }

private:
    lv_obj_t* _parent;
    bool _closed;

    // Backdrop
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _backdrop;

    // Dialog container
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _dialog;

    // Title
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _title_label;

    // SSID input
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _ssid_label;
    lv_obj_t* _ssid_textarea;

    // Password input
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _password_label;
    lv_obj_t* _password_textarea;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button> _show_password_btn;
    bool _password_visible;

    // Status
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _status_label;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Spinner> _connecting_spinner;

    // Buttons
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button> _connect_btn;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button> _cancel_btn;

    // Keyboard
    std::unique_ptr<Keyboard> _keyboard;
    lv_obj_t* _active_textarea;

    void create_dialog();
    void try_connect();
    void toggle_password_visibility();
    void show_keyboard_for(lv_obj_t* textarea);
};

}  // namespace radio_view
