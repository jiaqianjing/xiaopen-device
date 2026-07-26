#include "local_face_recognition.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <list>
#include <string>
#include <string_view>

#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_spiffs.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <lwip/def.h>
#include <lwip/ip4_addr.h>

#include "application.h"
#include "assets/lang_config.h"
#include "boards/common/board.h"
#include "display/display.h"
#include "dl_image_jpeg.hpp"
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"

namespace {
constexpr char kTag[] = "LocalFace";
extern const char door_dad_prompt_ogg_start[]
    asm("_binary_door_dad_prompt_ogg_start");
extern const char door_dad_prompt_ogg_end[]
    asm("_binary_door_dad_prompt_ogg_end");
extern const char door_stranger_prompt_ogg_start[]
    asm("_binary_door_stranger_prompt_ogg_start");
extern const char door_stranger_prompt_ogg_end[]
    asm("_binary_door_stranger_prompt_ogg_end");

const std::string_view kDadPromptOgg(
    door_dad_prompt_ogg_start,
    static_cast<size_t>(door_dad_prompt_ogg_end -
                        door_dad_prompt_ogg_start));
const std::string_view kStrangerPromptOgg(
    door_stranger_prompt_ogg_start,
    static_cast<size_t>(door_stranger_prompt_ogg_end -
                        door_stranger_prompt_ogg_start));
constexpr char kFaceDbPath[] = "/face/dad.face.db";
constexpr size_t kMaxJpegBytes = 200 * 1024;
constexpr int kConfirmFrames = 2;
constexpr int kClearPresenceFrames = 3;
constexpr int64_t kCameraOnlineWindowUs = 10 * 1000 * 1000;
constexpr int64_t kEnrollmentWindowUs = 20 * 1000 * 1000;

bool EndsWith(const std::string& value, const char* suffix) {
    const size_t suffix_length = std::strlen(suffix);
    return value.size() >= suffix_length &&
        value.compare(value.size() - suffix_length, suffix_length, suffix) == 0;
}
}  // namespace

LocalFaceRecognition::LocalFaceRecognition() {
    preview_mutex_ = xSemaphoreCreateMutex();
}

LocalFaceRecognition::~LocalFaceRecognition() {
    running_.store(false);
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
    delete recognizer_;
    delete detector_;
    if (latest_jpeg_ != nullptr) {
        heap_caps_free(latest_jpeg_);
        latest_jpeg_ = nullptr;
    }
    if (preview_mutex_ != nullptr) {
        vSemaphoreDelete(preview_mutex_);
        preview_mutex_ = nullptr;
    }
}

bool LocalFaceRecognition::Start() {
    if (running_.exchange(true)) {
        return true;
    }
    BaseType_t created = xTaskCreatePinnedToCore(
        TaskEntry, "local_face", 12 * 1024, this, 2, &task_handle_, 0);
    if (created != pdPASS) {
        running_.store(false);
        ESP_LOGE(kTag, "Failed to create local face task");
        return false;
    }
    return true;
}

void LocalFaceRecognition::RequestEnrollDad() {
    enroll_deadline_us_.store(esp_timer_get_time() + kEnrollmentWindowUs);
    operation_.store(LocalFaceOperation::WaitingForFace);
    command_.store(Command::EnrollDad);
    Notify("准备录入爸爸，请正对摄像头");
}

void LocalFaceRecognition::RequestClearDad() {
    operation_.store(LocalFaceOperation::Clearing);
    command_.store(Command::ClearDad);
    Notify("正在清除爸爸的人脸数据");
}

void LocalFaceRecognition::TestAnnouncement(LocalFaceIdentity identity) {
    if (identity == LocalFaceIdentity::Dad) {
        Announce(Identity::Dad, true);
    } else if (identity == LocalFaceIdentity::Stranger) {
        Announce(Identity::Stranger, true);
    }
}

LocalFaceStatus LocalFaceRecognition::GetStatus() const {
    const Command command = command_.load();
    const int64_t last_success = last_camera_success_us_.load();
    const int64_t now = esp_timer_get_time();
    return {
        .running = running_.load(),
        .camera_online = last_success != 0 && now - last_success < kCameraOnlineWindowUs,
        .enrollment_pending = command == Command::EnrollDad,
        .clear_pending = command == Command::ClearDad,
        .enrolled_samples = enrolled_samples_.load(),
        .max_samples = CONFIG_LOCAL_FACE_MAX_DAD_SAMPLES,
        .detected_faces = detected_faces_.load(),
        .identity = confirmed_identity_.load(),
        .operation = operation_.load(),
    };
}

bool LocalFaceRecognition::CopyLatestJpeg(uint8_t*& data, size_t& size) {
    data = nullptr;
    size = 0;
    if (preview_mutex_ == nullptr || xSemaphoreTake(preview_mutex_, pdMS_TO_TICKS(250)) != pdTRUE) {
        return false;
    }
    if (latest_jpeg_ != nullptr && latest_jpeg_size_ > 0) {
        data = static_cast<uint8_t*>(heap_caps_malloc(
            latest_jpeg_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (data != nullptr) {
            std::memcpy(data, latest_jpeg_, latest_jpeg_size_);
            size = latest_jpeg_size_;
        }
    }
    xSemaphoreGive(preview_mutex_);
    return data != nullptr;
}

void LocalFaceRecognition::TaskEntry(void* arg) {
    static_cast<LocalFaceRecognition*>(arg)->TaskLoop();
}

void LocalFaceRecognition::TaskLoop() {
    if (!IsAllowedLocalUrl(CONFIG_LOCAL_FACE_CAMERA_URL)) {
        ESP_LOGE(kTag, "Rejected non-local camera URL: %s", CONFIG_LOCAL_FACE_CAMERA_URL);
        Notify("摄像头地址不是局域网地址，已拒绝");
        running_.store(false);
        task_handle_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    if (!InitializeStorageAndModels()) {
        Notify("本地人脸识别初始化失败");
        running_.store(false);
        task_handle_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(kTag, "Local-only recognition started; camera=%s samples=%d",
             CONFIG_LOCAL_FACE_CAMERA_URL, enrolled_samples_.load());
    if (enrolled_samples_.load() == 0) {
        Notify("请双击中间键录入爸爸");
    }

    while (running_.load()) {
        const Command command = command_.load();
        if (command == Command::EnrollDad &&
            esp_timer_get_time() >= enroll_deadline_us_.load()) {
            command_.store(Command::None);
            operation_.store(LocalFaceOperation::TimedOut);
            Notify("录入超时，请在预览中对准后重试");
        }
        if (command == Command::ClearDad) {
            command_.store(Command::None);
            if (recognizer_->clear_all_feats() == ESP_OK) {
                enrolled_samples_.store(0);
                ResetPresence();
                operation_.store(LocalFaceOperation::Cleared);
                Notify("爸爸的人脸数据已清除");
            } else {
                operation_.store(LocalFaceOperation::ClearFailed);
                Notify("清除人脸数据失败");
            }
        }

        wifi_ap_record_t access_point = {};
        if (esp_wifi_sta_get_ap_info(&access_point) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info = {};
        if (station == nullptr || esp_netif_get_ip_info(station, &ip_info) != ESP_OK ||
            ip_info.ip.addr == 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // The camera is another low-power Wi-Fi station on the same LAN.
        // Modem sleep can make direct station-to-station TCP connects time out
        // on some home APs, so keep Wi-Fi awake while local recognition runs.
        wifi_ps_type_t power_save = WIFI_PS_MIN_MODEM;
        if (esp_wifi_get_ps(&power_save) == ESP_OK && power_save != WIFI_PS_NONE) {
            esp_wifi_set_ps(WIFI_PS_NONE);
        }

        uint8_t* jpeg = nullptr;
        size_t jpeg_size = 0;
        if (FetchLocalJpeg(jpeg, jpeg_size)) {
            last_camera_success_us_.store(esp_timer_get_time());
            UpdatePreview(jpeg, jpeg_size);
            ProcessFrame(jpeg, jpeg_size);
            heap_caps_free(jpeg);
        } else {
            // Treat a transient camera/network miss like one no-face frame so
            // that a single dropped JPEG cannot retrigger an announcement.
            ProcessIdentity(Identity::None);
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_LOCAL_FACE_POLL_INTERVAL_MS));
    }

    task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

void LocalFaceRecognition::UpdatePreview(const uint8_t* jpeg, size_t jpeg_size) {
    if (preview_mutex_ == nullptr || jpeg == nullptr || jpeg_size == 0) {
        return;
    }
    uint8_t* copy = static_cast<uint8_t*>(heap_caps_malloc(
        jpeg_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        return;
    }
    std::memcpy(copy, jpeg, jpeg_size);
    if (xSemaphoreTake(preview_mutex_, pdMS_TO_TICKS(250)) != pdTRUE) {
        heap_caps_free(copy);
        return;
    }
    uint8_t* previous = latest_jpeg_;
    latest_jpeg_ = copy;
    latest_jpeg_size_ = jpeg_size;
    xSemaphoreGive(preview_mutex_);
    if (previous != nullptr) {
        heap_caps_free(previous);
    }
}

bool LocalFaceRecognition::InitializeStorageAndModels() {
    esp_vfs_spiffs_conf_t storage = {};
    storage.base_path = "/face";
    storage.partition_label = "face_db";
    storage.max_files = 3;
    storage.format_if_mount_failed = true;
    const esp_err_t result = esp_vfs_spiffs_register(&storage);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Failed to mount face_db: %s", esp_err_to_name(result));
        return false;
    }

    detector_ = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, true);
    recognizer_ = new HumanFaceRecognizer(
        kFaceDbPath, HumanFaceFeat::MFN_S8_V1, true);
    enrolled_samples_.store(recognizer_->get_num_feats());
    return true;
}

bool LocalFaceRecognition::IsAllowedLocalUrl(const char* url) const {
    if (url == nullptr) {
        return false;
    }
    std::string value(url);
    constexpr char scheme[] = "http://";
    if (value.rfind(scheme, 0) != 0) {
        return false;
    }

    const size_t authority_start = sizeof(scheme) - 1;
    const size_t authority_end = value.find('/', authority_start);
    std::string authority = value.substr(
        authority_start, authority_end == std::string::npos ? std::string::npos : authority_end - authority_start);
    if (authority.empty() || authority.find('@') != std::string::npos || authority.front() == '[') {
        return false;
    }

    const size_t port_separator = authority.rfind(':');
    if (port_separator != std::string::npos) {
        const std::string port = authority.substr(port_separator + 1);
        if (port.empty() || !std::all_of(port.begin(), port.end(), [](unsigned char ch) {
                return std::isdigit(ch) != 0;
            })) {
            return false;
        }
        const long port_number = std::strtol(port.c_str(), nullptr, 10);
        if (port_number < 1 || port_number > 65535) {
            return false;
        }
        authority.resize(port_separator);
    }

    std::transform(authority.begin(), authority.end(), authority.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (authority == "localhost" || EndsWith(authority, ".local")) {
        return true;
    }

    ip4_addr_t address = {};
    if (!ip4addr_aton(authority.c_str(), &address)) {
        return false;
    }
    const uint32_t ip = lwip_ntohl(ip4_addr_get_u32(&address));
    return (ip & 0xff000000U) == 0x0a000000U ||
           (ip & 0xfff00000U) == 0xac100000U ||
           (ip & 0xffff0000U) == 0xc0a80000U ||
           (ip & 0xff000000U) == 0x7f000000U ||
           (ip & 0xffff0000U) == 0xa9fe0000U;
}

bool LocalFaceRecognition::FetchLocalJpeg(uint8_t*& data, size_t& size) {
    data = nullptr;
    size = 0;

    esp_http_client_config_t config = {};
    config.url = CONFIG_LOCAL_FACE_CAMERA_URL;
    config.timeout_ms = 4000;
    config.buffer_size = 4096;
    config.disable_auto_redirect = true;
    config.transport_type = HTTP_TRANSPORT_OVER_TCP;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }

    bool success = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        const int64_t header_length = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        const int64_t content_length = esp_http_client_get_content_length(client);
        if (header_length >= 0 && status == 200 &&
            (content_length < 0 || content_length <= static_cast<int64_t>(kMaxJpegBytes))) {
            const size_t capacity = content_length > 0 ? static_cast<size_t>(content_length) : kMaxJpegBytes;
            data = static_cast<uint8_t*>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (data != nullptr) {
                while (size < capacity) {
                    const int read = esp_http_client_read(
                        client, reinterpret_cast<char*>(data + size), capacity - size);
                    if (read < 0) {
                        break;
                    }
                    if (read == 0) {
                        success = size > 0;
                        break;
                    }
                    size += static_cast<size_t>(read);
                }
                if (content_length >= 0) {
                    success = size == static_cast<size_t>(content_length);
                }
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!success && data != nullptr) {
        heap_caps_free(data);
        data = nullptr;
        size = 0;
    }
    return success;
}

void LocalFaceRecognition::ProcessFrame(const uint8_t* jpeg, size_t jpeg_size) {
    const dl::image::jpeg_img_t encoded = {
        .data = const_cast<uint8_t*>(jpeg),
        .data_len = jpeg_size,
    };
    dl::image::img_t image = dl::image::sw_decode_jpeg(
        encoded, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
    if (image.data == nullptr) {
        ESP_LOGW(kTag, "JPEG decode failed");
        return;
    }

    std::list<dl::detect::result_t> detections = detector_->run(image);
    detected_faces_.store(static_cast<int>(detections.size()));
    const Command command = command_.load();
    if (command == Command::EnrollDad) {
        if (enrolled_samples_.load() >= CONFIG_LOCAL_FACE_MAX_DAD_SAMPLES) {
            command_.store(Command::None);
            operation_.store(LocalFaceOperation::SampleLimit);
            Notify("爸爸样本已满，请先双击减号键清除");
        } else if (detections.empty()) {
            operation_.store(LocalFaceOperation::WaitingForFace);
        } else if (detections.size() != 1) {
            if (operation_.exchange(LocalFaceOperation::MultipleFaces) !=
                LocalFaceOperation::MultipleFaces) {
                Notify("请只让爸爸一人出现在画面中");
            }
        } else if (recognizer_->enroll(image, detections) == ESP_OK) {
            command_.store(Command::None);
            const int count = recognizer_->get_num_feats();
            enrolled_samples_.store(count);
            operation_.store(LocalFaceOperation::Enrolled);
            Notify(("爸爸样本已录入：" + std::to_string(count) + "/" +
                    std::to_string(CONFIG_LOCAL_FACE_MAX_DAD_SAMPLES)).c_str());
        } else {
            command_.store(Command::None);
            operation_.store(LocalFaceOperation::EnrollFailed);
            Notify("爸爸样本录入失败");
        }
        heap_caps_free(image.data);
        return;
    }

    if (detections.empty()) {
        ProcessIdentity(Identity::None);
    } else if (enrolled_samples_.load() == 0) {
        ResetPresence();
    } else {
        bool dad_found = false;
        for (const auto& detection : detections) {
            std::list<dl::detect::result_t> one_face = {detection};
            if (!recognizer_->recognize(image, one_face).empty()) {
                dad_found = true;
                break;
            }
        }
        ProcessIdentity(dad_found ? Identity::Dad : Identity::Stranger);
    }
    heap_caps_free(image.data);
}

void LocalFaceRecognition::ProcessIdentity(Identity identity) {
    if (identity == Identity::None) {
        if (++no_face_frames_ >= kClearPresenceFrames) {
            ResetPresence();
        }
        return;
    }

    no_face_frames_ = 0;
    if (candidate_identity_ != identity) {
        candidate_identity_ = identity;
        candidate_frames_ = 1;
        return;
    }
    if (candidate_frames_ < kConfirmFrames) {
        ++candidate_frames_;
    }
    if (candidate_frames_ >= kConfirmFrames && present_identity_ != identity) {
        present_identity_ = identity;
        confirmed_identity_.store(identity);
        Announce(identity);
    }
}

void LocalFaceRecognition::ResetPresence() {
    candidate_identity_ = Identity::None;
    present_identity_ = Identity::None;
    candidate_frames_ = 0;
    no_face_frames_ = 0;
    confirmed_identity_.store(Identity::None);
}

void LocalFaceRecognition::Announce(Identity identity, bool bypass_cooldown) {
    const int64_t now = esp_timer_get_time();
    int64_t* last_announcement = identity == Identity::Dad
        ? &last_dad_announcement_us_ : &last_stranger_announcement_us_;
    const int64_t cooldown_us = static_cast<int64_t>(CONFIG_LOCAL_FACE_ANNOUNCE_COOLDOWN_MS) * 1000;
    if (!bypass_cooldown && *last_announcement != 0 &&
        now - *last_announcement < cooldown_us) {
        return;
    }
    *last_announcement = now;

    auto& app = Application::GetInstance();
    app.Schedule([identity]() {
        auto& scheduled_app = Application::GetInstance();
        auto* display = Board::GetInstance().GetDisplay();
        if (identity == Identity::Dad) {
            display->SetEmotion("happy");
            display->ShowNotification("认出爸爸了，正在想一句俏皮话…", 5000);
            scheduled_app.SendAudioPrompt(kDadPromptOgg,
                                          Lang::Sounds::OGG_DAD_HOME);
        } else {
            display->SetEmotion("shocked");
            display->ShowNotification("发现陌生人，正在组织警戒台词…", 5000);
            scheduled_app.SendAudioPrompt(kStrangerPromptOgg,
                                          Lang::Sounds::OGG_STRANGER_ALERT);
        }
    });
}

void LocalFaceRecognition::Notify(const char* message) {
    const std::string copy = message == nullptr ? "" : message;
    Application::GetInstance().Schedule([copy]() {
        Board::GetInstance().GetDisplay()->ShowNotification(copy, 4000);
    });
}
