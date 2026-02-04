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
// - 256KB buffer = ~16 seconds of audio
// - 64KB prebuffer = ~4 seconds before playback starts
// Larger buffers help with network jitter and WiFi instability
#define RING_BUFFER_SIZE     (256 * 1024)  // 256KB ring buffer (in PSRAM)
#define PREBUFFER_SIZE       (64 * 1024)   // 64KB prebuffer before playback
#define MIN_BUFFER_LEVEL     (8 * 1024)    // 8KB minimum before rebuffering
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
    uint32_t streamId        = 0;  // Incremented each stream, used to ignore old HTTP tasks
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
// Stream ID for current HTTP task - used to ignore data from stale tasks
static uint32_t s_http_task_stream_id = 0;

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

            // Check if this HTTP task is still the current one
            // If a new stream started, ignore data from old tasks
            if (s_http_task_stream_id != s_radio.streamId) {
                return ESP_FAIL;  // Stale task, stop it
            }

            // Handle ICY metadata if present
            if (s_radio.icyMetaInt > 0) {
                uint8_t* data     = (uint8_t*)evt->data;
                int dataLen       = evt->data_len;
                int dataPos       = 0;

                while (dataPos < dataLen && s_http_task_stream_id == s_radio.streamId) {
                    if (s_radio.bytesUntilMeta > 0) {
                        // Write audio data
                        int audioBytes = (dataLen - dataPos < s_radio.bytesUntilMeta)
                                             ? (dataLen - dataPos)
                                             : s_radio.bytesUntilMeta;
                        size_t written = s_radio.ringBuffer.write(data + dataPos, audioBytes);
                        if (written < (size_t)audioBytes) {
                            // Buffer full - this shouldn't happen with our large buffer
                            static uint32_t lastFullLog = 0;
                            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
                            if ((now - lastFullLog) > 1000) {
                                mclog::tagWarn(TAG, "Ring buffer full! Dropping {} bytes", audioBytes - written);
                                lastFullLog = now;
                            }
                        }
                        dataPos += audioBytes;
                        s_radio.bytesUntilMeta -= audioBytes;
                    } else {
                        // Read metadata length byte
                        int metaLen = data[dataPos++] * 16;
                        if (metaLen > 0) {
                            if (dataPos + metaLen <= dataLen) {
                                // Full metadata in this chunk
                                parse_icy_metadata((char*)(data + dataPos), metaLen);
                                dataPos += metaLen;
                            } else {
                                // Metadata spans chunks - skip it (rare edge case)
                                // Just skip the bytes we have and reset counter
                                int skipBytes = dataLen - dataPos;
                                dataPos = dataLen;
                                mclog::tagWarn(TAG, "ICY metadata spans chunks, skipping ({} bytes)", metaLen);
                            }
                        }
                        s_radio.bytesUntilMeta = s_radio.icyMetaInt;
                    }
                }
            } else {
                // No ICY metadata, write directly
                size_t written = s_radio.ringBuffer.write((uint8_t*)evt->data, evt->data_len);
                if (written < (size_t)evt->data_len) {
                    static uint32_t lastFullLog = 0;
                    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
                    if ((now - lastFullLog) > 1000) {
                        mclog::tagWarn(TAG, "Ring buffer full! Dropping {} bytes", evt->data_len - written);
                        lastFullLog = now;
                    }
                }
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
    // Capture the stream ID for this task - used to ignore data from stale tasks
    uint32_t myStreamId = s_radio.streamId;
    s_http_task_stream_id = myStreamId;
    mclog::tagInfo(TAG, "HTTP stream task started for: {} (stream #{})", s_radio.currentUrl, myStreamId);

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

    // Perform HTTP request - this blocks while streaming
    mclog::tagInfo(TAG, "HTTP client performing request...");
    esp_err_t err = esp_http_client_perform(client);

    // Log why the HTTP request ended
    if (err != ESP_OK) {
        if (s_radio.stopRequested) {
            mclog::tagInfo(TAG, "HTTP stream stopped by user request");
        } else {
            mclog::tagError(TAG, "HTTP stream error: {} ({})", esp_err_to_name(err), (int)err);
            if (xSemaphoreTake(s_radio.mutex, portMAX_DELAY)) {
                s_radio.state = hal::HalBase::RADIO_ERROR;
                xSemaphoreGive(s_radio.mutex);
            }
        }
    } else {
        mclog::tagInfo(TAG, "HTTP stream completed normally (unexpected for live stream!)");
    }

    esp_http_client_cleanup(client);
    mclog::tagInfo(TAG, "HTTP stream task ended (stream #{})", myStreamId);

    s_radio.httpTask = nullptr;
    vTaskDelete(nullptr);
}

/* -------------------------------------------------------------------------- */
/*                           Audio Decode Task                                */
/* -------------------------------------------------------------------------- */

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

// Save first bytes for rewind support (needed for is_mp3() detection)
// ID3v2 header is 10 bytes, but we need more to handle the ID3 tag size
// which can be up to several KB. We'll save enough for typical ID3 detection.
#define HEADER_BUFFER_SIZE 4096
static uint8_t* s_header_buffer = nullptr;  // Dynamically allocated in PSRAM
static size_t s_header_bytes_saved = 0;
static size_t s_header_read_pos = 0;
static bool s_header_replay_mode = false;

// Flag to log first header read - reset when starting new stream
static bool s_first_header_read_logged = false;

static ssize_t ringbuffer_read(void* cookie, char* buf, size_t size)
{
    if (!s_audio_ring_buffer || s_radio.stopRequested) {
        return -1;
    }

    int totalRead = 0;

    // If in header replay mode, serve from saved header first
    if (s_header_buffer && s_header_replay_mode && s_header_read_pos < s_header_bytes_saved) {
        size_t headerAvail = s_header_bytes_saved - s_header_read_pos;
        size_t toRead = (size < headerAvail) ? size : headerAvail;
        memcpy(buf, s_header_buffer + s_header_read_pos, toRead);
        s_header_read_pos += toRead;
        totalRead = toRead;
        s_stream_position += toRead;

        // Log first read from header buffer (once per stream)
        if (!s_first_header_read_logged) {
            mclog::tagInfo(TAG, "First header read: {} bytes, first 4: {:02X} {:02X} {:02X} {:02X}",
                          toRead, (uint8_t)buf[0], (uint8_t)buf[1], (uint8_t)buf[2], (uint8_t)buf[3]);
            s_first_header_read_logged = true;
        }

        // If we've replayed all header bytes, exit replay mode
        if (s_header_read_pos >= s_header_bytes_saved) {
            mclog::tagInfo(TAG, "Header replay complete, switching to ring buffer");
            s_header_replay_mode = false;
        }

        // If we've satisfied the request, return
        if ((size_t)totalRead >= size) {
            return totalRead;
        }
        // Otherwise continue reading from ring buffer
        buf += totalRead;
        size -= totalRead;
    }

    // Block until we have at least some data - this is critical because
    // returning 0 from read() signals EOF to the audio player!
    // For live streams, we need to be very patient - network can have long stalls
    int retries    = 0;
    int maxRetries = 3000;  // 30 seconds timeout - very patient for live streams
    static uint32_t lastBufferLog = 0;
    static uint32_t lastHealthyLog = 0;

    // Wait for at least 1 byte of data before returning
    while (retries < maxRetries && !s_radio.stopRequested) {
        size_t available = s_audio_ring_buffer->available();

        if (available > 0) {
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

            // Log buffer level periodically when low
            if (available < MIN_BUFFER_LEVEL && (now - lastBufferLog) > 1000) {
                mclog::tagWarn(TAG, "Buffer low: {} bytes ({} KB)", available, available / 1024);
                lastBufferLog = now;
            }

            // Log healthy buffer periodically (every 10 seconds)
            if (available >= MIN_BUFFER_LEVEL && (now - lastHealthyLog) > 10000) {
                mclog::tagInfo(TAG, "Buffer healthy: {} KB", available / 1024);
                lastHealthyLog = now;
            }

            // Read as much as available, up to requested size
            size_t toRead = (size < available) ? size : available;
            size_t bytesRead = s_audio_ring_buffer->read((uint8_t*)buf, toRead);

            totalRead += bytesRead;
            s_stream_position += bytesRead;
            break;  // Got data, exit wait loop
        } else {
            // Buffer empty! Log this (but not too often)
            if (retries == 0) {
                mclog::tagWarn(TAG, "Buffer empty! Waiting for data...");
            } else if (retries % 100 == 0) {
                // Log every second while waiting
                mclog::tagWarn(TAG, "Still waiting for data... ({} seconds)", retries / 100);
            }
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
        // Timeout waiting for data - after 30 seconds, give up
        mclog::tagError(TAG, "ringbuffer_read: timeout after 30s waiting for data");
        return -1;
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

// Fake seek - for streams we can only support SEEK_SET to 0 (reset)
// The MP3 decoder calls fseek(fp, 0, SEEK_SET) to reset position
// We support seeking back to 0 by replaying saved header bytes
static int ringbuffer_seek(void* cookie, off_t* offset, int whence)
{
    if (whence == SEEK_SET && *offset == 0) {
        // Seek to start - enable header replay mode if we have saved bytes
        if (s_header_buffer && s_header_bytes_saved > 0) {
            s_header_replay_mode = true;
            s_header_read_pos = 0;
            s_stream_position = 0;
            mclog::tagInfo(TAG, "Seek to 0: enabling header replay mode ({} bytes saved)", s_header_bytes_saved);
            return 0;
        }
        // No header buffer, just reset position counter
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
    s_header_bytes_saved = 0;
    s_header_read_pos = 0;
    s_header_replay_mode = false;
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

    // Allocate header buffer for seek support FIRST (before audio player starts reading)
    if (!s_header_buffer) {
        s_header_buffer = (uint8_t*)heap_caps_malloc(HEADER_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_header_buffer) {
            s_header_buffer = (uint8_t*)malloc(HEADER_BUFFER_SIZE);
        }
    }
    if (!s_header_buffer) {
        mclog::tagError(TAG, "Failed to allocate header buffer");
        s_radio.audioTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // Scan the prebuffer to find the first valid MP3 frame header
    // When joining a live stream, we may be in the middle of a frame
    // MP3 frame header: 0xFF followed by specific bit patterns
    // See: http://www.mp3-tech.org/programmer/frame_header.html
    {
        // Read data from ring buffer to scan for sync
        size_t available = s_radio.ringBuffer.available();
        size_t scanSize = (available < HEADER_BUFFER_SIZE) ? available : HEADER_BUFFER_SIZE;
        size_t bytesRead = s_radio.ringBuffer.read(s_header_buffer, scanSize);

        // Find valid MP3 frame header (need at least 4 bytes for full header)
        int syncOffset = -1;
        for (size_t i = 0; i < bytesRead - 3; i++) {
            uint8_t b0 = s_header_buffer[i];
            uint8_t b1 = s_header_buffer[i + 1];
            uint8_t b2 = s_header_buffer[i + 2];

            // Check for sync word: 11 bits set (0xFF followed by 0xE0 or higher)
            if (b0 != 0xFF || (b1 & 0xE0) != 0xE0) {
                continue;
            }

            // Extract and validate header fields
            // Bits in b1: AAAB BCCD
            // A = sync (111), B = version (00=2.5, 01=reserved, 10=2, 11=1)
            // C = layer (00=reserved, 01=III, 10=II, 11=I), D = protection
            int version = (b1 >> 3) & 0x03;
            int layer = (b1 >> 1) & 0x03;

            // Skip reserved version (01) and reserved layer (00)
            if (version == 1 || layer == 0) {
                continue;
            }

            // Bits in b2: EEEE FFGH
            // E = bitrate index (0000 and 1111 are invalid)
            // F = sample rate index (11 is reserved)
            int bitrateIdx = (b2 >> 4) & 0x0F;
            int sampleRateIdx = (b2 >> 2) & 0x03;

            // Skip invalid bitrate (0 = free, 15 = bad) and reserved sample rate
            if (bitrateIdx == 0 || bitrateIdx == 15 || sampleRateIdx == 3) {
                continue;
            }

            // This looks like a valid MP3 frame header!
            syncOffset = i;
            mclog::tagInfo(TAG, "Found valid MP3 header at offset {}: {:02X} {:02X} {:02X} {:02X} "
                          "(v={} l={} br={} sr={})",
                          i, b0, b1, b2, s_header_buffer[i + 3],
                          version, layer, bitrateIdx, sampleRateIdx);
            break;
        }

        if (syncOffset > 0) {
            // Move data so sync word is at the beginning
            mclog::tagInfo(TAG, "Shifting buffer to start at MP3 frame");
            s_header_bytes_saved = bytesRead - syncOffset;
            memmove(s_header_buffer, s_header_buffer + syncOffset, s_header_bytes_saved);
        } else if (syncOffset == 0) {
            // Sync word already at start
            mclog::tagInfo(TAG, "MP3 frame already at start of buffer");
            s_header_bytes_saved = bytesRead;
        } else {
            // No valid frame found - log first bytes for debugging
            mclog::tagWarn(TAG, "No valid MP3 frame in first {} bytes, first 8 bytes: "
                          "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                          bytesRead,
                          s_header_buffer[0], s_header_buffer[1], s_header_buffer[2], s_header_buffer[3],
                          s_header_buffer[4], s_header_buffer[5], s_header_buffer[6], s_header_buffer[7]);
            s_header_bytes_saved = bytesRead;
        }
    }

    // Initialize the fopencookie state BEFORE creating audio player
    // Start in replay mode so is_mp3() reads from header buffer
    s_audio_ring_buffer = &s_radio.ringBuffer;
    s_stream_position = 0;
    s_header_read_pos = 0;
    s_header_replay_mode = true;  // Start in replay mode!
    s_first_header_read_logged = false;  // Reset logging flag for new stream

    // Initialize audio player - volume is controlled by HAL setSpeakerVolume()
    bsp_codec_config_t* codec_handle = bsp_get_codec_handle();

    // Pre-configure I2S to 44100Hz (standard for MP3 radio streams) to avoid
    // "Mode conflict" error when audio_player tries to reconfigure while running
    // This sets up the codec before playback starts
    mclog::tagInfo(TAG, "Pre-configuring I2S for 44100Hz stereo");
    codec_handle->i2s_reconfig_clk_fn(44100, 16, I2S_SLOT_MODE_STEREO);
    // The MP3 decoder will call clk_set_fn with the correct sample rate from the stream

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
    mclog::tagInfo(TAG, "Calling audio_player_play with {} bytes in header buffer", s_header_bytes_saved);
    ret = audio_player_play(stream_fp);
    if (ret != ESP_OK) {
        mclog::tagError(TAG, "Failed to start playback: {}", esp_err_to_name(ret));
        fclose(stream_fp);
        audio_player_delete();
        s_radio.audioTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // Give the audio player's internal task time to start and begin decoding
    vTaskDelay(pdMS_TO_TICKS(500));

    // Check if playback actually started
    audio_player_state_t initial_state = audio_player_get_state();
    mclog::tagInfo(TAG, "After 500ms delay, player state: {}", (int)initial_state);

    // Wait for stop signal or idle state
    // Also monitor buffer health and HTTP task status
    uint32_t lastStatusLog = 0;
    while (!s_radio.stopRequested) {
        audio_player_state_t state = audio_player_get_state();
        if (state == AUDIO_PLAYER_STATE_IDLE) {
            mclog::tagInfo(TAG, "Audio player became idle");
            break;
        }

        // Log status every 5 seconds
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if ((now - lastStatusLog) > 5000) {
            size_t bufferBytes = s_radio.ringBuffer.available();
            int bufferPct = s_radio.ringBuffer.bufferPercent();
            bool httpRunning = (s_radio.httpTask != nullptr);
            mclog::tagInfo(TAG, "Status: buffer={}KB ({}%), HTTP task={}, player state={}",
                          bufferBytes / 1024, bufferPct, httpRunning ? "running" : "stopped", (int)state);
            lastStatusLog = now;
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

    // Set state - increment stream ID to invalidate old HTTP tasks
    if (xSemaphoreTake(s_radio.mutex, portMAX_DELAY)) {
        s_radio.streamId++;  // New stream, old HTTP tasks will be ignored
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
        mclog::tagInfo(TAG, "Starting stream #{}", s_radio.streamId);
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
