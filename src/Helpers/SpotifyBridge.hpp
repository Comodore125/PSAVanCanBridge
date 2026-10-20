#pragma once
// Spotify Connect controls hosted on the bridge, independent of Safari lifetime.
// Deliberately NOT part of VAN/CAN receive, send, scheduling, or ISR paths.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <ctime>
#include <cstdint>
#include "cJSON.h"
#include "nvs.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "Helpers/CarState.hpp"

class SpotifyBridge {
public:
    static SpotifyBridge& Instance() { static SpotifyBridge singleton; return singleton; }

    static bool IsHotspotAvailable() { return IsStationConnected(); }

    void Start(CarState* state) {
        if (task_) return;
        state_ = state;
        mutex_ = xSemaphoreCreateMutex();
        if (!mutex_) return;
        LoadCredentials();
        // Do not accept stale edge counters after boot or Wi-Fi reconnect.
        lastNext_ = __atomic_load_n(&state_->MediaNextCount, __ATOMIC_RELAXED);
        lastPrevious_ = __atomic_load_n(&state_->MediaPreviousCount, __ATOMIC_RELAXED);
        if (xTaskCreate(&SpotifyBridge::TaskEntry, "SpotifyCtl", 10240, this, 1, &task_) != pdPASS) {
            // Insufficient RAM: fail closed instead of reporting an enabled
            // controller that can never consume seek events.
            printf("SpotifyCtl: task allocation failed\n");
            vSemaphoreDelete(mutex_);
            mutex_ = nullptr;
        }
    }

    bool Provision(const char* client, const char* refresh) {
        if (!mutex_ || !client || !refresh || strlen(client) != 32 || strlen(refresh) < 16 ||
            strlen(refresh) >= sizeof(refresh_)) return false;
        for (int i=0;i<32;i++)
            if (!std::isxdigit(static_cast<unsigned char>(client[i]))) return false;
        if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(500)) != pdTRUE) return false;
        nvs_handle_t h = 0;
        bool ok = nvs_open("spotify_bridge", NVS_READWRITE, &h) == ESP_OK;
        if (ok) {
            ok = nvs_set_str(h, "client_id", client) == ESP_OK &&
                 nvs_set_str(h, "refresh", refresh) == ESP_OK &&
                 nvs_set_u8(h, "enabled", 0) == ESP_OK &&
                 nvs_set_u8(h, "invert", 0) == ESP_OK &&
                 nvs_commit(h) == ESP_OK;
            CloseNvs(h);
        }
        if (ok) {
            snprintf(clientId_, sizeof(clientId_), "%s", client);
            snprintf(refresh_, sizeof(refresh_), "%s", refresh);
            enabled_ = false;
            invert_ = false;
            access_.clear();
            expiresMs_ = 0;
            error_ = "Credentials installed; enable control explicitly";
        }
        xSemaphoreGive(mutex_);
        return ok;
    }

    bool Enable(bool value, bool invert) {
        if (!mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(500)) != pdTRUE) return false;
        if (value && (!clientId_[0] || !refresh_[0])) { xSemaphoreGive(mutex_); return false; }
        nvs_handle_t h = 0;
        bool ok = nvs_open("spotify_bridge", NVS_READWRITE, &h) == ESP_OK;
        if (ok) {
            ok = nvs_set_u8(h, "enabled", value ? 1 : 0) == ESP_OK &&
                 nvs_set_u8(h, "invert", invert ? 1 : 0) == ESP_OK &&
                 nvs_commit(h) == ESP_OK;
            CloseNvs(h);
        }
        if (ok) {
            enabled_ = value;
            invert_ = invert;
        }
        xSemaphoreGive(mutex_);
        return ok;
    }

    bool Clear() {
        if (!mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(500)) != pdTRUE) return false;
        nvs_handle_t h = 0;
        bool ok = nvs_open("spotify_bridge", NVS_READWRITE, &h) == ESP_OK;
        if (ok) { ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK; CloseNvs(h); }
        if (ok) {
            enabled_ = false;
            memset(clientId_, 0, sizeof(clientId_));
            memset(refresh_, 0, sizeof(refresh_));
            access_.clear();
            expiresMs_ = 0;
            error_ = "Credentials cleared";
        }
        xSemaphoreGive(mutex_);
        return ok;
    }

    std::string Status() {
        if (!mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(200)) != pdTRUE)
            return "{\"ready\":false}";
        cJSON* json = cJSON_CreateObject();
        if (!json) { xSemaphoreGive(mutex_); return "{}"; }
        cJSON_AddBoolToObject(json, "ready", true);
        cJSON_AddBoolToObject(json, "configured", clientId_[0] && refresh_[0]);
        cJSON_AddBoolToObject(json, "enabled", enabled_);
        cJSON_AddBoolToObject(json, "invert", invert_);
        cJSON_AddBoolToObject(json, "online", IsStationConnected());
        cJSON_AddStringToObject(json, "last_action", lastAction_.c_str());
        cJSON_AddStringToObject(json, "error", error_.c_str());
        cJSON_AddNumberToObject(json, "last_http", lastHttp_);
        char* output = cJSON_PrintUnformatted(json);
        cJSON_Delete(json);
        std::string response = output ? output : "{}";
        if (output) cJSON_free(output);
        xSemaphoreGive(mutex_);
        return response;
    }

private:
    // Prevent a GCC -Wdangling-pointer false positive after nvs_open out-param.
    static __attribute__((noinline)) void CloseNvs(nvs_handle_t value) { nvs_close(value); }
    // GCC/ESP-IDF sometimes diagnoses NVS opaque integer handles as
    // a dangling pointer. Keep the NVS load in a separate function and
    // locally suppress that false-positive (the handle is a value type).
    __attribute__((noinline)) void LoadCredentials() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
        nvs_handle_t handle = 0;
        if (nvs_open("spotify_bridge", NVS_READONLY, &handle) == ESP_OK) {
            size_t length = sizeof(clientId_);
            if (nvs_get_str(handle, "client_id", clientId_, &length) != ESP_OK) clientId_[0] = '\0';
            length = sizeof(refresh_);
            if (nvs_get_str(handle, "refresh", refresh_, &length) != ESP_OK) refresh_[0] = '\0';
            uint8_t enabled = 0;
            if (nvs_get_u8(handle, "enabled", &enabled) == ESP_OK) enabled_ = enabled != 0;
            uint8_t invert = 0;
            if (nvs_get_u8(handle, "invert", &invert) == ESP_OK) invert_ = invert != 0;
            CloseNvs(handle);
        }
#pragma GCC diagnostic pop
    }
    SpotifyBridge() = default;
    SpotifyBridge(const SpotifyBridge&) = delete;
    SpotifyBridge& operator=(const SpotifyBridge&) = delete;
    CarState* state_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t task_ = nullptr;
    char clientId_[33]{};
    char refresh_[2048]{};
    bool enabled_ = false;
    bool invert_ = false;
    std::string access_;
    uint64_t expiresMs_ = 0;
    uint64_t retryAfterMs_ = 0;
    std::string error_;
    std::string lastAction_;
    int lastHttp_ = 0;
    uint32_t lastNext_ = 0, lastPrevious_ = 0;

    struct Response {
        std::string body;
        int code = 0;
        int retrySeconds = 0;
    };
    static uint64_t NowMs() { return uint64_t(esp_timer_get_time()/1000ULL); }
    static bool IsStationConnected() {
        wifi_ap_record_t record{};
        return esp_wifi_sta_get_ap_info(&record) == ESP_OK;
    }
    static std::string Encode(const char* data) {
        static const char* hex="0123456789ABCDEF";
        std::string result;
        for (const unsigned char* p=reinterpret_cast<const unsigned char*>(data); *p; ++p) {
            if (std::isalnum(*p) || *p=='-' || *p=='_' || *p=='.' || *p=='~') result.push_back(char(*p));
            else { result.push_back('%'); result.push_back(hex[*p>>4]); result.push_back(hex[*p&15]); }
        }
        return result;
    }
    static esp_err_t HttpEvent(esp_http_client_event_t* event) {
        auto* response = static_cast<Response*>(event->user_data);
        if (!response) return ESP_OK;
        if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0 && response->body.size() < 8192)
            response->body.append(static_cast<const char*>(event->data),
                std::min(size_t(event->data_len), size_t(8192 - response->body.size())));
        if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
            strcasecmp(event->header_key, "Retry-After") == 0)
            response->retrySeconds = std::max(0, atoi(event->header_value));
        return ESP_OK;
    }
    static Response Request(const char* url, const char* method, const std::string& body,
                            const char* contentType, const char* bearer) {
        Response result;
        esp_http_client_config_t config{};
        config.url = url;
        config.event_handler = &HttpEvent;
        config.user_data = &result;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.timeout_ms = 5000;
        config.disable_auto_redirect = true;
        esp_http_client_handle_t h = esp_http_client_init(&config);
        if (!h) return result;
        esp_http_client_set_method(h, strcmp(method,"POST")==0 ? HTTP_METHOD_POST : HTTP_METHOD_GET);
        if (contentType) esp_http_client_set_header(h, "Content-Type", contentType);
        std::string authorization;
        if (bearer) {
            authorization = std::string("Bearer ") + bearer;
            esp_http_client_set_header(h, "Authorization", authorization.c_str());
        }
        if (!body.empty()) esp_http_client_set_post_field(h, body.c_str(), int(body.size()));
        esp_err_t err = esp_http_client_perform(h);
        if (err == ESP_OK) result.code = esp_http_client_get_status_code(h);
        esp_http_client_cleanup(h);
        return result;
    }
    bool RefreshAccess() {
        char cid[33], refreshCopy[2048];
        xSemaphoreTake(mutex_, portMAX_DELAY);
        snprintf(cid, sizeof(cid), "%s", clientId_);
        snprintf(refreshCopy, sizeof(refreshCopy), "%s", refresh_);
        xSemaphoreGive(mutex_);
        if (!cid[0] || !refreshCopy[0]) return false;
        const std::string form = "grant_type=refresh_token&client_id=" + Encode(cid)
                               + "&refresh_token=" + Encode(refreshCopy);
        Response res = Request("https://accounts.spotify.com/api/token","POST",form,
                               "application/x-www-form-urlencoded",nullptr);
        cJSON* root = cJSON_Parse(res.body.c_str());
        const cJSON* token = root ? cJSON_GetObjectItemCaseSensitive(root,"access_token") : nullptr;
        const cJSON* expiry = root ? cJSON_GetObjectItemCaseSensitive(root,"expires_in") : nullptr;
        const cJSON* rotated = root ? cJSON_GetObjectItemCaseSensitive(root,"refresh_token") : nullptr;
        const bool valid = res.code == 200 && cJSON_IsString(token) && strlen(token->valuestring) > 10;
        xSemaphoreTake(mutex_, portMAX_DELAY);
        lastHttp_ = res.code;
        if (valid) {
            access_ = token->valuestring;
            expiresMs_ = NowMs() + uint64_t(std::max(60, cJSON_IsNumber(expiry) ? expiry->valueint : 3600)-90)*1000;
            error_.clear();
            if (cJSON_IsString(rotated) && rotated->valuestring && strlen(rotated->valuestring)<sizeof(refresh_)) {
                nvs_handle_t h = 0;
                if (nvs_open("spotify_bridge",NVS_READWRITE,&h)==ESP_OK) {
                    if (nvs_set_str(h,"refresh",rotated->valuestring)==ESP_OK && nvs_commit(h)==ESP_OK)
                        snprintf(refresh_,sizeof(refresh_),"%s",rotated->valuestring);
                    CloseNvs(h);
                }
            }
        } else {
            error_ = "Spotify token refresh failed (HTTP " + std::to_string(res.code) + ")";
            retryAfterMs_ = NowMs()+30000;
        }
        xSemaphoreGive(mutex_);
        if (root) cJSON_Delete(root);
        memset(refreshCopy,0,sizeof(refreshCopy));
        return valid;
    }
    bool SendAction(const char* action) {
        if (NowMs() < retryAfterMs_) return false;
        bool expired;
        xSemaphoreTake(mutex_,portMAX_DELAY);
        expired = access_.empty() || NowMs() >= expiresMs_;
        xSemaphoreGive(mutex_);
        if (expired && !RefreshAccess()) return false;
        std::string bearer;
        xSemaphoreTake(mutex_,portMAX_DELAY);
        bearer=access_;
        xSemaphoreGive(mutex_);
        const std::string uri=std::string("https://api.spotify.com/v1/me/player/") + action;
        Response result = Request(uri.c_str(),"POST","","application/json",bearer.c_str());
        if (result.code==401) {
            xSemaphoreTake(mutex_,portMAX_DELAY); access_.clear(); xSemaphoreGive(mutex_);
            if (RefreshAccess()) {
                xSemaphoreTake(mutex_,portMAX_DELAY); bearer=access_; xSemaphoreGive(mutex_);
                result=Request(uri.c_str(),"POST","","application/json",bearer.c_str());
            }
        }
        const bool ok = result.code == 204 || result.code == 200 || result.code == 202;
        xSemaphoreTake(mutex_,portMAX_DELAY);
        lastHttp_ = result.code;
        if (ok) {
            lastAction_ = action;
            error_.clear();
        } else {
            error_ = std::string(action) + " failed (HTTP " + std::to_string(result.code) + ")";
            retryAfterMs_ = NowMs() +
                ((result.code == 429) ? uint64_t(std::max(3,result.retrySeconds))*1000 : 5000);
        }
        xSemaphoreGive(mutex_);
        return ok;
    }
    static void TaskEntry(void* parameter) {
        static_cast<SpotifyBridge*>(parameter)->Loop();
    }
    void Loop() {
        bool sntpStarted = false;
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(200));
            if (!state_ || !mutex_) continue;
            const uint32_t next = __atomic_load_n(&state_->MediaNextCount, __ATOMIC_RELAXED);
            const uint32_t prev = __atomic_load_n(&state_->MediaPreviousCount, __ATOMIC_RELAXED);
            const uint32_t newNext = next - lastNext_, newPrev = prev - lastPrevious_;
            lastNext_ = next;
            lastPrevious_ = prev;
            // After SNTP has been initialized, idle ticks require no Wi-Fi
            // driver query or mutex. Preserve 200 ms event latency and never
            // queue old presses while authorization/network is unavailable.
            if (sntpStarted && newNext == 0 && newPrev == 0) continue;
            bool enabled, configured, invert;
            xSemaphoreTake(mutex_,portMAX_DELAY);
            enabled = enabled_;
            invert = invert_;
            configured = clientId_[0] && refresh_[0];
            xSemaphoreGive(mutex_);
            if (!enabled || !configured || !IsStationConnected()) continue;
            if (!sntpStarted) {
                esp_sntp_config_t ntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
                if (esp_netif_sntp_init(&ntp)==ESP_OK) sntpStarted=true;
            }
            // Certificate validation requires valid wall-clock time.
            if (time(nullptr)<1704067200) continue;
            if (newNext + newPrev > 4 || NowMs() < retryAfterMs_) continue;
            for(uint32_t i=0;i<newNext;i++) if (!SendAction(invert ? "previous" : "next")) break;
            for(uint32_t i=0;i<newPrev;i++) if (!SendAction(invert ? "next" : "previous")) break;
        }
    }
};
