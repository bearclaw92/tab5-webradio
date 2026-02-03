/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include <mooncake_log.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_http_client.h>
#include <bsp/m5stack_tab5.h>
#include <audio_player.h>
#include <cmath>

static const char* TAG = "radio";

/* -------------------------------------------------------------------------- */
/*                              Ring Buffer                                   */
/* -------------------------------------------------------------------------- */
#define RING_BUFFER_SIZE     (64 * 1024)  // 64KB ring buffer
#define PREBUFFER_SIZE       (16 * 1024)  // 16KB prebuffer before playback
#define SPECTRUM_BANDS       32

struct RingBuffer {
    uint8_t* buffer      = nullptr;
    size_t size          = 0;
    size_t writePos      = 0;
    size_t readPos       = 0;
    size_t dataAvailable = 0;
    SemaphoreHandle_t mutex;

    bool init(size_t bufferSize)
    {
        buffer = (uint8_t*)heap_caps_malloc(bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buffer) {
            buffer = (uint8_t*)malloc(bufferSize);
        }
        if (!buffer) {
            return false;
        }
        size  = bufferSize;
        mutex = xSemaphoreCreateMutex();
        return mutex != nullptr;
    }

    void deinit()
    {
        if (buffer) {
            free(buffer);
            buffer = nullptr;
        }
        if (mutex) {
            vSemaphoreDelete(mutex);
            mutex = nullptr;
        }
    }

    void reset()
    {
        if (xSemaphoreTake(mutex, portMAX_DELAY)) {
            writePos      = 0;
            readPos       = 0;
            dataAvailable = 0;
            xSemaphoreGive(mutex);
        }
    }

    size_t write(const uint8_t* data, size_t len)
    {
        if (!xSemaphoreTake(mutex, pdMS_TO_TICKS(100))) {
            return 0;
        }

        size_t freeSpace = size - dataAvailable;
        size_t toWrite   = (len < freeSpace) ? len : freeSpace;

        for (size_t i = 0; i < toWrite; i++) {
            buffer[writePos] = data[i];
            writePos         = (writePos + 1) % size;
        }
        dataAvailable += toWrite;

        xSemaphoreGive(mutex);
        return toWrite;
    }

    size_t read(uint8_t* data, size_t len)
    {
        if (!xSemaphoreTake(mutex, pdMS_TO_TICKS(100))) {
            return 0;
        }

        size_t toRead = (len < dataAvailable) ? len : dataAvailable;

        for (size_t i = 0; i < toRead; i++) {
            data[i]  = buffer[readPos];
            readPos  = (readPos + 1) % size;
        }
        dataAvailable -= toRead;

        xSemaphoreGive(mutex);
        return toRead;
    }

    size_t available()
    {
        if (!xSemaphoreTake(mutex, pdMS_TO_TICKS(100))) {
            return 0;
        }
        size_t avail = dataAvailable;
        xSemaphoreGive(mutex);
        return avail;
    }

    int bufferPercent()
    {
        return (available() * 100) / size;
    }
};

/* -------------------------------------------------------------------------- */
/*                           Radio Stream State                               */
/* -------------------------------------------------------------------------- */
struct RadioStreamState {
    SemaphoreHandle_t mutex = nullptr;
    hal::HalBase::RadioState_t state = hal::HalBase::RADIO_STOPPED;
    std::string currentUrl;
    std::string streamTitle;
    int icyMetaInt           = 0;
    int bytesUntilMeta       = 0;
    bool stopRequested       = false;
    TaskHandle_t httpTask    = nullptr;
    TaskHandle_t audioTask   = nullptr;
    RingBuffer ringBuffer;
    uint8_t spectrum[SPECTRUM_BANDS] = {0};
    HalEsp32* hal                    = nullptr;
};

static RadioStreamState s_radio;

/* -------------------------------------------------------------------------- */
/*                           ICY Metadata Parsing                             */
/* -------------------------------------------------------------------------- */
static void parse_icy_metadata(const char* metadata, size_t len)
{
    // Format: StreamTitle='Artist - Track';StreamUrl='...';
    const char* titleStart = strstr(metadata, "StreamTitle='");
    if (titleStart) {
        titleStart += 13;  // Skip "StreamTitle='"
        const char* titleEnd = strchr(titleStart, '\'');
        if (titleEnd && titleEnd > titleStart) {
            size_t titleLen = titleEnd - titleStart;
            if (titleLen > 0 && titleLen < 256) {
                if (xSemaphoreTake(s_radio.mutex, pdMS_TO_TICKS(100))) {
                    s_radio.streamTitle = std::string(titleStart, titleLen);
                    if (s_radio.hal) {
                        s_radio.hal->radioMetadata.title = s_radio.streamTitle;
                    }
                    xSemaphoreGive(s_radio.mutex);
                    mclog::tagInfo(TAG, "Now playing: {}", s_radio.streamTitle);
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                           HTTP Event Handler                               */
/* -------------------------------------------------------------------------- */
static esp_err_t http_event_handler(esp_http_client_event_t* evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            // Check for ICY metadata interval
            if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
                s_radio.icyMetaInt     = atoi(evt->header_value);
                s_radio.bytesUntilMeta = s_radio.icyMetaInt;
                mclog::tagInfo(TAG, "ICY metadata interval: {}", s_radio.icyMetaInt);
            } else if (strcasecmp(evt->header_key, "icy-name") == 0) {
                if (xSemaphoreTake(s_radio.mutex, pdMS_TO_TICKS(100))) {
                    if (s_radio.hal) {
                        s_radio.hal->radioMetadata.station = evt->header_value;
                    }
                    xSemaphoreGive(s_radio.mutex);
                    mclog::tagInfo(TAG, "Station: {}", evt->header_value);
                }
            } else if (strcasecmp(evt->header_key, "icy-br") == 0) {
                if (xSemaphoreTake(s_radio.mutex, pdMS_TO_TICKS(100))) {
                    if (s_radio.hal) {
                        s_radio.hal->radioMetadata.bitrate = atoi(evt->header_value);
                    }
                    xSemaphoreGive(s_radio.mutex);
                }
            }
            break;

        case HTTP_EVENT_ON_DATA:
            if (s_radio.stopRequested) {
                return ESP_FAIL;  // Stop the HTTP request
            }

            // Handle ICY metadata if present
            if (s_radio.icyMetaInt > 0) {
                uint8_t* data     = (uint8_t*)evt->data;
                int dataLen       = evt->data_len;
                int dataPos       = 0;

                while (dataPos < dataLen) {
                    if (s_radio.bytesUntilMeta > 0) {
                        // Write audio data
                        int audioBytes = (dataLen - dataPos < s_radio.bytesUntilMeta)
                                             ? (dataLen - dataPos)
                                             : s_radio.bytesUntilMeta;
                        s_radio.ringBuffer.write(data + dataPos, audioBytes);
                        dataPos += audioBytes;
                        s_radio.bytesUntilMeta -= audioBytes;
                    } else {
                        // Read metadata length byte
                        int metaLen = data[dataPos++] * 16;
                        if (metaLen > 0 && dataPos + metaLen <= dataLen) {
                            parse_icy_metadata((char*)(data + dataPos), metaLen);
                            dataPos += metaLen;
                        }
                        s_radio.bytesUntilMeta = s_radio.icyMetaInt;
                    }
                }
            } else {
                // No ICY metadata, write directly
                s_radio.ringBuffer.write((uint8_t*)evt->data, evt->data_len);
            }

            // Update buffer percentage
            if (xSemaphoreTake(s_radio.mutex, pdMS_TO_TICKS(10))) {
                if (s_radio.hal) {
                    s_radio.hal->radioMetadata.bufferPercent = s_radio.ringBuffer.bufferPercent();
                }
                xSemaphoreGive(s_radio.mutex);
            }
            break;

        default:
            break;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                           HTTP Streaming Task                              */
/* -------------------------------------------------------------------------- */
static void http_stream_task(void* param)
{
    mclog::tagInfo(TAG, "HTTP stream task started for: {}", s_radio.currentUrl);

    esp_http_client_config_t config = {};
    config.url                      = s_radio.currentUrl.c_str();
    config.event_handler            = http_event_handler;
    config.buffer_size              = 4096;
    config.timeout_ms               = 30000;
    config.keep_alive_enable        = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        mclog::tagError(TAG, "Failed to init HTTP client");
        if (xSemaphoreTake(s_radio.mutex, portMAX_DELAY)) {
            s_radio.state = hal::HalBase::RADIO_ERROR;
            xSemaphoreGive(s_radio.mutex);
        }
        vTaskDelete(nullptr);
        return;
    }

    // Set ICY metadata request header
    esp_http_client_set_header(client, "Icy-MetaData", "1");
    esp_http_client_set_header(client, "User-Agent", "Tab5-WebRadio/1.0");

    // Perform HTTP request
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK && !s_radio.stopRequested) {
        mclog::tagError(TAG, "HTTP stream error: {}", esp_err_to_name(err));
        if (xSemaphoreTake(s_radio.mutex, portMAX_DELAY)) {
            s_radio.state = hal::HalBase::RADIO_ERROR;
            xSemaphoreGive(s_radio.mutex);
        }
    }

    esp_http_client_cleanup(client);
    mclog::tagInfo(TAG, "HTTP stream task ended");

    s_radio.httpTask = nullptr;
    vTaskDelete(nullptr);
}

/* -------------------------------------------------------------------------- */
/*                           Audio Decode Task                                */
/* -------------------------------------------------------------------------- */
static uint8_t _current_volume = 60;

static esp_err_t audio_mute_function(AUDIO_PLAYER_MUTE_SETTING setting)
{
    bsp_codec_config_t* codec_handle = bsp_get_codec_handle();
    codec_handle->set_mute(setting == AUDIO_PLAYER_MUTE ? true : false);
    return ESP_OK;
}

// Custom FILE-like read from ring buffer
static RingBuffer* s_audio_ring_buffer = nullptr;

static int ringbuffer_read(void* cookie, char* buf, int size)
{
    if (!s_audio_ring_buffer || s_radio.stopRequested) {
        return -1;
    }

    // Wait for data with timeout
    int totalRead  = 0;
    int retries    = 0;
    int maxRetries = 100;  // 1 second timeout

    while (totalRead < size && retries < maxRetries && !s_radio.stopRequested) {
        size_t available = s_audio_ring_buffer->available();
        if (available > 0) {
            size_t toRead = (size - totalRead < available) ? (size - totalRead) : available;
            size_t read   = s_audio_ring_buffer->read((uint8_t*)buf + totalRead, toRead);
            totalRead += read;
            retries = 0;  // Reset retries on successful read
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
            retries++;
        }
    }

    if (totalRead == 0 && s_radio.stopRequested) {
        return -1;  // Signal EOF
    }

    // Update spectrum data from audio
    if (totalRead > 0) {
        // Simple amplitude-based spectrum visualization
        int16_t* samples   = (int16_t*)buf;
        int numSamples     = totalRead / 2;
        int samplesPerBand = numSamples / SPECTRUM_BANDS;
        if (samplesPerBand < 1) samplesPerBand = 1;

        for (int band = 0; band < SPECTRUM_BANDS && band * samplesPerBand < numSamples; band++) {
            int sum = 0;
            for (int i = 0; i < samplesPerBand && (band * samplesPerBand + i) < numSamples; i++) {
                sum += abs(samples[band * samplesPerBand + i]);
            }
            int avg             = sum / samplesPerBand;
            s_radio.spectrum[band] = (uint8_t)((avg * 255) / 32768);
        }
    }

    return totalRead;
}

static int ringbuffer_close(void* cookie)
{
    return 0;
}

static void audio_decode_task(void* param)
{
    mclog::tagInfo(TAG, "Audio decode task started");

    // Wait for prebuffer
    mclog::tagInfo(TAG, "Prebuffering...");
    while (s_radio.ringBuffer.available() < PREBUFFER_SIZE && !s_radio.stopRequested) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (s_radio.stopRequested) {
        mclog::tagInfo(TAG, "Audio decode task stopped during prebuffer");
        s_radio.audioTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // Update state to playing
    if (xSemaphoreTake(s_radio.mutex, portMAX_DELAY)) {
        s_radio.state = hal::HalBase::RADIO_PLAYING;
        xSemaphoreGive(s_radio.mutex);
    }
    mclog::tagInfo(TAG, "Prebuffer complete, starting playback");

    // Initialize audio player
    bsp_codec_config_t* codec_handle = bsp_get_codec_handle();
    codec_handle->set_volume(_current_volume);
    codec_handle->i2s_reconfig_clk_fn(44100, 16, I2S_SLOT_MODE_STEREO);

    audio_player_config_t config = {
        .mute_fn    = audio_mute_function,
        .clk_set_fn = codec_handle->i2s_reconfig_clk_fn,
        .write_fn   = codec_handle->i2s_write,
        .priority   = 8,
        .coreID     = 1,
    };
    esp_err_t ret = audio_player_new(config);
    if (ret != ESP_OK) {
        mclog::tagError(TAG, "Failed to create audio player: {}", esp_err_to_name(ret));
        s_radio.audioTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // Create custom FILE from ring buffer using funopen
    s_audio_ring_buffer = &s_radio.ringBuffer;
    cookie_io_functions_t io_funcs = {
        .read  = ringbuffer_read,
        .write = nullptr,
        .seek  = nullptr,
        .close = ringbuffer_close,
    };
    FILE* stream_fp = fopencookie(nullptr, "r", io_funcs);
    if (!stream_fp) {
        mclog::tagError(TAG, "Failed to create stream FILE");
        audio_player_delete();
        s_radio.audioTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // Start playback
    ret = audio_player_play(stream_fp);
    if (ret != ESP_OK) {
        mclog::tagError(TAG, "Failed to start playback: {}", esp_err_to_name(ret));
        fclose(stream_fp);
        audio_player_delete();
        s_radio.audioTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // Wait for stop signal
    while (!s_radio.stopRequested) {
        audio_player_state_t state = audio_player_get_state();
        if (state == AUDIO_PLAYER_STATE_IDLE) {
            mclog::tagInfo(TAG, "Audio player became idle");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Cleanup
    audio_player_delete();
    fclose(stream_fp);
    s_audio_ring_buffer = nullptr;

    mclog::tagInfo(TAG, "Audio decode task ended");
    s_radio.audioTask = nullptr;
    vTaskDelete(nullptr);
}

/* -------------------------------------------------------------------------- */
/*                           HAL Implementation                               */
/* -------------------------------------------------------------------------- */
hal::HalBase::RadioState_t HalEsp32::getRadioState()
{
    if (xSemaphoreTake(s_radio.mutex, pdMS_TO_TICKS(100))) {
        RadioState_t state = s_radio.state;
        xSemaphoreGive(s_radio.mutex);
        return state;
    }
    return _radio_state;
}

bool HalEsp32::startRadioStream(const std::string& url)
{
    mclog::tagInfo(TAG, "Starting radio stream: {}", url);

    // Stop any existing stream
    stopRadioStream();

    // Initialize mutex if needed
    if (!s_radio.mutex) {
        s_radio.mutex = xSemaphoreCreateMutex();
        if (!s_radio.mutex) {
            mclog::tagError(TAG, "Failed to create radio mutex");
            return false;
        }
    }

    // Initialize ring buffer
    if (!s_radio.ringBuffer.buffer) {
        if (!s_radio.ringBuffer.init(RING_BUFFER_SIZE)) {
            mclog::tagError(TAG, "Failed to init ring buffer");
            return false;
        }
    }
    s_radio.ringBuffer.reset();

    // Set state
    if (xSemaphoreTake(s_radio.mutex, portMAX_DELAY)) {
        s_radio.currentUrl     = url;
        s_radio.state          = RADIO_BUFFERING;
        s_radio.stopRequested  = false;
        s_radio.streamTitle    = "";
        s_radio.icyMetaInt     = 0;
        s_radio.bytesUntilMeta = 0;
        s_radio.hal            = this;
        radioMetadata.title    = "";
        radioMetadata.station  = "";
        radioMetadata.bufferPercent = 0;
        memset(s_radio.spectrum, 0, sizeof(s_radio.spectrum));
        xSemaphoreGive(s_radio.mutex);
    }

    _radio_state = RADIO_BUFFERING;

    // Start HTTP streaming task
    BaseType_t ret =
        xTaskCreate(http_stream_task, "http_stream", 8192, nullptr, 5, &s_radio.httpTask);
    if (ret != pdPASS) {
        mclog::tagError(TAG, "Failed to create HTTP stream task");
        _radio_state = RADIO_ERROR;
        return false;
    }

    // Start audio decode task
    ret = xTaskCreate(audio_decode_task, "audio_decode", 8192, nullptr, 6, &s_radio.audioTask);
    if (ret != pdPASS) {
        mclog::tagError(TAG, "Failed to create audio decode task");
        s_radio.stopRequested = true;
        _radio_state          = RADIO_ERROR;
        return false;
    }

    return true;
}

void HalEsp32::stopRadioStream()
{
    mclog::tagInfo(TAG, "Stopping radio stream");

    s_radio.stopRequested = true;

    // Wait for tasks to end
    int timeout = 50;  // 5 seconds
    while ((s_radio.httpTask || s_radio.audioTask) && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout--;
    }

    // Force delete if still running
    if (s_radio.httpTask) {
        vTaskDelete(s_radio.httpTask);
        s_radio.httpTask = nullptr;
    }
    if (s_radio.audioTask) {
        vTaskDelete(s_radio.audioTask);
        s_radio.audioTask = nullptr;
    }

    // Update state
    if (s_radio.mutex && xSemaphoreTake(s_radio.mutex, portMAX_DELAY)) {
        s_radio.state = RADIO_STOPPED;
        xSemaphoreGive(s_radio.mutex);
    }
    _radio_state = RADIO_STOPPED;

    radioMetadata.title         = "";
    radioMetadata.bufferPercent = 0;
}

void HalEsp32::getRadioSpectrum(uint8_t* spectrum, size_t len)
{
    size_t copyLen = (len < SPECTRUM_BANDS) ? len : SPECTRUM_BANDS;
    memcpy(spectrum, s_radio.spectrum, copyLen);
}
