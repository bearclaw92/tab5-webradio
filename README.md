# Tab5 Web Radio - SomaFM Player

A standalone internet radio player for the M5Stack Tab5 that streams SomaFM stations with a modern LVGL UI.

## Features

- Stream SomaFM internet radio stations
- Modern dark-themed UI with spectrum visualizer
- On-screen QWERTY keyboard for WiFi configuration
- ICY metadata display (current track info)
- Persistent settings (WiFi credentials, last station, volume)

## Based On

This project is based on the [M5Tab5-UserDemo](https://github.com/m5stack/M5Tab5-UserDemo) codebase.

## Build

### Fetch Dependencies

```bash
python ./fetch_repos.py
```

### Desktop Build (for development/testing)

#### Tool Chains

```bash
sudo apt install build-essential cmake libsdl2-dev
```

#### Build & Run

```bash
mkdir build && cd build
cmake .. && make -j8
./desktop/app_desktop_build
```

### ESP32 Build (for Tab5 hardware)

#### Tool Chains

[ESP-IDF v5.4.2](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32s3/index.html)

#### Build & Flash

```bash
cd platforms/tab5
idf.py build
idf.py flash
```

## SomaFM Stations

- Groove Salad - Ambient/Downtempo
- Drone Zone - Atmospheric Textures
- Space Station Soma - Spaced-out Ambient
- Deep Space One - Deep Ambient
- DEF CON Radio - Hacker Tunes
- Secret Agent - Lounge/Spy Music
- Lush - Sensuous Vocals
- Boot Liquor - Americana/Roots
- The Trip - Progressive House
- cliqhop idm - IDM/Glitch

## License

MIT License - See [LICENSE](LICENSE) file.

## Acknowledgments

- [SomaFM](https://somafm.com/) - Internet radio stations
- [M5Stack](https://m5stack.com/) - Tab5 hardware
- [LVGL](https://lvgl.io/) - UI framework
- [esp-audio-player](https://github.com/chmorgan/esp-audio-player) - MP3 decoding
