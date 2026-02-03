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
// 128kbps MP3 = 16KB/sec, so:
// - 128KB buffer = ~8 seconds of audio
// - 32KB prebuffer = ~2 seconds before playback starts
#define RING_BUFFER_SIZE     (128 * 1024)  // 128KB ring buffer (in PSRAM)
#define PREBUFFER_SIZE       (32 * 1024)   // 32KB prebuffer before playback
#define MIN_BUFFER_LEVEL     (4 * 1024)    // 4KB minimum before rebuffering
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

    // Check if URL is HTTPS or HTTP
    bool is_https = s_radio.currentUrl.find("https://") == 0;

    esp_http_client_config_t config = {};
    config.url                      = s_radio.currentUrl.c_str();
    config.event_handler            = http_event_handler;
    config.buffer_size              = 4096;
    config.timeout_ms               = 30000;
    config.keep_alive_enable        = true;

    if (is_https) {
        // For HTTPS: skip certificate verification for radio streams
        // (RTC may not have correct time, and streams are public anyway)
        config.skip_cert_common_name_check = true;
        config.use_global_ca_store         = false;
        config.crt_bundle_attach           = NULL;
        // Set cert_pem to empty to skip server cert verification
        config.cert_pem                    = NULL;
    }

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

// Track position for fake seek support
static size_t s_stream_position = 0;

static ssize_t ringbuffer_read(void* cookie, char* buf, size_t size)
{
    if (!s_audio_ring_buffer || s_radio.stopRequested) {
        return -1;
    }

    // Block until we have at least some data - this is critical because
    // returning 0 from read() signals EOF to the audio player!
    int totalRead  = 0;
    int retries    = 0;
    int maxRetries = 500;  // 5 seconds timeout - long enough for rebuffering

    // Wait for at least 1 byte of data before returning
    while (totalRead == 0 && retries < maxRetries && !s_radio.stopRequested) {
        size_t available = s_audio_ring_buffer->available();

        if (available > 0) {
            // Read as much as available, up to requested size
            size_t toRead = (size < available) ? size : available;
            totalRead = s_audio_ring_buffer->read((uint8_t*)buf, toRead);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
            retries++;
        }
    }

    // If we couldn't get any data and stop was requested, signal EOF
    if (totalRead == 0) {
        if (s_radio.stopRequested) {
            mclog::tagInfo(TAG, "ringbuffer_read: stop requested, returning -1");
            return -1;  // Signal EOF
        }
        // Timeout waiting for data - this shouldn't happen often
        // Return -1 to signal error rather than 0 (which means EOF)
        mclog::tagWarn(TAG, "ringbuffer_read: timeout waiting for data");
        return -1;
    }

    s_stream_position += totalRead;

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

// Fake seek - for streams we can only support SEEK_SET to 0 (reset)
// The MP3 decoder calls fseek(fp, 0, SEEK_SET) to reset position
// We can't actually seek in a live stream, but we can pretend for the initial check
static int ringbuffer_seek(void* cookie, off_t* offset, int whence)
{
    // For a stream, we can only "seek" to report current position
    // or pretend to seek to start (which we can't actually do)
    if (whence == SEEK_SET && *offset == 0) {
        // Pretend we're at the start - this allows is_mp3() detection to work
        // The actual read will get data from wherever the stream currently is
        s_stream_position = 0;
        return 0;
    } else if (whence == SEEK_CUR && *offset == 0) {
        // Just return current position
        *offset = (off_t)s_stream_position;
        return 0;
    } else if (whence == SEEK_END) {
        // Can't seek to end of a live stream
        return -1;
    }

    // For any other seek, return error
    return -1;
}

static int ringbuffer_close(void* cookie)
{
    s_stream_position = 0;
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

    // Create custom FILE from ring buffer using fopencookie
    // We provide a seek function so is_mp3() detection works
    s_audio_ring_buffer = &s_radio.ringBuffer;
    s_stream_position = 0;
    cookie_io_functions_t io_funcs = {
        .read  = ringbuffer_read,
        .write = nullptr,
        .seek  = ringbuffer_seek,
        .close = ringbuffer_close,
    };
    FILE* stream_fp = fopencookie(nullptr, "rb", io_funcs);
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

    // Wait for stop signal or idle state
    while (!s_radio.stopRequested) {
        audio_player_state_t state = audio_player_get_state();
        if (state == AUDIO_PLAYER_STATE_IDLE) {
            mclog::tagInfo(TAG, "Audio player became idle");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Cleanup - delete audio player first
    audio_player_delete();

    // Note: Don't call fclose on fopencookie FILE - it can cause issues
    // The ringbuffer_close callback handles cleanup
    // fclose(stream_fp);  // Removed - causes mutex assertion failure
    s_audio_ring_buffer = nullptr;

    // Update state
    if (s_radio.mutex && xSemaphoreTake(s_radio.mutex, pdMS_TO_TICKS(100))) {
        s_radio.state = hal::HalBase::RADIO_STOPPED;
        xSemaphoreGive(s_radio.mutex);
    }

    mclog::tagInfo(TAG, "Audio decode task ended");
    s_radio.audioTask = nullptr;
    vTaskDelete(nullptr);
}

/* -------------------------------------------------------------------------- */
/*                           HAL Implementation                               */
/* -------------------------------------------------------------------------- */
hal::HalBase::RadioState_t HalEsp32::getRadioState()
{
    // Check if mutex exists (radio hasn't been initialized yet)
    if (!s_radio.mutex) {
        return RADIO_STOPPED;
    }
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

    // Signal tasks to stop
    s_radio.stopRequested = true;

    // Wait for audio task to end first (it's the consumer)
    int timeout = 50;  // 5 seconds for audio task
    while (s_radio.audioTask && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout--;
    }

    // Wait for HTTP task to end (it should stop after audio task is gone)
    timeout = 50;  // 5 seconds for HTTP task
    while (s_radio.httpTask && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout--;
    }

    // Don't force delete HTTP task - it crashes the lwip stack!
    // If tasks are still running, just log a warning and continue
    // The tasks will clean themselves up eventually
    if (s_radio.httpTask) {
        mclog::tagWarn(TAG, "HTTP task still running, not force deleting (would crash lwip)");
        // Just clear the handle - task will delete itself
        s_radio.httpTask = nullptr;
    }
    if (s_radio.audioTask) {
        mclog::tagWarn(TAG, "Audio task still running, not force deleting");
        s_radio.audioTask = nullptr;
    }

    // Reset audio state
    s_audio_ring_buffer = nullptr;
    s_stream_position = 0;

    // Update state
    if (s_radio.mutex && xSemaphoreTake(s_radio.mutex, portMAX_DELAY)) {
        s_radio.state = RADIO_STOPPED;
        xSemaphoreGive(s_radio.mutex);
    }
    _radio_state = RADIO_STOPPED;

    radioMetadata.title         = "";
    radioMetadata.bufferPercent = 0;

    // Give time for tasks to fully clean up and network stack to settle
    vTaskDelay(pdMS_TO_TICKS(500));
}

void HalEsp32::getRadioSpectrum(uint8_t* spectrum, size_t len)
{
    size_t copyLen = (len < SPECTRUM_BANDS) ? len : SPECTRUM_BANDS;
    memcpy(spectrum, s_radio.spectrum, copyLen);
}
