#include "youtube_audio_poller.h"
#include <nvs.h>
#include <nvs_flash.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>

#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio/audio_service.h"
#include "protocols/protocol.h"
#include "assets/lang_config.h"

static const char* TAG = "YTPoller";
std::string YouTubeAudioPoller::GetServerDomain() {
    nvs_handle_t nvs;
    char buf[64] = {};
    size_t len = sizeof(buf);
    if (nvs_open("system", NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_str(nvs, "server_domain", buf, &len) == ESP_OK && len > 0) {
            nvs_close(nvs);
            return std::string(buf);
        }
        nvs_close(nvs);
    }
    return "xiaozhiscig.biz.id";
}

std::string YouTubeAudioPoller::GetBaseUrl() {
    return "http://" + GetServerDomain();
}

std::string YouTubeAudioPoller::GetStreamUrl() {
    return "http://" + GetServerDomain();
}

YouTubeAudioPoller::YouTubeAudioPoller() {
}

YouTubeAudioPoller::~YouTubeAudioPoller() {
    Stop();
    if (task_stack_ != nullptr) {
        heap_caps_free(task_stack_);
        task_stack_ = nullptr;
    }
    if (task_buffer_ != nullptr) {
        heap_caps_free(task_buffer_);
        task_buffer_ = nullptr;
    }
}

YouTubeAudioPoller& YouTubeAudioPoller::GetInstance() {
    static YouTubeAudioPoller instance;
    return instance;
}

void YouTubeAudioPoller::Start() {
    if (running_.load()) {
        return;
    }
    running_ = true;
    abort_requested_ = false;

    const size_t stack_size = 4096 * 8; // 32KB stack in PSRAM
    if (task_stack_ == nullptr) {
        task_stack_ = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (task_buffer_ == nullptr) {
        task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    }

    if (task_stack_ != nullptr && task_buffer_ != nullptr) {
        task_handle_ = xTaskCreateStatic([](void* arg) {
            static_cast<YouTubeAudioPoller*>(arg)->TaskLoop();
            vTaskDelete(NULL);
        }, "yt_poller", stack_size, this, 3, task_stack_, task_buffer_);
        ESP_LOGI(TAG, "YouTubeAudioPoller started (PSRAM stack: 32KB)");
    } else {
        xTaskCreate([](void* arg) {
            static_cast<YouTubeAudioPoller*>(arg)->TaskLoop();
            vTaskDelete(NULL);
        }, "yt_poller", 8192, this, 3, &task_handle_);
        ESP_LOGI(TAG, "YouTubeAudioPoller started (internal stack fallback: 8KB)");
    }
}

void YouTubeAudioPoller::Stop() {
    if (!running_.load()) {
        return;
    }
    running_ = false;
    abort_requested_ = true;
    if (task_handle_ != nullptr) {
        task_handle_ = nullptr;
    }
    ESP_LOGI(TAG, "YouTubeAudioPoller stopped");
}

void YouTubeAudioPoller::Abort() {
    has_pending_confirmation_ = false;
    if (is_playing_.load()) {
        ESP_LOGI(TAG, "Abort requested for YouTube audio stream");
        abort_requested_ = true;
        Application::GetInstance().GetAudioService().ResetDecoder();
    }
}

void YouTubeAudioPoller::TriggerFetch() {
    ESP_LOGI(TAG, "Fetch triggered externally");
    trigger_requested_ = true;
}

void YouTubeAudioPoller::PlaySong(const std::string& video_id, const std::string& title, const std::string& duration) {
    std::string vid = video_id;
    std::string t = title;
    std::string dur = duration;
    {
        std::lock_guard<std::mutex> lock(song_mutex_);
        if (vid.empty() && !pending_video_id_.empty()) {
            vid = pending_video_id_;
            if (t.empty()) {
                t = pending_title_;
            }
            if (dur.empty()) {
                dur = pending_duration_;
            }
        }
        if (vid.empty()) {
            if (!t.empty()) {
                PlayQuery(t);
            } else {
                ESP_LOGW(TAG, "PlaySong called with empty video_id and no pending song");
            }
            return;
        }
        pending_video_id_ = vid;
        pending_title_ = t.empty() ? "YouTube Song" : t;
        pending_duration_ = dur;
    }
    ESP_LOGI(TAG, "PlaySong called: title=\"%s\", vid=%s, dur=\"%s\"",
             pending_title_.c_str(), pending_video_id_.c_str(), pending_duration_.c_str());
    has_pending_confirmation_ = false;
    play_requested_ = true;
}

bool YouTubeAudioPoller::SearchSong(const std::string& query, std::string& video_id, std::string& title, std::string& duration) {
    if (query.empty()) {
        return false;
    }
    bool found = FetchDirect(query, video_id, title, duration);
    if (found) {
        std::lock_guard<std::mutex> lock(song_mutex_);
        pending_video_id_ = video_id;
        pending_title_ = title;
        pending_duration_ = duration;
        has_pending_confirmation_ = true;
        ESP_LOGI(TAG, "SearchSong cached: title=\"%s\", vid=%s, dur=\"%s\"",
                 title.c_str(), video_id.c_str(), duration.c_str());
    }
    return found;
}

void YouTubeAudioPoller::PlayQuery(const std::string& query) {
    if (query.empty()) {
        return;
    }
    // Guard: ignore if already playing or a play is pending (prevents TTS text triggering duplicate)
    if (is_playing_.load() || play_requested_.load()) {
        ESP_LOGW(TAG, "PlayQuery ignored (already playing or pending): \"%s\"", query.c_str());
        return;
    }
    ESP_LOGI(TAG, "PlayQuery called: query=\"%s\"", query.c_str());
    {
        std::lock_guard<std::mutex> lock(song_mutex_);
        pending_query_ = query;
    }
    query_requested_ = true;
}

bool YouTubeAudioPoller::FetchDirect(const std::string& query, std::string& video_id, std::string& title, std::string& duration) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) return false;

    auto http = network->CreateHttp(0);
    if (http == nullptr) return false;

    std::string encoded_q;
    for (char c : query) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded_q += c;
        } else if (c == ' ') {
            encoded_q += "%20";
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
            encoded_q += hex;
        }
    }

    std::string mac = SystemInfo::GetMacAddress();
    std::string url = GetStreamUrl() + "/api/audio/play_direct?q=" + encoded_q + "&mac=" + mac;
    ESP_LOGI(TAG, "FetchDirect querying URL: %s", url.c_str());
    http->SetTimeout(12000);
    http->SetHeader("User-Agent", "ESP32-XiaoZhi");
    http->SetHeader("Device-Id", mac.c_str());
    http->SetHeader("X-Device-Mac", mac.c_str());

    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "FetchDirect failed to open: %s", url.c_str());
        http->Close();
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGW(TAG, "FetchDirect HTTP returned %d", http->GetStatusCode());
        http->Close();
        return false;
    }

    std::string body = http->ReadAll();
    http->Close();

    if (body.empty()) return false;

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return false;

    bool found = false;
    cJSON* vid_item = cJSON_GetObjectItem(root, "video_id");
    cJSON* title_item = cJSON_GetObjectItem(root, "title");
    cJSON* dur_item = cJSON_GetObjectItem(root, "duration");
    cJSON* dur_str_item = cJSON_GetObjectItem(root, "duration_string");

    if (cJSON_IsString(vid_item)) {
        video_id = vid_item->valuestring;
        title = cJSON_IsString(title_item) ? title_item->valuestring : query;

        if (cJSON_IsString(dur_str_item)) {
            duration = dur_str_item->valuestring;
        } else if (cJSON_IsNumber(dur_item) && dur_item->valueint > 0) {
            int sec = dur_item->valueint;
            int m = sec / 60;
            int s = sec % 60;
            char buf[32];
            snprintf(buf, sizeof(buf), "%d:%02d", m, s);
            duration = buf;
        } else {
            duration = "";
        }
        found = true;
    }
    cJSON_Delete(root);
    return found;
}

bool YouTubeAudioPoller::FetchDirect(const std::string& query, std::string& video_id, std::string& title) {
    std::string dur;
    return FetchDirect(query, video_id, title, dur);
}

bool YouTubeAudioPoller::FetchCommands(int& cmd_id, std::string& video_id, std::string& title, std::string& duration) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }

    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        return false;
    }

    std::string mac = SystemInfo::GetMacAddress();
    std::string chip = CONFIG_IDF_TARGET;
    std::string url = GetBaseUrl() + "/api/device/audio/commands?mac=" + mac + "&chip=" + chip;

    http->SetTimeout(10000);
    http->SetHeader("User-Agent", "ESP32-XiaoZhi");
    http->SetHeader("Device-Id", mac.c_str());
    http->SetHeader("X-Device-Mac", mac.c_str());
    http->SetHeader("X-Chip-Type", chip.c_str());
    http->SetHeader("X-Device-Chip", chip.c_str());
    http->SetHeader("Device-Chip", chip.c_str());

    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "FetchCommands: Failed to connect to %s", url.c_str());
        http->Close();
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGW(TAG, "FetchCommands: HTTP returned status %d", http->GetStatusCode());
        http->Close();
        return false;
    }

    std::string body = http->ReadAll();
    http->Close();

    if (body.empty()) {
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }

    bool found = false;
    cJSON* commands = cJSON_GetObjectItem(root, "commands");
    if (cJSON_IsArray(commands) && cJSON_GetArraySize(commands) > 0) {
        cJSON* cmd = cJSON_GetArrayItem(commands, 0);
        if (cmd != nullptr) {
            cJSON* id_item = cJSON_GetObjectItem(cmd, "id");
            cJSON* vid_item = cJSON_GetObjectItem(cmd, "video_id");
            cJSON* title_item = cJSON_GetObjectItem(cmd, "title");
            cJSON* dur_item = cJSON_GetObjectItem(cmd, "duration");
            cJSON* dur_str_item = cJSON_GetObjectItem(cmd, "duration_string");

            if (cJSON_IsNumber(id_item) && cJSON_IsString(vid_item)) {
                cmd_id = id_item->valueint;
                video_id = vid_item->valuestring;
                title = cJSON_IsString(title_item) ? title_item->valuestring : "YouTube Song";

                if (cJSON_IsString(dur_str_item)) {
                    duration = dur_str_item->valuestring;
                } else if (cJSON_IsNumber(dur_item) && dur_item->valueint > 0) {
                    int sec = dur_item->valueint;
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%d:%02d", sec / 60, sec % 60);
                    duration = buf;
                } else {
                    duration = "";
                }
                found = true;
            }
        }
    }

    cJSON_Delete(root);
    return found;
}

bool YouTubeAudioPoller::FetchCommands(int& cmd_id, std::string& video_id, std::string& title) {
    std::string dur;
    return FetchCommands(cmd_id, video_id, title, dur);
}

bool YouTubeAudioPoller::AckCommand(int cmd_id) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }

    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        return false;
    }

    std::string mac = SystemInfo::GetMacAddress();
    std::string chip = CONFIG_IDF_TARGET;
    std::string url = GetBaseUrl() + "/api/device/audio/ack";

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "command_id", cmd_id);
    cJSON_AddStringToObject(root, "mac", mac.c_str());
    cJSON_AddStringToObject(root, "chip", chip.c_str());
    char* json_str = cJSON_PrintUnformatted(root);
    std::string payload = json_str ? json_str : "";
    if (json_str) {
        free(json_str);
    }
    cJSON_Delete(root);

    http->SetTimeout(10000);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("User-Agent", "ESP32-XiaoZhi");
    http->SetHeader("Device-Id", mac.c_str());
    http->SetHeader("X-Device-Mac", mac.c_str());
    http->SetHeader("X-Chip-Type", chip.c_str());
    http->SetHeader("X-Device-Chip", chip.c_str());
    http->SetHeader("Device-Chip", chip.c_str());
    http->SetContent(std::move(payload));

    bool ok = http->Open("POST", url);
    if (ok) {
        http->ReadAll();
    }
    http->Close();
    return ok;
}

bool YouTubeAudioPoller::ReportStatus(const std::string& status, const std::string& video_id) {
    ESP_LOGI(TAG, "ReportStatus: status=\"%s\", video_id=%s (local)", status.c_str(), video_id.c_str());
    return true;
}


void YouTubeAudioPoller::StreamAudio(const std::string& video_id, const std::string& title, const std::string& duration) {
    auto& app = Application::GetInstance();
    auto display = Board::GetInstance().GetDisplay();

    is_playing_ = true;
    abort_requested_ = false;
    app.ResetAborted();

    int total_seconds = 0;
    if (!duration.empty()) {
        int p1 = 0, p2 = 0, p3 = 0;
        int count = sscanf(duration.c_str(), "%d:%d:%d", &p1, &p2, &p3);
        if (count == 3) {
            total_seconds = p1 * 3600 + p2 * 60 + p3;
        } else {
            count = sscanf(duration.c_str(), "%d:%d", &p1, &p2);
            if (count == 2) {
                total_seconds = p1 * 60 + p2;
            } else {
                count = sscanf(duration.c_str(), "%d", &p1);
                if (count == 1) {
                    total_seconds = p1;
                }
            }
        }
    }

    ESP_LOGI(TAG, "Starting YouTube stream: %s (%s, dur=%s, %ds)",
             title.c_str(), video_id.c_str(), duration.c_str(), total_seconds);
    ReportStatus("playing", video_id);
    app.GetAudioService().EnableWakeWordDetection(false);

    // Wait for previous audio playback to finish (max 3 sec) before opening stream
    {
        int wait_ms = 0;
        while (!app.GetAudioService().IsPlaybackIdle() && wait_ms < 3000 && !abort_requested_.load()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_ms += 100;
        }
        if (wait_ms > 0) {
            ESP_LOGI(TAG, "Waited %d ms for previous playback to finish before streaming", wait_ms);
        }
    }

    if (abort_requested_.load()) {
        is_playing_ = false;
        app.SetDeviceState(kDeviceStateIdle);
        return;
    }

    // Close any open audio channel to the server so mic does not record speaker output or hit 30s listening timeout
    app.CloseAudioChannel();
    app.SetDeviceState(kDeviceStateSpeaking);  // Show "speaking" state during YouTube playback

    // Clear any pending play/query requests that accumulated during TTS wait
    play_requested_ = false;
    query_requested_ = false;

    std::string display_title = "[PLAY] " + title;
    if (display != nullptr) {
        display->StartMusicVisualizer(title.c_str(), total_seconds);
        display->ShowNotification(display_title.c_str(), 5000);
    }
    auto bl = Board::GetInstance().GetBacklight();
    if (bl != nullptr) {
        ESP_LOGI(TAG, "YouTube playback started -> set screen brightness to 70%%");
        bl->SetBrightness(70, false);
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        if (!codec->output_enabled()) {
            codec->EnableOutput(true);
        }
        ESP_LOGI(TAG, "Audio playback using current device volume: %d%%", codec->output_volume());
    }

    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Reset decoder to clear any leftover TTS audio in the queue
    app.GetAudioService().ResetDecoder();
    vTaskDelay(pdMS_TO_TICKS(50));

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        is_playing_ = false;
        if (display != nullptr) {
            display->StopMusicVisualizer();
        }
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        esp_wifi_set_ps(WIFI_PS_NONE);
        app.SetDeviceState(kDeviceStateIdle);
        return;
    }

    // 1. Initial Bitrate Evaluation based on Wi-Fi RSSI & Free Internal SRAM
    wifi_ap_record_t ap_info;
    int8_t rssi = -100;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }
    size_t free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    std::string current_bitrate = "32k";
#if CONFIG_IDF_TARGET_ESP32S3
    if (rssi >= -72) {
        current_bitrate = "32k"; // Sinyal Bagus: 32k (High fidelity, extremely stable on ESP32-S3)
    } else if (rssi >= -80) {
        current_bitrate = "24k"; // Sinyal Sedang: 24k
    } else if (rssi >= -86) {
        current_bitrate = "16k"; // Sinyal Lemah: 16k
    } else {
        current_bitrate = "12k"; // Sinyal Sangat Lemah: 12k
    }
    static const size_t kOggBufCap = 65536; // 64KB buffer leveraging 8MB PSRAM for smooth TCP window
#else
    if (rssi >= -65 && free_sram >= 48000) {
        current_bitrate = "16k";
    } else if (rssi >= -75 && free_sram >= 36000) {
        current_bitrate = "12k";
    } else if (rssi >= -82 && free_sram >= 28000) {
        current_bitrate = "8k";
    } else {
        current_bitrate = "6k";
    }
    static const size_t kOggBufCap = 4096;
#endif
    ESP_LOGI(TAG, "Adaptive Profile Selected: RSSI=%d dBm, FreeSRAM=%u B -> Bitrate=%s",
             rssi, (unsigned int)free_sram, current_bitrate.c_str());

    static const uint8_t OGGS_MAGIC[4] = {'O', 'g', 'g', 'S'};
    uint8_t* raw_ogg = (uint8_t*)heap_caps_malloc(kOggBufCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raw_ogg == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate ogg_buf in PSRAM");
        is_playing_ = false;
        if (display != nullptr) {
            display->StopMusicVisualizer();
        }
        app.SetDeviceState(kDeviceStateIdle);
        return;
    }
    std::unique_ptr<uint8_t[], void(*)(void*)> ogg_buf(raw_ogg, [](void* p) { heap_caps_free(p); });

    size_t total_bytes_streamed = 0;
    int frames_pushed = 0;
    int downshift_retries = 0;
    int eof_retries = 0;
    int last_reported_sec = -1;
    static const int kMaxRetries = 4;
    int64_t last_downshift_time_us = 0;
    int64_t upshift_healthy_start_us = esp_timer_get_time();
    int64_t last_rssi_check_us = 0;
    int8_t live_rssi = rssi;

    while (running_.load() && !abort_requested_.load()) {
        bool need_downshift = false;
        auto http = network->CreateHttp(0);
        if (http == nullptr) {
            break;
        }

        float start_sec = frames_pushed * 0.060f;
        char url_buf[256];
        std::string mac = SystemInfo::GetMacAddress();
        std::string chip = CONFIG_IDF_TARGET;
        if (start_sec > 0.5f) {
            snprintf(url_buf, sizeof(url_buf), "%s/api/audio/stream/%s?br=%s&start=%.2f&mac=%s&chip=%s",
                     GetStreamUrl().c_str(), video_id.c_str(), current_bitrate.c_str(), start_sec, mac.c_str(), chip.c_str());
        } else {
            snprintf(url_buf, sizeof(url_buf), "%s/api/audio/stream/%s?br=%s&mac=%s&chip=%s",
                     GetStreamUrl().c_str(), video_id.c_str(), current_bitrate.c_str(), mac.c_str(), chip.c_str());
        }
        std::string stream_url(url_buf);

        http->SetTimeout(4000); // 4s timeout is fast and responsive
        http->SetHeader("User-Agent", "ESP32-XiaoZhi");
        http->SetHeader("Device-Id", mac.c_str());
        http->SetHeader("X-Device-Mac", mac.c_str());
        http->SetHeader("X-Device-Chip", chip.c_str());
        http->SetHeader("Device-Chip", chip.c_str());

        ESP_LOGI(TAG, "Connecting to stream URL: %s", stream_url.c_str());
        if (!http->Open("GET", stream_url)) {
            ESP_LOGE(TAG, "Failed to open stream URL: %s", stream_url.c_str());
            http->Close();
            if (frames_pushed > 100) {
                ESP_LOGI(TAG, "Failed to reconnect beyond stream end (frames=%d), finishing song naturally", frames_pushed);
                need_downshift = false;
                break;
            }
            if (frames_pushed > 0 && downshift_retries < kMaxRetries) {
                downshift_retries++;
                vTaskDelay(pdMS_TO_TICKS(500 * downshift_retries));
                continue;
            }
            break;
        }

        if (http->GetStatusCode() != 200) {
            ESP_LOGE(TAG, "Stream returned HTTP %d", http->GetStatusCode());
            http->Close();
            if (frames_pushed > 100) {
                ESP_LOGI(TAG, "Stream returned %d after playing %d frames, finishing song naturally", http->GetStatusCode(), frames_pushed);
                need_downshift = false;
                break;
            }
            if (frames_pushed > 0 && downshift_retries < kMaxRetries) {
                downshift_retries++;
                vTaskDelay(pdMS_TO_TICKS(500 * downshift_retries));
                continue;
            }
            break;
        }

        ESP_LOGI(TAG, "Stream connected (HTTP 200, br=%s, start=%.2fs), reading chunks...",
                 current_bitrate.c_str(), start_sec);

        if (total_seconds <= 0) {
            std::string h_dur = http->GetResponseHeader("X-Song-Duration");
            if (h_dur.empty()) h_dur = http->GetResponseHeader("Content-Duration");
            if (!h_dur.empty()) {
                int d = 0;
                if (sscanf(h_dur.c_str(), "%d", &d) == 1 && d > 0) {
                    total_seconds = d;
                    if (display != nullptr) {
                        display->SetMusicProgress(0, total_seconds);
                    }
                }
            }
        }

        size_t ogg_len = 0;
        bool seen_head = false;
        bool seen_tags = false;
        bool eos_detected = false;
        int sample_rate = 24000;
        int64_t last_audio_active_us = esp_timer_get_time();
        size_t session_bytes = 0;
        int session_frames = 0;

        // Dynamic Pre-Buffer (Jitter Buffer for Music Playback):
        // Pre-buffer 16-24 frame (~1.0 - 1.5 detik) sebelum audio dimainkan ke speaker.
        // Hal ini memberi ruang bagi HTTP task untuk mendahului mendownload dan mengisi
        // antrean decode hingga 6 detik (100 frame), sehingga musik TIDAK AKAN tersendat!
        size_t prebuffer_target = (live_rssi >= -75) ? 16 : 24;
        std::vector<std::unique_ptr<AudioStreamPacket>> prebuffer;
        bool is_prebuffering = true;

        try {
            while (running_.load() && !abort_requested_.load()) {
                int64_t now_us = esp_timer_get_time();
                bool playback_idle = app.GetAudioService().IsPlaybackIdle();

                // Periksa status sinyal live berkala (tiap 5 detik) dengan aman tanpa lag
                if (now_us - last_rssi_check_us > 5000000) {
                    last_rssi_check_us = now_us;
                    wifi_ap_record_t live_ap;
                    if (esp_wifi_sta_get_ap_info(&live_ap) == ESP_OK) {
                        live_rssi = live_ap.rssi;
                    }
                }

                // 1. Ultra-lightweight Speaker Starvation Watchdog (Fast Drop):
                // Jika audio playback queue kosong / speaker tidak bersuara > 5.0s, auto-recovering!
                if (frames_pushed > 10) {
                    if (!playback_idle) {
                        last_audio_active_us = now_us;
                    } else if (now_us - last_audio_active_us > 5000000) { // 5.0s speaker silence (tolerant to jitter)
                        ESP_LOGW(TAG, "Audio starvation detected! Speaker is silent at br=%s, auto-recovering...", current_bitrate.c_str());
#if CONFIG_IDF_TARGET_ESP32S3
                        if (current_bitrate != "12k") {
                            if (current_bitrate == "40k") current_bitrate = "32k";
                            else if (current_bitrate == "32k") current_bitrate = "24k";
                            else if (current_bitrate == "24k") current_bitrate = "16k";
                            else current_bitrate = "12k";
                            need_downshift = true;
                        } else if (downshift_retries < kMaxRetries) {
                            need_downshift = true;
                            downshift_retries++;
                        }
#else
                        if (current_bitrate != "6k") {
                            if (current_bitrate == "24k" || current_bitrate == "16k") current_bitrate = "12k";
                            else if (current_bitrate == "12k") current_bitrate = "8k";
                            else current_bitrate = "6k";
                            need_downshift = true;
                        } else if (downshift_retries < kMaxRetries) {
                            need_downshift = true;
                            downshift_retries++;
                        }
#endif
                        if (need_downshift) {
                            last_downshift_time_us = now_us;
                            upshift_healthy_start_us = now_us;
                            ESP_LOGW(TAG, "Watchdog recovered stream by downshifting/reconnecting to %s at %.2fs",
                                     current_bitrate.c_str(), frames_pushed * 0.060f);
                            break;
                        }
                    }
                }

                // 2. Adaptive Upshift (Slow & Safe Climb - Golden Rule: Drop Fast, Climb Slow):
                // Jika bitrate di bawah 48k, cooldown downshift > 40s, dan antrean decode sehat (>= 8 frame) selama 35s kontinu:
#if CONFIG_IDF_TARGET_ESP32S3
                if (current_bitrate != "40k" && frames_pushed > 20) {
                    size_t q_size = app.GetAudioService().GetDecodeQueueSize();
                    bool queue_healthy = (q_size >= 8 && !playback_idle);

                    bool rssi_qualified = false;
                    std::string next_tier;
                    if (current_bitrate == "12k" && live_rssi >= -84) {
                        next_tier = "16k";
                        rssi_qualified = true;
                    } else if (current_bitrate == "16k" && live_rssi >= -78) {
                        next_tier = "24k";
                        rssi_qualified = true;
                    } else if (current_bitrate == "24k" && live_rssi >= -72) {
                        next_tier = "32k";
                        rssi_qualified = true;
                    } else if (current_bitrate == "32k" && live_rssi >= -65) {
                        next_tier = "40k";
                        rssi_qualified = true;
                    }

                    if (queue_healthy && rssi_qualified) {
                        if (now_us - last_downshift_time_us > 40000000) { // Cooldown 40 detik
                            if (now_us - upshift_healthy_start_us > 35000000) { // 35 detik konsisten sehat
                                ESP_LOGI(TAG, "Network condition excellent! Sustained healthy buffer & RSSI=%d dBm for 35s. Upshifting %s -> %s at %.2fs",
                                         live_rssi, current_bitrate.c_str(), next_tier.c_str(), frames_pushed * 0.060f);
                                current_bitrate = next_tier;
                                need_downshift = true; // Reconnect bersih ke tingkat bitrate lebih tinggi
                                upshift_healthy_start_us = now_us;
                                break;
                            }
                        }
                    } else {
                        upshift_healthy_start_us = now_us; // Reset jika ada fluktuasi
                    }
                }
#endif

                size_t avail = kOggBufCap - ogg_len;
                if (avail == 0) {
                    // Buffer is full without a parsed page. Discard until next OggS magic.
                    size_t next_oggs = 1;
                    while (next_oggs + 4 <= ogg_len && memcmp(ogg_buf.get() + next_oggs, OGGS_MAGIC, 4) != 0) {
                        next_oggs++;
                    }
                    if (next_oggs < ogg_len) {
                        memmove(ogg_buf.get(), ogg_buf.get() + next_oggs, ogg_len - next_oggs);
                        ogg_len -= next_oggs;
                    } else {
                        ogg_len = 0;
                    }
                    avail = kOggBufCap - ogg_len;
                }
                int to_read = (int)std::min((size_t)8192, avail);
                int n = http->Read(reinterpret_cast<char*>(ogg_buf.get() + ogg_len), to_read);
                if (n < 0) {
                    ESP_LOGW(TAG, "HTTP read error or stream closed (ret=%d, total=%u, frames=%d)",
                             n, (unsigned int)total_bytes_streamed, frames_pushed);
                    // Jika EOS sudah terdeteksi, atau durasi tercapai, atau sesi baru tidak mendapat frame setelah frames_pushed > 100:
                    // JANGAN lakukan downshift/reconnect retry yang membuang waktu puluhan detik!
                    if (eos_detected || (total_seconds > 0 && (frames_pushed * 0.060f >= (total_seconds - 3.0f))) || (session_frames == 0 && frames_pushed > 100)) {
                        ESP_LOGI(TAG, "Natural stream completion detected at %.2fs, finishing playback without retry",
                                 frames_pushed * 0.060f);
                        need_downshift = false;
                        break;
                    }
#if CONFIG_IDF_TARGET_ESP32S3
                    if (current_bitrate != "12k") {
                        if (current_bitrate == "40k") current_bitrate = "32k";
                        else if (current_bitrate == "32k") current_bitrate = "24k";
                        else if (current_bitrate == "24k") current_bitrate = "16k";
                        else current_bitrate = "12k";
                        need_downshift = true;
                        last_downshift_time_us = now_us;
                        upshift_healthy_start_us = now_us;
                        ESP_LOGW(TAG, "Connection interrupted mid-stream! Downshifting to %s at %.2fs",
                                 current_bitrate.c_str(), frames_pushed * 0.060f);
                    } else if (downshift_retries < kMaxRetries) {
                        need_downshift = true;
                        downshift_retries++;
                        last_downshift_time_us = now_us;
                        upshift_healthy_start_us = now_us;
                        ESP_LOGW(TAG, "Connection interrupted at 12k! Reconnecting (attempt %d/%d) at %.2fs",
                                 downshift_retries, kMaxRetries, frames_pushed * 0.060f);
                    }
#else
                    if (current_bitrate != "6k") {
                        if (current_bitrate == "24k" || current_bitrate == "16k") current_bitrate = "12k";
                        else if (current_bitrate == "12k") current_bitrate = "8k";
                        else current_bitrate = "6k";
                        need_downshift = true;
                        last_downshift_time_us = now_us;
                        upshift_healthy_start_us = now_us;
                        ESP_LOGW(TAG, "Connection interrupted mid-stream! Downshifting to %s at %.2fs",
                                 current_bitrate.c_str(), frames_pushed * 0.060f);
                    } else if (downshift_retries < kMaxRetries) {
                        need_downshift = true;
                        downshift_retries++;
                        last_downshift_time_us = now_us;
                        upshift_healthy_start_us = now_us;
                        ESP_LOGW(TAG, "Connection interrupted at 6k! Reconnecting (attempt %d/%d) at %.2fs",
                                 downshift_retries, kMaxRetries, frames_pushed * 0.060f);
                    }
#endif
                    break;
                }
                if (n == 0) {
                    ESP_LOGI(TAG, "Stream EOF reached (session_bytes=%u, session_frames=%d, total=%u, frames_pushed=%d)",
                             (unsigned int)session_bytes, session_frames, (unsigned int)total_bytes_streamed, frames_pushed);
                    bool near_end = (total_seconds > 0 && (frames_pushed * 0.060f >= (total_seconds - 3.0f)));
                    if (eos_detected || near_end || (frames_pushed > 100 && session_bytes == 0)) {
                        ESP_LOGI(TAG, "Confirmed end of audio stream at %.2fs, completing playback naturally", frames_pushed * 0.060f);
                        need_downshift = false;
                    } else if (frames_pushed > 0 && session_bytes > 0 && eof_retries < 2 && !abort_requested_.load()) {
                        eof_retries++;
                        need_downshift = true;
                        ESP_LOGI(TAG, "Potential premature EOF from YouTube/CDN! Auto-resuming at %.2fs (probe %d/2)...",
                                 frames_pushed * 0.060f, eof_retries);
                    } else {
                        need_downshift = false;
                    }
                    break;
                }

                ogg_len += n;
                total_bytes_streamed += n;
                session_bytes += n;

                // Demux Ogg pages incrementally
                while (ogg_len >= 27) {
                    if (abort_requested_.load()) {
                        ESP_LOGI(TAG, "Audio stream aborted by user request");
                        break;
                    }

                    // Find next OggS magic
                    size_t oggs_pos = 0;
                    bool found = false;
                    while (oggs_pos + 4 <= ogg_len) {
                        if (memcmp(ogg_buf.get() + oggs_pos, OGGS_MAGIC, 4) == 0) {
                            found = true;
                            break;
                        }
                        oggs_pos++;
                    }

                    if (!found) {
                        if (ogg_len >= 3) {
                            memmove(ogg_buf.get(), ogg_buf.get() + ogg_len - 3, 3);
                            ogg_len = 3;
                        }
                        break;
                    }

                    if (oggs_pos > 0) {
                        memmove(ogg_buf.get(), ogg_buf.get() + oggs_pos, ogg_len - oggs_pos);
                        ogg_len -= oggs_pos;
                        if (ogg_len < 27) break;
                    }

                    uint8_t header_type = ogg_buf[5];
                    if (header_type & 0x04) {
                        ESP_LOGI(TAG, "Ogg EOS (End Of Stream) flag detected at %.2fs! Song stream is finished.",
                                 frames_pushed * 0.060f);
                        eos_detected = true;
                    }

                    uint8_t num_segs = ogg_buf[26];
                    size_t hdr_len = 27 + num_segs;
                    if (ogg_len < hdr_len) {
                        break;
                    }

                    size_t body_len = 0;
                    for (size_t i = 0; i < num_segs; ++i) {
                        body_len += ogg_buf[27 + i];
                    }

                    size_t page_total = hdr_len + body_len;
                    if (ogg_len < page_total) {
                        break;
                    }

                    // Extract packets from this page
                    size_t cur = hdr_len;
                    size_t seg_idx = 0;
                    while (seg_idx < num_segs) {
                        size_t pkt_len = 0;
                        size_t pkt_start = cur;
                        bool continued = false;
                        do {
                            uint8_t l = ogg_buf[27 + seg_idx++];
                            pkt_len += l;
                            cur += l;
                            continued = (l == 255);
                        } while (continued && seg_idx < num_segs);

                        if (pkt_len == 0) {
                            continue;
                        }

                        const uint8_t* pkt_ptr = ogg_buf.get() + pkt_start;

                        if (!seen_head) {
                            if (pkt_len >= 19 && memcmp(pkt_ptr, "OpusHead", 8) == 0) {
                                seen_head = true;
                                if (pkt_len >= 16) {
                                    sample_rate = pkt_ptr[12] | (pkt_ptr[13] << 8) | (pkt_ptr[14] << 16) | (pkt_ptr[15] << 24);
                                    ESP_LOGI(TAG, "OpusHead: sample_rate=%d", sample_rate);
                                }
                            }
                            continue;
                        }

                        if (!seen_tags) {
                            if (pkt_len >= 8 && memcmp(pkt_ptr, "OpusTags", 8) == 0) {
                                seen_tags = true;
                                ESP_LOGI(TAG, "OpusTags parsed, ready to decode audio");
                            }
                            continue;
                        }

                        if (abort_requested_.load()) {
                            ESP_LOGI(TAG, "Audio stream aborted by user request");
                            break;
                        }

                        // Push raw Opus frame (decode directly at codec output sample rate: 24kHz)
                        auto packet = std::make_unique<AudioStreamPacket>();
                        auto codec = Board::GetInstance().GetAudioCodec();
                        packet->sample_rate = (codec != nullptr) ? codec->output_sample_rate() : 24000;
                        packet->frame_duration = 60;
                        packet->payload.assign(pkt_ptr, pkt_ptr + pkt_len);

                        if (is_prebuffering) {
                            prebuffer.push_back(std::move(packet));
                            if (prebuffer.size() >= prebuffer_target) {
                                ESP_LOGI(TAG, "Pre-buffer ready (%u frames, ~%u ms), flushing to decoder (br=%s)",
                                         (unsigned int)prebuffer.size(), (unsigned int)(prebuffer.size() * 60), current_bitrate.c_str());
                                for (auto& p : prebuffer) {
                                    app.GetAudioService().PushPacketToDecodeQueue(std::move(p), true);
                                    frames_pushed++;
                                    session_frames++;
                                    int current_sec = (int)(frames_pushed * 0.060f);
                                    if (current_sec != last_reported_sec) {
                                        last_reported_sec = current_sec;
                                        if (display != nullptr) {
                                            display->SetMusicProgress(current_sec, total_seconds);
                                        }
                                    }
                                }
                                prebuffer.clear();
                                is_prebuffering = false;
                            }
                        } else {
                            app.GetAudioService().PushPacketToDecodeQueue(std::move(packet), true);
                            frames_pushed++;
                            session_frames++;
                            int current_sec = (int)(frames_pushed * 0.060f);
                            if (current_sec != last_reported_sec) {
                                last_reported_sec = current_sec;
                                if (display != nullptr) {
                                    display->SetMusicProgress(current_sec, total_seconds);
                                }
                            }
                            if (frames_pushed == 1 || frames_pushed % 50 == 0) {
                                ESP_LOGI(TAG, "Audio frame #%d pushed to decoder (len=%u, q_size=%u, elapsed=%ds/%ds)",
                                         frames_pushed, (unsigned int)pkt_len, (unsigned int)app.GetAudioService().GetDecodeQueueSize(),
                                         current_sec, total_seconds);
                            }
                        }

                        if (session_frames >= 20) {
                            downshift_retries = 0;
                            eof_retries = 0;
                        }
                    }

                    memmove(ogg_buf.get(), ogg_buf.get() + page_total, ogg_len - page_total);
                    ogg_len -= page_total;

                    if (abort_requested_.load()) {
                        ESP_LOGI(TAG, "Audio stream aborted by user request");
                        break;
                    }

                    // Jika EOS tercapai atau seluruh frame audio sesuai total durasi sudah selesai dipush ke decoder:
                    if (eos_detected) {
                        ESP_LOGI(TAG, "EOS page processed (%.2fs), concluding network stream instantly",
                                 frames_pushed * 0.060f);
                        need_downshift = false;
                        break;
                    }

                    if (total_seconds > 0 && (frames_pushed * 0.060f) >= (total_seconds + 0.5f)) {
                        ESP_LOGI(TAG, "Song stream fully pushed to decoder (%.2fs >= %ds), finishing network stream",
                                 frames_pushed * 0.060f, total_seconds);
                        need_downshift = false;
                        break;
                    }
                }

                if (eos_detected || (total_seconds > 0 && (frames_pushed * 0.060f) >= (total_seconds + 0.5f))) {
                    need_downshift = false;
                    break;
                }
            }
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "StreamAudio exception: %s", e.what());
        }

        // Flush sisa prebuffer jika stream terputus sebelum mencapai target
        if (!prebuffer.empty()) {
            for (auto& p : prebuffer) {
                app.GetAudioService().PushPacketToDecodeQueue(std::move(p), true);
                frames_pushed++;
                session_frames++;
                int current_sec = (int)(frames_pushed * 0.060f);
                if (current_sec != last_reported_sec) {
                    last_reported_sec = current_sec;
                    if (display != nullptr) {
                        display->SetMusicProgress(current_sec, total_seconds);
                    }
                }
            }
            prebuffer.clear();
        }

        http->Close();

        if (!need_downshift || abort_requested_.load()) {
            break;
        }
    }
    ESP_LOGI(TAG, "Stream finished (total %u bytes read, frames_pushed=%d, aborted=%d)",
             (unsigned int)total_bytes_streamed, frames_pushed, abort_requested_.load());

    bool was_aborted = abort_requested_.load();
    if (!was_aborted) {
        int wait_ms = 0;
        // Tunggu sampai buffer audio terakhir di speaker benar-benar selesai keluar (cek tiap 50ms)
        while (!app.GetAudioService().IsPlaybackIdle() && wait_ms < 6000 && !abort_requested_.load()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait_ms += 50;
        }
    } else {
        app.GetAudioService().ResetDecoder();
    }

    if (display != nullptr) {
        display->StopMusicVisualizer();
        display->SetChatMessage("assistant", "");
        display->SetChatMessage("system", "");
    }

    std::string end_status = was_aborted ? "stopped" : "finished";
    ReportStatus(end_status, video_id);

    is_playing_ = false;

    if (was_aborted) {
        // User sengaja menghentikan lagu -> masuk mode standby hemat daya
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        esp_wifi_set_ps(WIFI_PS_NONE);
        if (display != nullptr) {
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
        }
        app.SetDeviceState(kDeviceStateIdle);
    } else {
        // Lagu selesai normal -> kasih tahu user & langsung buka listening secara instan!
        ESP_LOGI(TAG, "Song playback completed! Notifying user and instantly entering listening mode...");
        if (display != nullptr) {
            display->ShowNotification("Lagu selesai diputar", 2500);
            display->SetChatMessage("assistant", "Lagu selesai diputar.");
            display->SetEmotion("happy");
        }
        // Mainkan notifikasi bunyi lonceng sukses (OGG_SUCCESS)
        app.GetAudioService().PlaySound(Lang::Sounds::OGG_SUCCESS);
        vTaskDelay(pdMS_TO_TICKS(800)); // Beri waktu 800ms agar lonceng sukses berbunyi tuntas

        app.SetDeviceState(kDeviceStateIdle);
        vTaskDelay(pdMS_TO_TICKS(50));
        app.WakeWordInvoke("");
    }
}

void YouTubeAudioPoller::TaskLoop() {
    ESP_LOGI(TAG, "YouTube player ready (on-demand direct playback)");

    while (running_.load()) {
        // Wait for direct tool call (play_requested_), direct query (query_requested_), or trigger (trigger_requested_)
        while (running_.load() && !play_requested_.load() && !query_requested_.load() && !trigger_requested_.load()) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (!running_.load()) {
            break;
        }

        // Direct MCP tool call has highest priority
        if (play_requested_.exchange(false)) {
            std::string vid, tit, dur;
            {
                std::lock_guard<std::mutex> lock(song_mutex_);
                vid = pending_video_id_;
                tit = pending_title_;
                dur = pending_duration_;
            }
            if (!vid.empty()) {
                ESP_LOGI(TAG, "Direct tool call playing: \"%s\" (vid=%s, dur=%s)", tit.c_str(), vid.c_str(), dur.c_str());
                StreamAudio(vid, tit, dur);
            }
        }
        // Direct voice query search
        else if (query_requested_.exchange(false)) {
            std::string q;
            {
                std::lock_guard<std::mutex> lock(song_mutex_);
                q = pending_query_;
            }
            if (!q.empty()) {
                ESP_LOGI(TAG, "Searching & streaming song query: \"%s\"", q.c_str());
                std::string vid, tit, dur;
                if (FetchDirect(q, vid, tit, dur)) {
                    StreamAudio(vid, tit, dur);
                }
            }
        }
        // Fallback queue trigger
        else if (trigger_requested_.exchange(false)) {
            int cmd_id = 0;
            std::string video_id;
            std::string title;
            std::string duration;

            if (FetchCommands(cmd_id, video_id, title, duration)) {
                ESP_LOGI(TAG, "Playing requested YouTube song (1 song only): id=%d, title=\"%s\", vid=%s, dur=%s",
                         cmd_id, title.c_str(), video_id.c_str(), duration.c_str());
                AckCommand(cmd_id);
                StreamAudio(video_id, title, duration);
            }
        }
    }
}
