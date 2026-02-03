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
#include <functional>

namespace radio_view {

/**
 * @brief On-screen QWERTY keyboard for text input
 */
class Keyboard {
public:
    Keyboard(lv_obj_t* parent);
    ~Keyboard();

    void show();
    void hide();
    bool isVisible() const { return _visible; }

    void setTarget(lv_obj_t* textarea);
    void setOnDone(std::function<void()> callback) { _on_done = callback; }

private:
    lv_obj_t* _parent;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _container;
    lv_obj_t* _keyboard;
    lv_obj_t* _target_ta;
    bool _visible;
    std::function<void()> _on_done;

    void create_keyboard();
};

}  // namespace radio_view
