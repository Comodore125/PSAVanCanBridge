#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include <cstdio>
#include <cstring>
#include "../ConfigFile.hpp"
#include "../TimeProvider.hpp"
#include "../CarState.hpp"
#include "../../Protocol/AEE2004/Structs/CanDisplayStructs.h"
#include "gzipped_webpage_data.h"

typedef esp_err_t (*my_httpd_handler_t)(httpd_req_t *req);
const int WIFI_INITIAL_TIMEOUT = 120;
const int WIFI_AFTER_CONNECT_TIMEOUT = 7;

class WebServer {
public:
WebServer(
    CarState* carState,
    ConfigFile* configFile,
    TimeProvider* timeProvider,
    ImmediateSignalCallback immediateSignalCallback
) : server(NULL) {
    _carState = carState;
    _configFile = configFile;
    _timeProvider = timeProvider;
    _immediateSignalCallback = immediateSignalCallback;
}

    // Initialize NVS and Wi-Fi
    void StartWebServer() {
        // Initialize NVS
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);

        // Initialize the Wi-Fi driver
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());

        esp_netif_t *netif = esp_netif_create_default_wifi_ap();

        //Set the IP address of the AP
        ESP_ERROR_CHECK(esp_netif_dhcps_stop(netif));
        // Configure the static IP settings
        esp_netif_ip_info_t ip_info;
        ip_info.ip.addr = esp_ip4addr_aton("192.168.100.1");
        ip_info.netmask.addr = esp_ip4addr_aton("255.255.255.0");
        ip_info.gw.addr = esp_ip4addr_aton("192.168.100.1");

        // Set the IP information to the network interface
        ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip_info));

        // Restart the DHCP server (if required)
        ESP_ERROR_CHECK(esp_netif_dhcps_start(netif));

        // Initialize Wi-Fi
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.nvs_enable = true;
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        // Set Wi-Fi to station mode
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

        // Configure the Wi-Fi connection
        wifi_config_t wifi_config = {};
        wifi_config = {
            .ap = {
                .ssid = "PSA VAN-CAN Bridge",
                .password = "123456789",
                .ssid_len = strlen("PSA VAN-CAN Bridge"),
                .channel = 1,
                .authmode = WIFI_AUTH_WPA_PSK,
                .max_connection = 4,
            }
        };

        // Set Wi-Fi configuration and start Wi-Fi
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(8));
        startWebServer();
    }

    void RegisterHandler(const char* uri, httpd_method_t method, my_httpd_handler_t handler)
    {
        httpd_uri_t uri_t = {
            .uri = uri,
            .method = method,
            .handler = handler,
            .user_ctx = this
        };
        httpd_register_uri_handler(server, &uri_t);
    }

    void RegisterEndpoints()
    {
        RegisterHandler("/", HTTP_GET, &WebServer::get_index_handler);
        RegisterHandler("/index.html", HTTP_GET, &WebServer::get_index_handler);
        RegisterHandler("/api/time", HTTP_GET, &WebServer::get_time_handler);
        RegisterHandler("/api/reboot", HTTP_GET, &WebServer::get_reboot_handler);
        RegisterHandler("/api/getVin", HTTP_GET, &WebServer::get_vin_handler);
        RegisterHandler("/api/config.json", HTTP_GET, &WebServer::get_config_handler);
        RegisterHandler("/api/config", HTTP_POST, &WebServer::post_config_handler);
        RegisterHandler("/api/time", HTTP_POST, &WebServer::post_time_handler);
        RegisterHandler("/api/debug/doorPopup/start", HTTP_POST, &WebServer::post_debug_door_popup_start_handler);
        RegisterHandler("/api/debug/doorPopup/stop", HTTP_POST, &WebServer::post_debug_door_popup_stop_handler);
        RegisterHandler("/api/debug/doorPopup/status", HTTP_GET, &WebServer::get_debug_door_popup_status_handler);
    }

    void UnRegisterEndpoints()
    {
        // Unregister all endpoints
        httpd_unregister_uri_handler(server, "/", HTTP_GET);
        httpd_unregister_uri_handler(server, "/index.html", HTTP_GET);
        httpd_unregister_uri_handler(server, "/api/time", HTTP_GET);
        httpd_unregister_uri_handler(server, "/api/reboot", HTTP_GET);
        httpd_unregister_uri_handler(server, "/api/getVin", HTTP_GET);
        httpd_unregister_uri_handler(server, "/api/config.json", HTTP_GET);
        httpd_unregister_uri_handler(server, "/api/config", HTTP_POST);
        httpd_unregister_uri_handler(server, "/api/time", HTTP_POST);
        httpd_unregister_uri_handler(server, "/api/debug/doorPopup/start", HTTP_POST);
        httpd_unregister_uri_handler(server, "/api/debug/doorPopup/stop", HTTP_POST);
        httpd_unregister_uri_handler(server, "/api/debug/doorPopup/status", HTTP_GET);
    }

    // Start web server
    esp_err_t startWebServer() {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.uri_match_fn = httpd_uri_match_wildcard;
        config.stack_size = 8192;
        if (httpd_start(&server, &config) == ESP_OK)
        {
            RegisterEndpoints();

            ESP_LOGI(TAG, "Web server started");
            printf("Web server started\n");
            _isRunning = true;
            _lastRequestTime = _carState->CurrenTime;
            _inactivityTimeout = WIFI_INITIAL_TIMEOUT;
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Failed to start the web server");
        return ESP_FAIL;
    }

    // Stop web server
    void stopWebServer() {
        if (server) {
            UnRegisterEndpoints();
            httpd_stop(server);
            server = nullptr;
            ESP_LOGI(TAG, "Web server stopped");
        }
    }

    // Stop Wi-Fi
    void stopWifi() {
        ESP_ERROR_CHECK(esp_wifi_stop());
        ESP_ERROR_CHECK(esp_wifi_deinit());
        ESP_LOGI(TAG, "Wi-Fi stopped");
    }

    // Handle GET requests
    static esp_err_t hello_get_handler(httpd_req_t *req) {
        printf("Received GET request\n");
        auto *instance = static_cast<WebServer *>(req->user_ctx);
        instance->_lastRequestTime = instance->_carState->CurrenTime;
        const char* resp_str = "Hello from ESP32!";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_sendstr(req, "Time saved");
        //httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    static esp_err_t get_index_handler(httpd_req_t *req)
    {
        // Set the appropriate headers for gzipped content
        auto *instance = static_cast<WebServer *>(req->user_ctx);
        instance->_lastRequestTime = instance->_carState->CurrenTime;
        instance->_inactivityTimeout = WIFI_AFTER_CONNECT_TIMEOUT;

        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

        // Send the data from PROGMEM
        size_t data_size = sizeof(ESP_REACT_DATA_0);
        httpd_resp_send(req, (const char*)ESP_REACT_DATA_0, data_size);
        return ESP_OK;
    }

    static esp_err_t get_time_handler(httpd_req_t *req)
    {
        // Send a 200 OK response with no content
        auto *instance = static_cast<WebServer *>(req->user_ctx);
        instance->_lastRequestTime = instance->_carState->CurrenTime;

        httpd_resp_set_type(req, "application/json");

        cJSON *json = cJSON_CreateObject();
        cJSON_AddNumberToObject(json, "hour", instance->_carState->Hour);
        cJSON_AddNumberToObject(json, "minute", instance->_carState->Minute);
        cJSON_AddNumberToObject(json, "second", instance->_carState->Second);
        cJSON_AddNumberToObject(json, "day", instance->_carState->MDay);
        cJSON_AddNumberToObject(json, "month", instance->_carState->Month);
        cJSON_AddNumberToObject(json, "year", instance->_carState->Year);
        cJSON_AddStringToObject(json, "firmware_version", instance->_carState->Version);
        const char *jsonResponse = cJSON_Print(json);
        cJSON_Delete(json);
        httpd_resp_sendstr(req, jsonResponse);

        return ESP_OK;
    }

    static esp_err_t get_reboot_handler(httpd_req_t *req)
    {
        // Send a 200 OK response with no content
        httpd_resp_send(req, NULL, 0);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        esp_restart();
        return ESP_OK;
    }

    static esp_err_t get_vin_handler(httpd_req_t *req)
    {
        // Send a 200 OK response with no content
        httpd_resp_send(req, NULL, 0);
        auto *instance = static_cast<WebServer *>(req->user_ctx);
        instance->_carState->RADIO_TYPE = req->uri[strlen(req->uri) - 1] - '0';

        printf("Request VIN read, radio type: %d\n", instance->_carState->RADIO_TYPE);

        if (instance->_immediateSignalCallback)
        {
            instance->_immediateSignalCallback(ImmediateSignal::StartVinRead);
        }
        return ESP_OK;
    }

    static esp_err_t get_config_handler(httpd_req_t *req)
    {
        printf("GET /api/config\n");

        auto *instance = static_cast<WebServer *>(req->user_ctx);
        auto _configFile = instance->_configFile;

        if (!_configFile)
        {
            printf("Config file is null\n");
            return ESP_FAIL;
        }
        else
        {
            printf("Config file is not null\n");
        }

        httpd_resp_set_type(req, "application/json");

        printf("Reading config file\n");
        auto jsonHandle = _configFile->GetAsJson();
        printf("Reading config file ok\n");
        if (jsonHandle)
        {
            cJSON *json = jsonHandle.get();
            const char *jsonString = cJSON_Print(json);
            httpd_resp_sendstr(req, jsonString);
            free((void *)jsonString);
        }
        else
        {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read config file");
        }
        return ESP_OK;
    }

    static esp_err_t post_config_handler(httpd_req_t *req)
    {
        printf("POST /api/config\n");
        char *content = (char *)malloc(req->content_len + 1);
        int ret, remaining = req->content_len;
        printf("Content length: %d\n", remaining);

        if (!content) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
            return ESP_FAIL;
        }
        while (remaining > 0) {
            ret = httpd_req_recv(req, content, remaining);
            if (ret <= 0) {
                if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                    httpd_resp_send_408(req);
                }
                free(content);
                return ESP_FAIL;
            }
            remaining -= ret;
        }
        content[req->content_len] = '\0'; // Null-terminate the received data

        //printf("Received config: %s\n", content);
        auto *instance = static_cast<WebServer *>(req->user_ctx);
        instance->_configFile->SaveJson(content);
        instance->_configFile->Read();
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_sendstr(req, "Config saved");

        free(content);
        return ESP_OK;
    }

    static esp_err_t post_time_handler(httpd_req_t *req)
    {
        char content[100];
        int ret, remaining = req->content_len;
        if (remaining > sizeof(content)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too large");
            return ESP_FAIL;
        }
        while (remaining > 0) {
            ret = httpd_req_recv(req, content, sizeof(content));
            if (ret <= 0) {
                if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                    httpd_resp_send_408(req);
                }
                return ESP_FAIL;
            }
            remaining -= ret;
        }
        cJSON *root = cJSON_Parse(content);
        if (!root) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
            return ESP_FAIL;
        }

        cJSON *jsonObj = cJSON_GetObjectItem(root, "year");
        double year = cJSON_GetNumberValue(jsonObj);

        jsonObj = cJSON_GetObjectItem(root, "month");
        double month = cJSON_GetNumberValue(jsonObj);

        jsonObj = cJSON_GetObjectItem(root, "day");
        double day = cJSON_GetNumberValue(jsonObj);

        jsonObj = cJSON_GetObjectItem(root, "hour");
        double hour = cJSON_GetNumberValue(jsonObj);

        jsonObj = cJSON_GetObjectItem(root, "minute");
        double minute = cJSON_GetNumberValue(jsonObj);

        printf("year: %d\n", (int)year);
        printf("month: %d\n", (int)month);
        printf("day: %d\n", (int)day);
        printf("hour: %d\n", (int)hour);
        printf("minute: %d\n", (int)minute);

        cJSON_Delete(root);

        auto *instance = static_cast<WebServer *>(req->user_ctx);
        instance->_timeProvider->SetDateTime((int)year, (int)month, (int)day, (int)hour, (int)minute, 0);

        httpd_resp_set_status(req, "200 OK");
        httpd_resp_sendstr(req, "Time saved");
        httpd_resp_set_type(req, "application/json");

        return ESP_OK;
    }

    static esp_err_t post_debug_door_popup_start_handler(httpd_req_t *req)
    {
        auto *instance = static_cast<WebServer *>(req->user_ctx);
        instance->_lastRequestTime = instance->_carState->CurrenTime;
        instance->StartDoorPopupDebug(instance->_carState->CurrenTime);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{"status":"started"}");
        return ESP_OK;
    }

    static esp_err_t post_debug_door_popup_stop_handler(httpd_req_t *req)
    {
        auto *instance = static_cast<WebServer *>(req->user_ctx);
        instance->_lastRequestTime = instance->_carState->CurrenTime;
        instance->StopDoorPopupDebug(instance->_carState->CurrenTime, false);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{"status":"stopped"}");
        return ESP_OK;
    }

    static esp_err_t get_debug_door_popup_status_handler(httpd_req_t *req)
    {
        auto *instance = static_cast<WebServer *>(req->user_ctx);
        instance->_lastRequestTime = instance->_carState->CurrenTime;
        instance->SendDoorPopupDebugStatus(req);
        return ESP_OK;
    }

    void Process()
    {
        ProcessDoorPopupDebug();

        if (_isRunning && server != nullptr)
        {
            if ((_carState->CurrenTime - _lastRequestTime) > _inactivityTimeout * 1000)
            {
                _isRunning = false;
                printf("Stopping web server due to inactivity\n");
                stopWebServer();
                stopWifi();
            }
        }
    }

private:
    struct DoorPopupDebugPreset
    {
        const char* key;
        const char* label;
        uint8_t doorStatus1;
        uint8_t doorStatus2;
    };

    inline static constexpr uint32_t DoorPopupDebugShowDurationMs = 5000;
    inline static constexpr uint32_t DoorPopupDebugHideDurationMs = 3000;
    inline static constexpr uint8_t DoorPopupDebugPresetCount = 10;
    inline static constexpr DoorPopupDebugPreset DoorPopupDebugPresets[DoorPopupDebugPresetCount] = {
        { "front-left", "Front left", 0x40, 0x00 },
        { "rear-left", "Rear left", 0x10, 0x00 },
        { "front-left+rear-left+rear-right", "Front left + rear left + rear right", 0x70, 0x00 },
        { "boot", "Boot", 0x08, 0x00 },
        { "front-left+rear-left", "Front left + rear left", 0x50, 0x00 },
        { "front-left+rear-right", "Front left + rear right", 0x60, 0x00 },
        { "rear-left+rear-right", "Rear left + rear right", 0x30, 0x00 },
        { "front-left+boot", "Front left + boot", 0x48, 0x00 },
        { "rear-left+rear-right+boot", "Rear left + rear right + boot", 0x38, 0x00 },
        { "front-left+rear-left+rear-right+boot", "Front left + rear left + rear right + boot", 0x78, 0x00 },
    };

    static void CopyText(char* target, size_t targetSize, const char* source)
    {
        if (target == nullptr || targetSize == 0)
        {
            return;
        }
        std::snprintf(target, targetSize, "%s", source != nullptr ? source : "");
    }

    static void BuildDoorPopupShowMessage(DisplayMessageStruct& displayMessage, uint8_t doorStatus1, uint8_t doorStatus2)
    {
        CanDisplayByte2Struct byte3{};
        byte3.data.show_popup_on_emf = 1;
        byte3.data.show_popup_on_cmb = 1;
        byte3.data.show_popup_on_vth = 1;
        byte3.data.priority = 1;

        auto& display = displayMessage.data;
        display.Field1 = CAN_POPUP_MSG_SHOW_CATEGORY1;
        display.Field2 = CAN_POPUP_MSG_DOORS_BOOT_BONNET_REAR_SCREEN_AND_FUEL_TANK_OPEN;
        display.Field3 = byte3.asByte;
        display.Field4 = doorStatus1;
        display.Field5 = doorStatus2;
        display.Field6 = 0xFF;
        display.Field7 = 0x00;
        display.Field8 = 0x00;
    }

    static void BuildDoorPopupHideMessage(DisplayMessageStruct& displayMessage)
    {
        CanDisplayByte2Struct byte3{};
        byte3.data.show_popup_on_emf = 0;
        byte3.data.show_popup_on_cmb = 0;
        byte3.data.show_popup_on_vth = 0;
        byte3.data.priority = 1;

        auto& display = displayMessage.data;
        display.Field1 = CAN_POPUP_MSG_HIDE;
        display.Field2 = CAN_POPUP_MSG_DOORS_BOOT_BONNET_REAR_SCREEN_AND_FUEL_TANK_OPEN;
        display.Field3 = byte3.asByte;
        display.Field4 = 0x00;
        display.Field5 = 0xFF;
        display.Field6 = 0xFF;
        display.Field7 = 0xFF;
        display.Field8 = 0xFF;
    }

    void AppendDoorPopupDebugLog(DoorPopupDebugState& debugState, uint64_t currentTime)
    {
        auto& logEntry = debugState.logEntries[debugState.logWriteIndex];
        logEntry.valid = true;
        logEntry.sequence = ++debugState.sequence;
        logEntry.timestampMs = currentTime;
        logEntry.presetIndex = debugState.currentPresetIndex;
        logEntry.showPhase = debugState.showPhase;
        logEntry.displayMessage = debugState.overrideDisplayMessage;

        debugState.logWriteIndex = (debugState.logWriteIndex + 1) % DoorPopupDebugState::LogCapacity;
        if (debugState.logCount < DoorPopupDebugState::LogCapacity)
        {
            ++debugState.logCount;
        }
    }

    void ApplyDoorPopupDebugPhase(DoorPopupDebugState& debugState, uint64_t currentTime)
    {
        if (debugState.currentPresetIndex >= DoorPopupDebugPresetCount)
        {
            return;
        }

        const auto& preset = DoorPopupDebugPresets[debugState.currentPresetIndex];
        debugState.currentDoorStatus1 = preset.doorStatus1;
        debugState.currentDoorStatus2 = preset.doorStatus2;
        CopyText(debugState.currentStateKey, sizeof(debugState.currentStateKey), preset.key);
        CopyText(debugState.currentStateLabel, sizeof(debugState.currentStateLabel), preset.label);

        if (debugState.showPhase)
        {
            BuildDoorPopupShowMessage(debugState.overrideDisplayMessage, preset.doorStatus1, preset.doorStatus2);
        }
        else
        {
            BuildDoorPopupHideMessage(debugState.overrideDisplayMessage);
        }

        debugState.overrideActive = true;
        debugState.overrideReleaseAtMs = 0;
        debugState.lastTransitionAtMs = currentTime;
        debugState.lastFramePreparedAtMs = currentTime;
        AppendDoorPopupDebugLog(debugState, currentTime);
    }

    void StartDoorPopupDebug(uint64_t currentTime)
    {
        if (_carState == nullptr)
        {
            return;
        }

        auto& debugState = _carState->DoorPopupDebug;
        debugState.Reset();
        debugState.enabled = true;
        debugState.finished = false;
        debugState.showPhase = true;
        debugState.currentPresetIndex = 0;
        debugState.presetCount = DoorPopupDebugPresetCount;
        debugState.startedAtMs = currentTime;
        debugState.phaseStartedAtMs = currentTime;
        ApplyDoorPopupDebugPhase(debugState, currentTime);
    }

    void StopDoorPopupDebug(uint64_t currentTime, bool finished)
    {
        if (_carState == nullptr)
        {
            return;
        }

        auto& debugState = _carState->DoorPopupDebug;
        debugState.enabled = false;
        debugState.finished = finished;
        debugState.showPhase = false;
        debugState.lastTransitionAtMs = currentTime;
        debugState.lastFramePreparedAtMs = currentTime;

        if (!finished)
        {
            BuildDoorPopupHideMessage(debugState.overrideDisplayMessage);
            debugState.overrideActive = true;
            debugState.overrideReleaseAtMs = currentTime + 1000;
            AppendDoorPopupDebugLog(debugState, currentTime);
            return;
        }

        debugState.overrideActive = false;
        debugState.overrideReleaseAtMs = 0;
    }

    void ProcessDoorPopupDebug()
    {
        if (_carState == nullptr)
        {
            return;
        }

        auto& debugState = _carState->DoorPopupDebug;
        if (!debugState.enabled)
        {
            if (debugState.overrideActive && debugState.overrideReleaseAtMs != 0 && _carState->CurrenTime >= debugState.overrideReleaseAtMs)
            {
                debugState.overrideActive = false;
                debugState.overrideReleaseAtMs = 0;
            }
            return;
        }

        const uint64_t currentTime = _carState->CurrenTime;
        const uint64_t phaseDuration = debugState.showPhase ? DoorPopupDebugShowDurationMs : DoorPopupDebugHideDurationMs;
        const uint64_t elapsedInPhase = currentTime - debugState.phaseStartedAtMs;

        if (elapsedInPhase < phaseDuration)
        {
            return;
        }

        if (debugState.showPhase)
        {
            debugState.showPhase = false;
            debugState.phaseStartedAtMs = currentTime;
            ApplyDoorPopupDebugPhase(debugState, currentTime);
            return;
        }

        const uint8_t nextPresetIndex = debugState.currentPresetIndex + 1;
        if (nextPresetIndex >= DoorPopupDebugPresetCount)
        {
            StopDoorPopupDebug(currentTime, true);
            return;
        }

        debugState.currentPresetIndex = nextPresetIndex;
        debugState.showPhase = true;
        debugState.phaseStartedAtMs = currentTime;
        ApplyDoorPopupDebugPhase(debugState, currentTime);
    }

    void AddDisplayMessageArrayToJson(cJSON* json, const char* fieldName, const DisplayMessageStruct& displayMessage)
    {
        cJSON* array = cJSON_CreateArray();
        const auto& display = displayMessage.data;
        cJSON_AddItemToArray(array, cJSON_CreateNumber(display.Field1));
        cJSON_AddItemToArray(array, cJSON_CreateNumber(display.Field2));
        cJSON_AddItemToArray(array, cJSON_CreateNumber(display.Field3));
        cJSON_AddItemToArray(array, cJSON_CreateNumber(display.Field4));
        cJSON_AddItemToArray(array, cJSON_CreateNumber(display.Field5));
        cJSON_AddItemToArray(array, cJSON_CreateNumber(display.Field6));
        cJSON_AddItemToArray(array, cJSON_CreateNumber(display.Field7));
        cJSON_AddItemToArray(array, cJSON_CreateNumber(display.Field8));
        cJSON_AddItemToObject(json, fieldName, array);
    }

    void SendDoorPopupDebugStatus(httpd_req_t *req)
    {
        httpd_resp_set_type(req, "application/json");

        cJSON *json = cJSON_CreateObject();
        const auto& debugState = _carState->DoorPopupDebug;

        cJSON_AddBoolToObject(json, "active", debugState.enabled);
        cJSON_AddBoolToObject(json, "finished", debugState.finished);
        cJSON_AddBoolToObject(json, "show_phase", debugState.showPhase);
        cJSON_AddBoolToObject(json, "ignition", _carState->Ignition != 0);
        cJSON_AddBoolToObject(json, "emulate_display_on_destination", _carState->EMULATE_DISPLAY_ON_DESTINATION);
        cJSON_AddNumberToObject(json, "current_preset_index", debugState.currentPresetIndex);
        cJSON_AddNumberToObject(json, "preset_count", DoorPopupDebugPresetCount);
        cJSON_AddStringToObject(json, "current_state_key", debugState.currentStateKey);
        cJSON_AddStringToObject(json, "current_state_label", debugState.currentStateLabel);
        cJSON_AddNumberToObject(json, "elapsed_total_ms", debugState.startedAtMs == 0 ? 0 : static_cast<double>(_carState->CurrenTime - debugState.startedAtMs));
        cJSON_AddNumberToObject(json, "elapsed_phase_ms", debugState.phaseStartedAtMs == 0 ? 0 : static_cast<double>(_carState->CurrenTime - debugState.phaseStartedAtMs));
        cJSON_AddNumberToObject(json, "phase_duration_ms", debugState.showPhase ? DoorPopupDebugShowDurationMs : DoorPopupDebugHideDurationMs);
        cJSON_AddNumberToObject(json, "current_door_status1", debugState.currentDoorStatus1);
        cJSON_AddNumberToObject(json, "current_door_status2", debugState.currentDoorStatus2);
        AddDisplayMessageArrayToJson(json, "current_display", debugState.overrideDisplayMessage);

        cJSON* presets = cJSON_CreateArray();
        for (uint8_t i = 0; i < DoorPopupDebugPresetCount; ++i)
        {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "index", i);
            cJSON_AddStringToObject(item, "key", DoorPopupDebugPresets[i].key);
            cJSON_AddStringToObject(item, "label", DoorPopupDebugPresets[i].label);
            cJSON_AddNumberToObject(item, "door_status1", DoorPopupDebugPresets[i].doorStatus1);
            cJSON_AddNumberToObject(item, "door_status2", DoorPopupDebugPresets[i].doorStatus2);
            cJSON_AddItemToArray(presets, item);
        }
        cJSON_AddItemToObject(json, "presets", presets);

        cJSON* logEntries = cJSON_CreateArray();
        const uint8_t oldestIndex = (debugState.logWriteIndex + DoorPopupDebugState::LogCapacity - debugState.logCount) % DoorPopupDebugState::LogCapacity;
        for (uint8_t i = 0; i < debugState.logCount; ++i)
        {
            const uint8_t index = (oldestIndex + i) % DoorPopupDebugState::LogCapacity;
            const auto& logEntry = debugState.logEntries[index];
            if (!logEntry.valid)
            {
                continue;
            }

            cJSON* item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "sequence", logEntry.sequence);
            cJSON_AddNumberToObject(item, "timestamp_ms", static_cast<double>(logEntry.timestampMs));
            cJSON_AddNumberToObject(item, "preset_index", logEntry.presetIndex);
            cJSON_AddStringToObject(item, "state_key", DoorPopupDebugPresets[logEntry.presetIndex].key);
            cJSON_AddStringToObject(item, "state_label", DoorPopupDebugPresets[logEntry.presetIndex].label);
            cJSON_AddStringToObject(item, "phase", logEntry.showPhase ? "show" : "hide");
            AddDisplayMessageArrayToJson(item, "display", logEntry.displayMessage);
            cJSON_AddItemToArray(logEntries, item);
        }
        cJSON_AddItemToObject(json, "log", logEntries);

        const char *jsonResponse = cJSON_PrintUnformatted(json);
        cJSON_Delete(json);
        httpd_resp_sendstr(req, jsonResponse);
        free((void *)jsonResponse);
    }

    CarState* _carState = nullptr;
    ConfigFile* _configFile = nullptr;
    TimeProvider* _timeProvider = nullptr;
    ImmediateSignalCallback _immediateSignalCallback = nullptr;
    uint64_t _lastRequestTime = 0;
    uint64_t _inactivityTimeout = WIFI_INITIAL_TIMEOUT;
    bool _isRunning = false;

    httpd_handle_t server;
    const char* TAG = "WebServer";
};

