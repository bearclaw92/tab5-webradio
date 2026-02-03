/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: MIT
 */
#include "keyboard.h"
#include "radio_view.h"

using namespace radio_view;
using namespace smooth_ui_toolkit::lvgl_cpp;

Keyboard::Keyboard(lv_obj_t* parent)
    : _parent(parent)
    , _keyboard(nullptr)
    , _target_ta(nullptr)
    , _visible(false)
{
    create_keyboard();
}

Keyboard::~Keyboard()
{
    if (_keyboard) {
        lv_obj_delete(_keyboard);
    }
}

void Keyboard::create_keyboard()
{
    // Create keyboard container
    _container = std::make_unique<Container>(_parent);
    _container->setSize(1280, 300);
    _container->align(LV_ALIGN_BOTTOM_MID, 0, 0);
    _container->setBgColor(lv_color_hex(colors::BG_SECONDARY));
    _container->setBorderWidth(0);
    _container->setRadius(0);
    _container->setHidden(true);

    // Create LVGL keyboard widget
    _keyboard = lv_keyboard_create(_container->get());
    lv_obj_set_size(_keyboard, 1240, 280);
    lv_obj_align(_keyboard, LV_ALIGN_CENTER, 0, 0);

    // Style the keyboard
    lv_obj_set_style_bg_color(_keyboard, lv_color_hex(colors::BG_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_keyboard, lv_color_hex(colors::BG_TERTIARY), LV_PART_ITEMS);
    lv_obj_set_style_text_color(_keyboard, lv_color_hex(colors::TEXT_PRIMARY), LV_PART_ITEMS);
    lv_obj_set_style_text_font(_keyboard, &lv_font_montserrat_18, LV_PART_ITEMS);
    lv_obj_set_style_border_width(_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(_keyboard, 8, LV_PART_ITEMS);
    lv_obj_set_style_pad_gap(_keyboard, 5, LV_PART_MAIN);

    // Style checked (shift) keys
    lv_obj_set_style_bg_color(_keyboard, lv_color_hex(colors::ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);

    // Set keyboard ready callback
    lv_obj_add_event_cb(_keyboard, [](lv_event_t* e) {
        Keyboard* kb = (Keyboard*)lv_event_get_user_data(e);
        uint32_t id = lv_keyboard_get_selected_button(kb->_keyboard);
        const char* txt = lv_keyboard_get_button_text(kb->_keyboard, id);

        if (txt && (strcmp(txt, LV_SYMBOL_OK) == 0 || strcmp(txt, LV_SYMBOL_KEYBOARD) == 0)) {
            kb->hide();
            if (kb->_on_done) {
                kb->_on_done();
            }
        }
    }, LV_EVENT_READY, this);

    // Also handle cancel
    lv_obj_add_event_cb(_keyboard, [](lv_event_t* e) {
        Keyboard* kb = (Keyboard*)lv_event_get_user_data(e);
        kb->hide();
    }, LV_EVENT_CANCEL, this);
}

void Keyboard::show()
{
    _container->setHidden(false);
    _visible = true;
}

void Keyboard::hide()
{
    _container->setHidden(true);
    _visible = false;
}

void Keyboard::setTarget(lv_obj_t* textarea)
{
    _target_ta = textarea;
    lv_keyboard_set_textarea(_keyboard, textarea);
}
