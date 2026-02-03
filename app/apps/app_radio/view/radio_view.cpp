/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: MIT
 */
#include "radio_view.h"
#include "wifi_config_dialog.h"
#include "../stations.h"
#include <hal/hal.h>
#include <mooncake_log.h>

using namespace radio_view;
using namespace smooth_ui_toolkit;
using namespace smooth_ui_toolkit::lvgl_cpp;

static const char* TAG = "radio_view";

RadioView::RadioView()
{
    memset(_spectrum_data, 0, sizeof(_spectrum_data));
}

RadioView::~RadioView()
{
    // Stop playback on destruction
    GetHAL()->stopRadioStream();
}

void RadioView::init()
{
    mclog::tagInfo(TAG, "Initializing radio view");

    // Get actual display size (after rotation)
    lv_display_t* disp = lv_display_get_default();
    int32_t screen_width = lv_display_get_horizontal_resolution(disp);
    int32_t screen_height = lv_display_get_vertical_resolution(disp);
    mclog::tagInfo(TAG, "Screen size: {}x{}", screen_width, screen_height);

    // Create root container (full screen)
    _root = std::make_unique<Container>(lv_screen_active());
    _root->setSize(screen_width, screen_height);
    _root->setPos(0, 0);
    _root->setBgColor(lv_color_hex(colors::BG_PRIMARY));
    _root->setBorderWidth(0);
    _root->setRadius(0);
    _root->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);

    // Build UI components
    create_wifi_status();
    create_now_playing_card();
    create_station_grid();
    create_transport_controls();
    create_wifi_settings_button();

    // Initialize state
    select_station(0);

    // Try auto-connect to saved WiFi
    try_auto_connect();
}

void RadioView::update()
{
    uint32_t now = GetHAL()->millis();

    // Update at ~20Hz
    if (now - _last_update < 50) {
        return;
    }
    _last_update = now;

    update_wifi_status();
    update_now_playing();
    update_spectrum();

    // Update WiFi dialog if open
    if (_wifi_dialog) {
        _wifi_dialog->update();
        if (_wifi_dialog->isClosed()) {
            _wifi_dialog.reset();
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                              Create UI Components                          */
/* -------------------------------------------------------------------------- */

void RadioView::create_wifi_status()
{
    // WiFi status container (top right) - stored as class member to keep alive
    _wifi_status_container = std::make_unique<Container>(_root->get());
    _wifi_status_container->align(LV_ALIGN_TOP_RIGHT, -20, 20);
    _wifi_status_container->setSize(200, 30);
    _wifi_status_container->setOpa(0);

    // Status dot
    _wifi_status_dot = std::make_unique<Container>(_wifi_status_container->get());
    _wifi_status_dot->setSize(10, 10);
    _wifi_status_dot->setRadius(5);
    _wifi_status_dot->align(LV_ALIGN_LEFT_MID, 0, 0);
    _wifi_status_dot->setBgColor(lv_color_hex(colors::ERROR_COLOR));
    _wifi_status_dot->setBorderWidth(0);

    // Status label
    _wifi_status_label = std::make_unique<Label>(_wifi_status_container->get());
    _wifi_status_label->align(LV_ALIGN_LEFT_MID, 18, 0);
    _wifi_status_label->setText("Disconnected");
    _wifi_status_label->setTextColor(lv_color_hex(colors::TEXT_SECONDARY));
    _wifi_status_label->setTextFont(&lv_font_montserrat_14);
}

void RadioView::create_now_playing_card()
{
    // Main card
    _now_playing_card = std::make_unique<Container>(_root->get());
    _now_playing_card->align(LV_ALIGN_TOP_MID, 0, 60);
    _now_playing_card->setSize(900, 280);
    _now_playing_card->setBgColor(lv_color_hex(colors::BG_SECONDARY));
    _now_playing_card->setRadius(16);
    _now_playing_card->setBorderWidth(2);
    _now_playing_card->setBorderColor(lv_color_hex(colors::BG_TERTIARY));

    // Station name (large)
    _station_name_label = std::make_unique<Label>(_now_playing_card->get());
    _station_name_label->align(LV_ALIGN_TOP_MID, 0, 25);
    _station_name_label->setText("GROOVE SALAD");
    _station_name_label->setTextColor(lv_color_hex(colors::TEXT_PRIMARY));
    _station_name_label->setTextFont(&lv_font_montserrat_36);

    // Station description
    _station_desc_label = std::make_unique<Label>(_now_playing_card->get());
    _station_desc_label->align(LV_ALIGN_TOP_MID, 0, 70);
    _station_desc_label->setText("Ambient/Downtempo");
    _station_desc_label->setTextColor(lv_color_hex(colors::TEXT_SECONDARY));
    _station_desc_label->setTextFont(&lv_font_montserrat_18);

    // Spectrum visualizer
    _spectrum_chart = std::make_unique<Chart>(_now_playing_card->get());
    _spectrum_chart->align(LV_ALIGN_CENTER, 0, 10);
    _spectrum_chart->setSize(800, 80);
    _spectrum_chart->setBgColor(lv_color_hex(colors::BG_TERTIARY));
    _spectrum_chart->setRadius(8);
    _spectrum_chart->setBorderWidth(0);
    _spectrum_chart->setStyleSize(0, 0, LV_PART_INDICATOR);
    _spectrum_chart->setPointCount(32);
    _spectrum_chart->setRange(LV_CHART_AXIS_PRIMARY_Y, 0, 255);
    _spectrum_chart->setUpdateMode(LV_CHART_UPDATE_MODE_CIRCULAR);
    _spectrum_chart->setDivLineCount(0, 0);
    _spectrum_chart->addSeries(lv_color_hex(colors::ACCENT_GLOW), LV_CHART_AXIS_PRIMARY_Y);

    // Track info
    _track_info_label = std::make_unique<Label>(_now_playing_card->get());
    _track_info_label->align(LV_ALIGN_BOTTOM_MID, 0, -50);
    _track_info_label->setText("Press Play to start streaming");
    _track_info_label->setTextColor(lv_color_hex(colors::TEXT_PRIMARY));
    _track_info_label->setTextFont(&lv_font_montserrat_16);

    // Status label (buffering/playing)
    _status_label = std::make_unique<Label>(_now_playing_card->get());
    _status_label->align(LV_ALIGN_BOTTOM_MID, 0, -25);
    _status_label->setText("");
    _status_label->setTextColor(lv_color_hex(colors::WARNING));
    _status_label->setTextFont(&lv_font_montserrat_14);

    // Buffering spinner (hidden by default)
    _buffering_spinner = std::make_unique<Spinner>(_now_playing_card->get());
    _buffering_spinner->align(LV_ALIGN_TOP_RIGHT, -20, 20);
    _buffering_spinner->setSize(30, 30);
    _buffering_spinner->setArcWidth(3, LV_PART_MAIN);
    _buffering_spinner->setArcWidth(3, LV_PART_INDICATOR);
    _buffering_spinner->setArcColor(lv_color_hex(colors::ACCENT), LV_PART_INDICATOR);
    _buffering_spinner->setAnimParams(1000, 200);
    _buffering_spinner->setHidden(true);
}

void RadioView::create_station_grid()
{
    // Station grid container
    _station_grid = std::make_unique<Container>(_root->get());
    _station_grid->align(LV_ALIGN_CENTER, 0, 100);
    _station_grid->setSize(1100, 180);
    _station_grid->setOpa(0);
    lv_obj_set_layout(_station_grid->get(), LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_station_grid->get(), LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(_station_grid->get(), LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(_station_grid->get(), 15, 0);

    // Create station cards
    for (int i = 0; i < radio::STATION_COUNT; i++) {
        auto card = std::make_unique<Container>(_station_grid->get());
        card->setSize(200, 75);
        card->setBgColor(lv_color_hex(colors::BG_SECONDARY));
        card->setRadius(12);
        card->setBorderWidth(2);
        card->setBorderColor(lv_color_hex(colors::BG_TERTIARY));

        // Station name label
        auto name_label = lv_label_create(card->get());
        lv_label_set_text(name_label, radio::STATIONS[i].name);
        lv_obj_set_style_text_color(name_label, lv_color_hex(colors::TEXT_PRIMARY), 0);
        lv_obj_set_style_text_font(name_label, &lv_font_montserrat_14, 0);
        lv_obj_align(name_label, LV_ALIGN_TOP_MID, 0, 15);

        // Station description label
        auto desc_label = lv_label_create(card->get());
        lv_label_set_text(desc_label, radio::STATIONS[i].description);
        lv_obj_set_style_text_color(desc_label, lv_color_hex(colors::TEXT_SECONDARY), 0);
        lv_obj_set_style_text_font(desc_label, &lv_font_montserrat_12, 0);
        lv_obj_align(desc_label, LV_ALIGN_BOTTOM_MID, 0, -12);

        // Click handler
        int station_idx = i;
        card->onClick().connect([this, station_idx]() {
            select_station(station_idx);
            if (_is_playing) {
                play_selected_station();
            }
        });

        _station_cards.push_back(std::move(card));
    }
}

void RadioView::create_transport_controls()
{
    // Transport container - stored as class member to keep alive
    _transport_container = std::make_unique<Container>(_root->get());
    _transport_container->align(LV_ALIGN_BOTTOM_MID, -100, -60);
    _transport_container->setSize(500, 60);
    _transport_container->setOpa(0);

    // Previous button
    _btn_prev = std::make_unique<Button>(_transport_container->get());
    _btn_prev->align(LV_ALIGN_LEFT_MID, 0, 0);
    _btn_prev->setSize(60, 50);
    _btn_prev->setBgColor(lv_color_hex(colors::BG_TERTIARY));
    _btn_prev->setRadius(12);
    _btn_prev->setBorderWidth(0);
    _btn_prev->setShadowWidth(0);
    _btn_prev->label().setText(LV_SYMBOL_PREV);
    _btn_prev->label().setTextColor(lv_color_hex(colors::TEXT_PRIMARY));
    _btn_prev->label().setTextFont(&lv_font_montserrat_20);
    _btn_prev->onClick().connect([this]() { prev_station(); });

    // Play/Pause button
    _btn_play = std::make_unique<Button>(_transport_container->get());
    _btn_play->align(LV_ALIGN_CENTER, 0, 0);
    _btn_play->setSize(100, 50);
    _btn_play->setBgColor(lv_color_hex(colors::ACCENT));
    _btn_play->setRadius(12);
    _btn_play->setBorderWidth(0);
    _btn_play->setShadowWidth(0);
    _btn_play->label().setText(LV_SYMBOL_PLAY " PLAY");
    _btn_play->label().setTextColor(lv_color_hex(colors::TEXT_PRIMARY));
    _btn_play->label().setTextFont(&lv_font_montserrat_16);
    _btn_play->onClick().connect([this]() { toggle_playback(); });

    // Next button
    _btn_next = std::make_unique<Button>(_transport_container->get());
    _btn_next->align(LV_ALIGN_RIGHT_MID, 0, 0);
    _btn_next->setSize(60, 50);
    _btn_next->setBgColor(lv_color_hex(colors::BG_TERTIARY));
    _btn_next->setRadius(12);
    _btn_next->setBorderWidth(0);
    _btn_next->setShadowWidth(0);
    _btn_next->label().setText(LV_SYMBOL_NEXT);
    _btn_next->label().setTextColor(lv_color_hex(colors::TEXT_PRIMARY));
    _btn_next->label().setTextFont(&lv_font_montserrat_20);
    _btn_next->onClick().connect([this]() { next_station(); });

    // Volume slider container - stored as class member to keep alive
    _volume_container = std::make_unique<Container>(_root->get());
    _volume_container->align(LV_ALIGN_BOTTOM_MID, 250, -60);
    _volume_container->setSize(300, 50);
    _volume_container->setOpa(0);

    // Volume icon
    _volume_label = std::make_unique<Label>(_volume_container->get());
    _volume_label->align(LV_ALIGN_LEFT_MID, 0, 0);
    _volume_label->setText(LV_SYMBOL_VOLUME_MAX);
    _volume_label->setTextColor(lv_color_hex(colors::TEXT_SECONDARY));
    _volume_label->setTextFont(&lv_font_montserrat_18);

    // Volume slider
    _volume_slider = std::make_unique<Slider>(_volume_container->get());
    _volume_slider->align(LV_ALIGN_RIGHT_MID, 0, 0);
    _volume_slider->setSize(220, 10);
    _volume_slider->setRange(0, 100);
    _volume_slider->setValue(GetHAL()->getSpeakerVolume());
    lv_obj_set_style_bg_color(_volume_slider->get(), lv_color_hex(colors::BG_TERTIARY), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_volume_slider->get(), lv_color_hex(colors::ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_volume_slider->get(), lv_color_hex(colors::TEXT_PRIMARY), LV_PART_KNOB);
    lv_obj_set_style_pad_all(_volume_slider->get(), 5, LV_PART_KNOB);

    // Volume change callback using native LVGL event
    lv_obj_add_event_cb(_volume_slider->get(), [](lv_event_t* e) {
        lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
        int vol = lv_slider_get_value(target);
        GetHAL()->setSpeakerVolume(vol);
    }, LV_EVENT_VALUE_CHANGED, this);
}

void RadioView::create_wifi_settings_button()
{
    _btn_wifi_settings = std::make_unique<Button>(_root->get());
    _btn_wifi_settings->align(LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    _btn_wifi_settings->setSize(130, 40);
    _btn_wifi_settings->setBgColor(lv_color_hex(colors::BG_TERTIARY));
    _btn_wifi_settings->setRadius(8);
    _btn_wifi_settings->setBorderWidth(0);
    _btn_wifi_settings->setShadowWidth(0);
    _btn_wifi_settings->label().setText(LV_SYMBOL_SETTINGS " WiFi");
    _btn_wifi_settings->label().setTextColor(lv_color_hex(colors::TEXT_SECONDARY));
    _btn_wifi_settings->label().setTextFont(&lv_font_montserrat_14);
    _btn_wifi_settings->onClick().connect([this]() { show_wifi_config(); });
}

/* -------------------------------------------------------------------------- */
/*                              Update Methods                                */
/* -------------------------------------------------------------------------- */

void RadioView::update_wifi_status()
{
    auto state = GetHAL()->getWifiState();
    std::string ip = GetHAL()->getWifiIp();

    switch (state) {
        case hal::HalBase::WIFI_CONNECTED:
            _wifi_status_dot->setBgColor(lv_color_hex(colors::SUCCESS));
            _wifi_status_label->setText(("WiFi: " + ip).c_str());
            break;
        case hal::HalBase::WIFI_CONNECTING:
            _wifi_status_dot->setBgColor(lv_color_hex(colors::WARNING));
            _wifi_status_label->setText("Connecting...");
            break;
        case hal::HalBase::WIFI_FAILED:
            _wifi_status_dot->setBgColor(lv_color_hex(colors::ERROR_COLOR));
            _wifi_status_label->setText("Connection Failed");
            break;
        default:
            _wifi_status_dot->setBgColor(lv_color_hex(colors::ERROR_COLOR));
            _wifi_status_label->setText("Disconnected");
            break;
    }
}

void RadioView::update_now_playing()
{
    auto state = GetHAL()->getRadioState();

    // Update status label and spinner
    switch (state) {
        case hal::HalBase::RADIO_BUFFERING:
            _status_label->setText("Buffering...");
            _status_label->setTextColor(lv_color_hex(colors::WARNING));
            _buffering_spinner->setHidden(false);
            break;
        case hal::HalBase::RADIO_PLAYING:
            _status_label->setText("Playing");
            _status_label->setTextColor(lv_color_hex(colors::SUCCESS));
            _buffering_spinner->setHidden(true);
            break;
        case hal::HalBase::RADIO_ERROR:
            _status_label->setText("Error - Check WiFi");
            _status_label->setTextColor(lv_color_hex(colors::ERROR_COLOR));
            _buffering_spinner->setHidden(true);
            _is_playing = false;
            _btn_play->label().setText(LV_SYMBOL_PLAY " PLAY");
            _btn_play->setBgColor(lv_color_hex(colors::ACCENT));
            break;
        default:
            _status_label->setText("");
            _buffering_spinner->setHidden(true);
            break;
    }

    // Update track info from metadata
    auto& metadata = GetHAL()->radioMetadata;
    if (!metadata.title.empty()) {
        _track_info_label->setText(("Now Playing: " + metadata.title).c_str());
    } else if (state == hal::HalBase::RADIO_STOPPED) {
        _track_info_label->setText("Press Play to start streaming");
    }
}

void RadioView::update_spectrum()
{
    if (GetHAL()->getRadioState() != hal::HalBase::RADIO_PLAYING) {
        return;
    }

    // Get spectrum data from HAL
    GetHAL()->getRadioSpectrum(_spectrum_data, 32);

    // Update chart
    for (int i = 0; i < 32; i++) {
        _spectrum_chart->setNextValue(0, _spectrum_data[i]);
    }
}

void RadioView::update_station_highlight()
{
    for (int i = 0; i < _station_cards.size(); i++) {
        if (i == _selected_station) {
            _station_cards[i]->setBgColor(lv_color_hex(radio::STATIONS[i].color));
            _station_cards[i]->setBorderColor(lv_color_hex(colors::ACCENT_GLOW));
            _station_cards[i]->setBorderWidth(3);
        } else {
            _station_cards[i]->setBgColor(lv_color_hex(colors::BG_SECONDARY));
            _station_cards[i]->setBorderColor(lv_color_hex(colors::BG_TERTIARY));
            _station_cards[i]->setBorderWidth(2);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                              Action Methods                                */
/* -------------------------------------------------------------------------- */

void RadioView::select_station(int index)
{
    if (index < 0 || index >= radio::STATION_COUNT) {
        return;
    }

    _selected_station = index;

    // Update now playing card
    _station_name_label->setText(radio::STATIONS[index].name);
    _station_desc_label->setText(radio::STATIONS[index].description);

    // Update card highlight
    update_station_highlight();

    // Update card border color
    _now_playing_card->setBorderColor(lv_color_hex(radio::STATIONS[index].color));
}

void RadioView::play_selected_station()
{
    // Check WiFi
    if (GetHAL()->getWifiState() != hal::HalBase::WIFI_CONNECTED) {
        _track_info_label->setText("Connect to WiFi first");
        show_wifi_config();
        return;
    }

    mclog::tagInfo(TAG, "Playing station: {}", radio::STATIONS[_selected_station].name);

    // Start streaming
    GetHAL()->startRadioStream(radio::STATIONS[_selected_station].streamUrl);

    _is_playing = true;
    _btn_play->label().setText(LV_SYMBOL_STOP " STOP");
    _btn_play->setBgColor(lv_color_hex(colors::ERROR_COLOR));
}

void RadioView::stop_playback()
{
    mclog::tagInfo(TAG, "Stopping playback");

    GetHAL()->stopRadioStream();

    _is_playing = false;
    _btn_play->label().setText(LV_SYMBOL_PLAY " PLAY");
    _btn_play->setBgColor(lv_color_hex(colors::ACCENT));
    _track_info_label->setText("Press Play to start streaming");
}

void RadioView::toggle_playback()
{
    if (_is_playing) {
        stop_playback();
    } else {
        play_selected_station();
    }
}

void RadioView::prev_station()
{
    int new_index = (_selected_station - 1 + radio::STATION_COUNT) % radio::STATION_COUNT;
    select_station(new_index);
    if (_is_playing) {
        play_selected_station();
    }
}

void RadioView::next_station()
{
    int new_index = (_selected_station + 1) % radio::STATION_COUNT;
    select_station(new_index);
    if (_is_playing) {
        play_selected_station();
    }
}

void RadioView::show_wifi_config()
{
    if (!_wifi_dialog) {
        _wifi_dialog = std::make_unique<WifiConfigDialog>(_root->get());
        _wifi_dialog->show();
    }
}

void RadioView::try_auto_connect()
{
    std::string ssid, password;
    if (GetHAL()->loadWifiConfig(ssid, password)) {
        mclog::tagInfo(TAG, "Auto-connecting to saved WiFi: {}", ssid);
        GetHAL()->connectWifiSta(ssid, password);
    }
}
