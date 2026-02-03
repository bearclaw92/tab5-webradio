/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: MIT
 */
#include "wifi_config_dialog.h"
#include "keyboard.h"
#include "radio_view.h"
#include <hal/hal.h>
#include <mooncake_log.h>

using namespace radio_view;
using namespace smooth_ui_toolkit::lvgl_cpp;

static const char* TAG = "wifi_dialog";

WifiConfigDialog::WifiConfigDialog(lv_obj_t* parent)
    : _parent(parent)
    , _closed(false)
    , _password_visible(false)
    , _active_textarea(nullptr)
{
    create_dialog();
}

WifiConfigDialog::~WifiConfigDialog()
{
}

void WifiConfigDialog::create_dialog()
{
    // Create semi-transparent backdrop
    _backdrop = std::make_unique<Container>(_parent);
    _backdrop->setSize(1280, 720);
    _backdrop->setPos(0, 0);
    _backdrop->setBgColor(lv_color_hex(0x000000));
    _backdrop->setBgOpa(LV_OPA_70);
    _backdrop->setBorderWidth(0);
    _backdrop->onClick().connect([this]() {
        // Close on backdrop click if keyboard not visible
        if (!_keyboard || !_keyboard->isVisible()) {
            hide();
        }
    });

    // Create dialog container
    _dialog = std::make_unique<Container>(_backdrop->get());
    _dialog->setSize(500, 380);
    _dialog->align(LV_ALIGN_TOP_MID, 0, 80);
    _dialog->setBgColor(lv_color_hex(colors::BG_SECONDARY));
    _dialog->setRadius(16);
    _dialog->setBorderWidth(2);
    _dialog->setBorderColor(lv_color_hex(colors::BG_TERTIARY));
    // Stop click propagation
    _dialog->onClick().connect([]() {});

    // Title
    _title_label = std::make_unique<Label>(_dialog->get());
    _title_label->align(LV_ALIGN_TOP_MID, 0, 20);
    _title_label->setText("WiFi Configuration");
    _title_label->setTextColor(lv_color_hex(colors::TEXT_PRIMARY));
    _title_label->setTextFont(&lv_font_montserrat_22);

    // SSID label
    _ssid_label = std::make_unique<Label>(_dialog->get());
    _ssid_label->align(LV_ALIGN_TOP_LEFT, 30, 70);
    _ssid_label->setText("SSID (Network Name)");
    _ssid_label->setTextColor(lv_color_hex(colors::TEXT_SECONDARY));
    _ssid_label->setTextFont(&lv_font_montserrat_14);

    // SSID textarea
    _ssid_textarea = lv_textarea_create(_dialog->get());
    lv_obj_set_size(_ssid_textarea, 440, 45);
    lv_obj_align(_ssid_textarea, LV_ALIGN_TOP_LEFT, 30, 95);
    lv_textarea_set_one_line(_ssid_textarea, true);
    lv_textarea_set_placeholder_text(_ssid_textarea, "Enter WiFi name");
    lv_obj_set_style_bg_color(_ssid_textarea, lv_color_hex(colors::BG_TERTIARY), 0);
    lv_obj_set_style_text_color(_ssid_textarea, lv_color_hex(colors::TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(_ssid_textarea, &lv_font_montserrat_16, 0);
    lv_obj_set_style_border_color(_ssid_textarea, lv_color_hex(colors::ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(_ssid_textarea, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(_ssid_textarea, 8, 0);

    // Load saved SSID if available
    std::string saved_ssid, saved_pass;
    if (GetHAL()->loadWifiConfig(saved_ssid, saved_pass)) {
        lv_textarea_set_text(_ssid_textarea, saved_ssid.c_str());
    }

    // SSID click to show keyboard
    lv_obj_add_event_cb(_ssid_textarea, [](lv_event_t* e) {
        WifiConfigDialog* dlg = (WifiConfigDialog*)lv_event_get_user_data(e);
        dlg->show_keyboard_for(dlg->_ssid_textarea);
    }, LV_EVENT_CLICKED, this);

    // Password label
    _password_label = std::make_unique<Label>(_dialog->get());
    _password_label->align(LV_ALIGN_TOP_LEFT, 30, 155);
    _password_label->setText("Password");
    _password_label->setTextColor(lv_color_hex(colors::TEXT_SECONDARY));
    _password_label->setTextFont(&lv_font_montserrat_14);

    // Password textarea
    _password_textarea = lv_textarea_create(_dialog->get());
    lv_obj_set_size(_password_textarea, 380, 45);
    lv_obj_align(_password_textarea, LV_ALIGN_TOP_LEFT, 30, 180);
    lv_textarea_set_one_line(_password_textarea, true);
    lv_textarea_set_placeholder_text(_password_textarea, "Enter password");
    lv_textarea_set_password_mode(_password_textarea, true);
    lv_obj_set_style_bg_color(_password_textarea, lv_color_hex(colors::BG_TERTIARY), 0);
    lv_obj_set_style_text_color(_password_textarea, lv_color_hex(colors::TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(_password_textarea, &lv_font_montserrat_16, 0);
    lv_obj_set_style_border_color(_password_textarea, lv_color_hex(colors::ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(_password_textarea, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(_password_textarea, 8, 0);

    // Password click to show keyboard
    lv_obj_add_event_cb(_password_textarea, [](lv_event_t* e) {
        WifiConfigDialog* dlg = (WifiConfigDialog*)lv_event_get_user_data(e);
        dlg->show_keyboard_for(dlg->_password_textarea);
    }, LV_EVENT_CLICKED, this);

    // Show/hide password button
    _show_password_btn = std::make_unique<Button>(_dialog->get());
    _show_password_btn->align(LV_ALIGN_TOP_LEFT, 420, 180);
    _show_password_btn->setSize(50, 45);
    _show_password_btn->setBgColor(lv_color_hex(colors::BG_TERTIARY));
    _show_password_btn->setRadius(8);
    _show_password_btn->setBorderWidth(0);
    _show_password_btn->setShadowWidth(0);
    _show_password_btn->label().setText(LV_SYMBOL_EYE_CLOSE);
    _show_password_btn->label().setTextColor(lv_color_hex(colors::TEXT_SECONDARY));
    _show_password_btn->onClick().connect([this]() { toggle_password_visibility(); });

    // Status label
    _status_label = std::make_unique<Label>(_dialog->get());
    _status_label->align(LV_ALIGN_TOP_MID, 0, 240);
    _status_label->setText("");
    _status_label->setTextColor(lv_color_hex(colors::WARNING));
    _status_label->setTextFont(&lv_font_montserrat_14);

    // Connecting spinner (hidden by default)
    _connecting_spinner = std::make_unique<Spinner>(_dialog->get());
    _connecting_spinner->align(LV_ALIGN_TOP_MID, 80, 235);
    _connecting_spinner->setSize(25, 25);
    _connecting_spinner->setArcWidth(3, LV_PART_MAIN);
    _connecting_spinner->setArcWidth(3, LV_PART_INDICATOR);
    _connecting_spinner->setArcColor(lv_color_hex(colors::ACCENT), LV_PART_INDICATOR);
    _connecting_spinner->setHidden(true);

    // Cancel button
    _cancel_btn = std::make_unique<Button>(_dialog->get());
    _cancel_btn->align(LV_ALIGN_BOTTOM_LEFT, 30, -30);
    _cancel_btn->setSize(120, 45);
    _cancel_btn->setBgColor(lv_color_hex(colors::BG_TERTIARY));
    _cancel_btn->setRadius(8);
    _cancel_btn->setBorderWidth(0);
    _cancel_btn->setShadowWidth(0);
    _cancel_btn->label().setText("Cancel");
    _cancel_btn->label().setTextColor(lv_color_hex(colors::TEXT_SECONDARY));
    _cancel_btn->label().setTextFont(&lv_font_montserrat_16);
    _cancel_btn->onClick().connect([this]() { hide(); });

    // Connect button
    _connect_btn = std::make_unique<Button>(_dialog->get());
    _connect_btn->align(LV_ALIGN_BOTTOM_RIGHT, -30, -30);
    _connect_btn->setSize(120, 45);
    _connect_btn->setBgColor(lv_color_hex(colors::ACCENT));
    _connect_btn->setRadius(8);
    _connect_btn->setBorderWidth(0);
    _connect_btn->setShadowWidth(0);
    _connect_btn->label().setText("Connect");
    _connect_btn->label().setTextColor(lv_color_hex(colors::TEXT_PRIMARY));
    _connect_btn->label().setTextFont(&lv_font_montserrat_16);
    _connect_btn->onClick().connect([this]() { try_connect(); });

    // Create keyboard (initially hidden)
    _keyboard = std::make_unique<Keyboard>(_backdrop->get());
    _keyboard->setOnDone([this]() {
        // Move dialog back up when keyboard closes
        _dialog->align(LV_ALIGN_TOP_MID, 0, 80);
    });
}

void WifiConfigDialog::show()
{
    _backdrop->setHidden(false);
    _closed = false;
}

void WifiConfigDialog::hide()
{
    if (_keyboard) {
        _keyboard->hide();
    }
    _backdrop->setHidden(true);
    _closed = true;
}

void WifiConfigDialog::update()
{
    auto state = GetHAL()->getWifiState();

    if (state == hal::HalBase::WIFI_CONNECTED) {
        _status_label->setText("Connected!");
        _status_label->setTextColor(lv_color_hex(colors::SUCCESS));
        _connecting_spinner->setHidden(true);

        // Auto-close after successful connection
        static uint32_t connected_time = 0;
        if (connected_time == 0) {
            connected_time = GetHAL()->millis();
        } else if (GetHAL()->millis() - connected_time > 1500) {
            connected_time = 0;
            hide();
        }
    } else if (state == hal::HalBase::WIFI_FAILED) {
        _status_label->setText("Connection failed. Check credentials.");
        _status_label->setTextColor(lv_color_hex(colors::ERROR_COLOR));
        _connecting_spinner->setHidden(true);
    }
}

void WifiConfigDialog::try_connect()
{
    const char* ssid     = lv_textarea_get_text(_ssid_textarea);
    const char* password = lv_textarea_get_text(_password_textarea);

    if (!ssid || strlen(ssid) == 0) {
        _status_label->setText("Please enter a WiFi name");
        _status_label->setTextColor(lv_color_hex(colors::ERROR_COLOR));
        return;
    }

    mclog::tagInfo(TAG, "Attempting to connect to: {}", ssid);

    // Hide keyboard if showing
    if (_keyboard) {
        _keyboard->hide();
    }

    // Show connecting state
    _status_label->setText("Connecting...");
    _status_label->setTextColor(lv_color_hex(colors::WARNING));
    _connecting_spinner->setHidden(false);

    // Save config
    GetHAL()->saveWifiConfig(ssid, password);

    // Start connection in background
    GetHAL()->connectWifiSta(ssid, password);
}

void WifiConfigDialog::toggle_password_visibility()
{
    _password_visible = !_password_visible;
    lv_textarea_set_password_mode(_password_textarea, !_password_visible);
    _show_password_btn->label().setText(_password_visible ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
}

void WifiConfigDialog::show_keyboard_for(lv_obj_t* textarea)
{
    _active_textarea = textarea;
    _keyboard->setTarget(textarea);
    _keyboard->show();

    // Move dialog up to make room for keyboard
    _dialog->align(LV_ALIGN_TOP_MID, 0, 20);
}
