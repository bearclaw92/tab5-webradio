/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <memory>

namespace radio_view {
class RadioView;
}

/**
 * @brief SomaFM Web Radio Player Application
 * Standalone app for streaming internet radio stations
 */
class AppRadio : public mooncake::AppAbility {
public:
    AppRadio();
    ~AppRadio() override;

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    std::unique_ptr<radio_view::RadioView> _view;
};
