/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <lvgl.h>
#include <smooth_ui_toolkit.h>
#include <smooth_lvgl.h>

namespace radio_view {

// Forward declarations
class Keyboard;
class WifiConfigDialog;

/**
 * @brief Modern dark theme color palette
 */
namespace colors {
    const uint32_t BG_PRIMARY     = 0x0D0D0D;  // Near black background
    const uint32_t BG_SECONDARY   = 0x1A1A1A;  // Card backgrounds
    const uint32_t BG_TERTIARY    = 0x2D2D2D;  // Elevated elements
    const uint32_t ACCENT         = 0x6366F1;  // Indigo accent
    const uint32_t ACCENT_GLOW    = 0x818CF8;  // Light indigo
    const uint32_t TEXT_PRIMARY   = 0xF5F5F5;  // White text
    const uint32_t TEXT_SECONDARY = 0xA3A3A3;  // Gray text
    const uint32_t SUCCESS        = 0x22C55E;  // Green (connected)
    const uint32_t WARNING        = 0xF59E0B;  // Amber (buffering)
    const uint32_t ERROR_COLOR    = 0xEF4444;  // Red (error)
}  // namespace colors

/**
 * @brief Main radio player view
 */
class RadioView {
public:
    RadioView();
    ~RadioView();

    void init();
    void update();

private:
    // Root container
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _root;

    // WiFi status indicator
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _wifi_status_container;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _wifi_status_label;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _wifi_status_dot;

    // Now playing card
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _now_playing_card;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _station_name_label;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _station_desc_label;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _track_info_label;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _status_label;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Chart> _spectrum_chart;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Spinner> _buffering_spinner;

    // Station grid
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _station_grid;
    std::vector<std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container>> _station_cards;

    // Transport controls
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _transport_container;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button> _btn_prev;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button> _btn_play;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button> _btn_next;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _volume_container;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Slider> _volume_slider;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _volume_label;

    // WiFi settings button
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button> _btn_wifi_settings;

    // Dialogs
    std::unique_ptr<WifiConfigDialog> _wifi_dialog;

    // State
    int _selected_station   = 0;
    bool _is_playing        = false;
    uint32_t _last_update   = 0;
    uint8_t _spectrum_data[32];

    // Methods
    void create_wifi_status();
    void create_now_playing_card();
    void create_station_grid();
    void create_transport_controls();
    void create_wifi_settings_button();

    void update_wifi_status();
    void update_now_playing();
    void update_spectrum();
    void update_station_highlight();

    void select_station(int index);
    void play_selected_station();
    void stop_playback();
    void toggle_playback();
    void prev_station();
    void next_station();
    void show_wifi_config();

    void try_auto_connect();
};

}  // namespace radio_view
