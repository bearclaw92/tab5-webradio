/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include <mooncake_log.h>
#include <vector>
#include <memory>
#include <string.h>
#include <bsp/m5stack_tab5.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_http_server.h>

static const char* TAG = "wifi";

// AP mode settings (kept for compatibility)
#define WIFI_AP_SSID    "M5Tab5-WebRadio"
#define WIFI_AP_PASS    ""
#define MAX_STA_CONN    4

// WiFi STA event bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     5

// NVS namespace for WiFi config
#define NVS_WIFI_NAMESPACE "wifi_cfg"

// Static variables for WiFi STA
static EventGroupHandle_t s_wifi_event_group = nullptr;
static int s_retry_num                       = 0;
static HalEsp32* s_hal_instance              = nullptr;
static esp_netif_t* s_sta_netif              = nullptr;
static bool s_wifi_started                   = false;

// HTTP 处理函数
esp_err_t hello_get_handler(httpd_req_t* req)
{
    const char* html_response = R"rawliteral(
        <!DOCTYPE html>
        <html>
        <head>
            <title>Hello</title>
            <style>
                body {
                    display: flex;
                    flex-direction: column;
                    justify-content: center;
                    align-items: center;
                    height: 100vh;
                    margin: 0;
                    font-family: sans-serif;
                    background-color: #f0f0f0;
                }
                h1 {
                    font-size: 48px;
                    color: #333;
                    margin: 0;
                }
                p {
                    font-size: 18px;
                    color: #666;
                    margin-top: 10px;
                }
            </style>
        </head>
        <body>
            <h1>Hello World</h1>
            <p>From M5Tab5</p>
        </body>
        </html>
    )rawliteral";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// URI 路由
httpd_uri_t hello_uri = {.uri = "/", .method = HTTP_GET, .handler = hello_get_handler, .user_ctx = nullptr};

// 启动 Web Server
httpd_handle_t start_webserver()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = nullptr;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &hello_uri);
    }
    return server;
}

// 初始化 Wi-Fi AP 模式
void wifi_init_softap()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.ap.ssid), WIFI_SSID, sizeof(wifi_config.ap.ssid));
    std::strncpy(reinterpret_cast<char*>(wifi_config.ap.password), WIFI_PASS, sizeof(wifi_config.ap.password));
    wifi_config.ap.ssid_len       = std::strlen(WIFI_SSID);
    wifi_config.ap.max_connection = MAX_STA_CONN;
    wifi_config.ap.authmode       = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi AP started. SSID:%s password:%s", WIFI_SSID, WIFI_PASS);
}

static void wifi_ap_test_task(void* param)
{
    wifi_init_softap();
    start_webserver();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}

bool HalEsp32::wifi_init()
{
    mclog::tagInfo(TAG, "wifi init");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    xTaskCreate(wifi_ap_test_task, "ap", 4096, nullptr, 5, nullptr);
    return true;
}

void HalEsp32::setExtAntennaEnable(bool enable)
{
    _ext_antenna_enable = enable;
    mclog::tagInfo(TAG, "set ext antenna enable: {}", _ext_antenna_enable);
    bsp_set_ext_antenna_enable(_ext_antenna_enable);
}

bool HalEsp32::getExtAntennaEnable()
{
    return _ext_antenna_enable;
}

void HalEsp32::startWifiAp()
{
    wifi_init();
}

/* -------------------------------------------------------------------------- */
/*                              WiFi STA Mode                                 */
/* -------------------------------------------------------------------------- */

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        mclog::tagInfo(TAG, "WiFi STA started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            mclog::tagInfo(TAG, "Retry connecting to AP, attempt {}", s_retry_num);
        } else {
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            if (s_hal_instance) {
                s_hal_instance->_wifi_state = hal::HalBase::WIFI_FAILED;
            }
            mclog::tagWarn(TAG, "Failed to connect to AP after {} attempts", WIFI_MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));

        mclog::tagInfo(TAG, "Got IP: {}", ip_str);

        if (s_hal_instance) {
            s_hal_instance->_wifi_ip    = ip_str;
            s_hal_instance->_wifi_state = hal::HalBase::WIFI_CONNECTED;
        }

        s_retry_num = 0;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

bool HalEsp32::wifi_sta_init()
{
    if (_wifi_initialized) {
        return true;
    }

    mclog::tagInfo(TAG, "Initializing WiFi STA mode");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        mclog::tagError(TAG, "NVS flash init failed: {}", esp_err_to_name(ret));
        return false;
    }

    // Initialize TCP/IP stack
    ret = esp_netif_init();
    if (ret != ESP_OK) {
        mclog::tagError(TAG, "Netif init failed: {}", esp_err_to_name(ret));
        return false;
    }

    // Create default event loop
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        mclog::tagError(TAG, "Event loop create failed: {}", esp_err_to_name(ret));
        return false;
    }

    // Create default WiFi STA netif
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        mclog::tagError(TAG, "Failed to create WiFi STA netif");
        return false;
    }

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret                    = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        mclog::tagError(TAG, "WiFi init failed: {}", esp_err_to_name(ret));
        return false;
    }

    // Create event group
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        mclog::tagError(TAG, "Failed to create event group");
        return false;
    }

    // Register event handlers
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr,
                                              &instance_any_id);
    if (ret != ESP_OK) {
        mclog::tagError(TAG, "Failed to register WiFi event handler");
        return false;
    }

    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr,
                                              &instance_got_ip);
    if (ret != ESP_OK) {
        mclog::tagError(TAG, "Failed to register IP event handler");
        return false;
    }

    // Set WiFi mode to STA
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        mclog::tagError(TAG, "Failed to set WiFi mode: {}", esp_err_to_name(ret));
        return false;
    }

    _wifi_initialized = true;
    s_hal_instance    = this;
    mclog::tagInfo(TAG, "WiFi STA initialized successfully");
    return true;
}

hal::HalBase::WifiState_t HalEsp32::getWifiState()
{
    return _wifi_state;
}

bool HalEsp32::connectWifiSta(const std::string& ssid, const std::string& password)
{
    if (!wifi_sta_init()) {
        return false;
    }

    mclog::tagInfo(TAG, "Connecting to SSID: {}", ssid);

    _wifi_state = WIFI_CONNECTING;
    _wifi_ssid  = ssid;
    _wifi_ip    = "";
    s_retry_num = 0;

    // Stop WiFi if already running
    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }

    // Configure WiFi
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password.c_str(), sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable    = true;
    wifi_config.sta.pmf_cfg.required   = false;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        mclog::tagError(TAG, "Failed to set WiFi config: {}", esp_err_to_name(ret));
        _wifi_state = WIFI_FAILED;
        return false;
    }

    // Start WiFi
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        mclog::tagError(TAG, "Failed to start WiFi: {}", esp_err_to_name(ret));
        _wifi_state = WIFI_FAILED;
        return false;
    }
    s_wifi_started = true;

    // Wait for connection with timeout (10 seconds)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(10000));

    if (bits & WIFI_CONNECTED_BIT) {
        mclog::tagInfo(TAG, "Connected to {} with IP: {}", ssid, _wifi_ip);
        return true;
    } else if (bits & WIFI_FAIL_BIT) {
        mclog::tagError(TAG, "Failed to connect to {}", ssid);
        _wifi_state = WIFI_FAILED;
        return false;
    } else {
        mclog::tagError(TAG, "Connection timeout for {}", ssid);
        _wifi_state = WIFI_FAILED;
        return false;
    }
}

void HalEsp32::disconnectWifi()
{
    if (s_wifi_started) {
        mclog::tagInfo(TAG, "Disconnecting WiFi");
        esp_wifi_disconnect();
        _wifi_state = WIFI_DISCONNECTED;
        _wifi_ip    = "";
    }
}

std::string HalEsp32::getWifiIp()
{
    return _wifi_ip;
}

std::string HalEsp32::getWifiSsid()
{
    return _wifi_ssid;
}

void HalEsp32::saveWifiConfig(const std::string& ssid, const std::string& password)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        mclog::tagError(TAG, "Failed to open NVS: {}", esp_err_to_name(ret));
        return;
    }

    nvs_set_str(handle, "ssid", ssid.c_str());
    nvs_set_str(handle, "password", password.c_str());
    nvs_commit(handle);
    nvs_close(handle);

    mclog::tagInfo(TAG, "WiFi config saved for SSID: {}", ssid);
}

bool HalEsp32::loadWifiConfig(std::string& ssid, std::string& password)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_WIFI_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        mclog::tagInfo(TAG, "No saved WiFi config found");
        return false;
    }

    char ssid_buf[64]     = {0};
    char password_buf[64] = {0};
    size_t ssid_len       = sizeof(ssid_buf);
    size_t password_len   = sizeof(password_buf);

    ret = nvs_get_str(handle, "ssid", ssid_buf, &ssid_len);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    ret = nvs_get_str(handle, "password", password_buf, &password_len);
    if (ret != ESP_OK) {
        // Password might be empty for open networks
        password_buf[0] = '\0';
    }

    nvs_close(handle);

    ssid     = ssid_buf;
    password = password_buf;

    mclog::tagInfo(TAG, "Loaded WiFi config for SSID: {}", ssid);
    return !ssid.empty();
}
