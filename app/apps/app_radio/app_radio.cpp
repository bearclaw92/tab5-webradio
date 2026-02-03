/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_radio.h"
#include "view/radio_view.h"
#include <mooncake_log.h>
#include <hal/hal.h>

static const char* TAG = "app_radio";

AppRadio::AppRadio()
{
    setAppName("SomaFM Radio");
}

AppRadio::~AppRadio()
{
}

void AppRadio::onCreate()
{
    mclog::tagInfo(TAG, "onCreate");
}

void AppRadio::onOpen()
{
    mclog::tagInfo(TAG, "onOpen");

    LvglLockGuard lock;
    _view = std::make_unique<radio_view::RadioView>();
    _view->init();
}

void AppRadio::onRunning()
{
    if (_view) {
        LvglLockGuard lock;
        _view->update();
    }
}

void AppRadio::onClose()
{
    mclog::tagInfo(TAG, "onClose");

    // Stop any playing stream
    GetHAL()->stopRadioStream();

    LvglLockGuard lock;
    _view.reset();
}
