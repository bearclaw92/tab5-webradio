/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>

namespace radio {

/**
 * @brief SomaFM station definition
 */
struct Station {
    const char* id;           // Station identifier
    const char* name;         // Display name
    const char* description;  // Short description
    const char* streamUrl;    // MP3 stream URL (128kbps)
    uint32_t color;           // UI accent color
};

/**
 * @brief List of SomaFM stations
 * All streams are 128kbps MP3 format
 */
static const Station STATIONS[] = {
    {
        "groovesalad",
        "Groove Salad",
        "Ambient/Downtempo",
        "https://ice1.somafm.com/groovesalad-128-mp3",
        0x7B68EE  // Medium slate blue
    },
    {
        "dronezone",
        "Drone Zone",
        "Atmospheric Textures",
        "https://ice1.somafm.com/dronezone-128-mp3",
        0x4682B4  // Steel blue
    },
    {
        "spacestation",
        "Space Station Soma",
        "Spaced-out Ambient",
        "https://ice1.somafm.com/spacestation-128-mp3",
        0x191970  // Midnight blue
    },
    {
        "deepspaceone",
        "Deep Space One",
        "Deep Ambient",
        "https://ice1.somafm.com/deepspaceone-128-mp3",
        0x2F4F4F  // Dark slate gray
    },
    {
        "defcon",
        "DEF CON Radio",
        "Hacker Tunes",
        "https://ice1.somafm.com/defcon-128-mp3",
        0x00FF00  // Lime green
    },
    {
        "secretagent",
        "Secret Agent",
        "Lounge/Spy Music",
        "https://ice1.somafm.com/secretagent-128-mp3",
        0xDC143C  // Crimson
    },
    {
        "lush",
        "Lush",
        "Sensuous Vocals",
        "https://ice1.somafm.com/lush-128-mp3",
        0xFF69B4  // Hot pink
    },
    {
        "bootliquor",
        "Boot Liquor",
        "Americana/Roots",
        "https://ice1.somafm.com/bootliquor-128-mp3",
        0x8B4513  // Saddle brown
    },
    {
        "thetrip",
        "The Trip",
        "Progressive House",
        "https://ice1.somafm.com/thetrip-128-mp3",
        0xFF4500  // Orange red
    },
    {
        "cliqhop",
        "cliqhop idm",
        "IDM/Glitch",
        "https://ice1.somafm.com/cliqhop-128-mp3",
        0x9400D3  // Dark violet
    },
};

static const int STATION_COUNT = sizeof(STATIONS) / sizeof(STATIONS[0]);

}  // namespace radio
