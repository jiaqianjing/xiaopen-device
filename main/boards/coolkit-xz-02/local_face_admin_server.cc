#include "local_face_admin_server.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

#include "local_face_recognition.h"
#include "settings.h"

namespace {
constexpr char kTag[] = "FaceAdmin";
constexpr uint16_t kAdminPort = 8080;
constexpr uint16_t kControlPort = 32770;
constexpr char kTokenHeader[] = "X-Admin-Token";

const char* IdentityName(LocalFaceIdentity identity) {
    switch (identity) {
        case LocalFaceIdentity::Dad:
            return "dad";
        case LocalFaceIdentity::Stranger:
            return "stranger";
        default:
            return "none";
    }
}

const char* OperationName(LocalFaceOperation operation) {
    switch (operation) {
        case LocalFaceOperation::WaitingForFace: return "waiting_for_face";
        case LocalFaceOperation::MultipleFaces: return "multiple_faces";
        case LocalFaceOperation::Enrolled: return "enrolled";
        case LocalFaceOperation::TimedOut: return "timed_out";
        case LocalFaceOperation::SampleLimit: return "sample_limit";
        case LocalFaceOperation::EnrollFailed: return "enroll_failed";
        case LocalFaceOperation::Clearing: return "clearing";
        case LocalFaceOperation::Cleared: return "cleared";
        case LocalFaceOperation::ClearFailed: return "clear_failed";
        default: return "idle";
    }
}

bool IsValidWakeCommand(const std::string& command) {
    if (command.size() < 3 || command.size() > 63 || command.front() == ' ' ||
        command.back() == ' ') {
        return false;
    }
    bool previous_space = false;
    for (const unsigned char ch : command) {
        if (ch == ' ') {
            if (previous_space) return false;
            previous_space = true;
        } else if (ch >= 'a' && ch <= 'z') {
            previous_space = false;
        } else {
            return false;
        }
    }
    return true;
}

bool IsValidDisplayText(const std::string& text) {
    if (text.empty() || text.size() > 48) return false;
    for (const unsigned char ch : text) {
        if (ch < 0x20 || ch == 0x7f) return false;
    }
    return true;
}
}  // namespace

LocalFaceAdminServer::LocalFaceAdminServer(LocalFaceRecognition& face) : face_(face) {
    std::array<uint8_t, 16> random = {};
    esp_fill_random(random.data(), random.size());
    char token[33] = {};
    for (size_t i = 0; i < random.size(); ++i) {
        std::snprintf(token + i * 2, sizeof(token) - i * 2, "%02x", random[i]);
    }
    admin_token_ = token;
}

LocalFaceAdminServer::~LocalFaceAdminServer() {
    Stop();
}

bool LocalFaceAdminServer::Start() {
    if (running_.exchange(true)) {
        return true;
    }
    const BaseType_t created = xTaskCreate(
        StartTaskEntry, "face_admin_start", 4096, this, 2, &start_task_handle_);
    if (created != pdPASS) {
        running_.store(false);
        start_task_handle_ = nullptr;
        ESP_LOGE(kTag, "Failed to create admin server start task");
        return false;
    }
    return true;
}

void LocalFaceAdminServer::Stop() {
    running_.store(false);
    if (start_task_handle_ != nullptr) {
        vTaskDelete(start_task_handle_);
        start_task_handle_ = nullptr;
    }
    if (server_handle_ != nullptr) {
        httpd_stop(server_handle_);
        server_handle_ = nullptr;
    }
}

void LocalFaceAdminServer::StartTaskEntry(void* arg) {
    static_cast<LocalFaceAdminServer*>(arg)->StartTaskLoop();
}

void LocalFaceAdminServer::StartTaskLoop() {
    while (running_.load()) {
        esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info = {};
        if (station != nullptr && esp_netif_get_ip_info(station, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0) {
            // Give the privacy-critical local camera loop the first chance to
            // establish its LAN connection after DHCP completes.
            vTaskDelay(pdMS_TO_TICKS(4000));
            if (running_.load()) {
                StartHttpServer();
            }
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    start_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

bool LocalFaceAdminServer::StartHttpServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kAdminPort;
    config.ctrl_port = kControlPort;
    config.stack_size = 8192;
    config.max_uri_handlers = 10;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    if (httpd_start(&server_handle_, &config) != ESP_OK) {
        ESP_LOGE(kTag, "Failed to start local admin server");
        server_handle_ = nullptr;
        return false;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = RootHandler, .user_ctx = this},
        {.uri = "/api/status", .method = HTTP_GET, .handler = StatusHandler, .user_ctx = this},
        {.uri = "/api/preview.jpg", .method = HTTP_GET, .handler = PreviewHandler, .user_ctx = this},
        {.uri = "/api/enroll", .method = HTTP_POST, .handler = EnrollHandler, .user_ctx = this},
        {.uri = "/api/clear", .method = HTTP_POST, .handler = ClearHandler, .user_ctx = this},
        {.uri = "/api/voice", .method = HTTP_POST, .handler = VoiceSettingsHandler, .user_ctx = this},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = RebootHandler, .user_ctx = this},
    };
    for (const auto& route : routes) {
        if (httpd_register_uri_handler(server_handle_, &route) != ESP_OK) {
            ESP_LOGE(kTag, "Failed to register route %s", route.uri);
            httpd_stop(server_handle_);
            server_handle_ = nullptr;
            return false;
        }
    }

    esp_netif_ip_info_t ip_info = {};
    esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (station != nullptr && esp_netif_get_ip_info(station, &ip_info) == ESP_OK) {
        ESP_LOGI(kTag, "Local admin page: http://" IPSTR ":%u",
                 IP2STR(&ip_info.ip), kAdminPort);
    }
    return true;
}

bool LocalFaceAdminServer::IsSameSubnetRequest(httpd_req_t* req) const {
    sockaddr_storage peer = {};
    socklen_t peer_length = sizeof(peer);
    const int socket = httpd_req_to_sockfd(req);
    if (socket < 0 || getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &peer_length) != 0) {
        return false;
    }

    uint32_t peer_ip = 0;
    if (peer.ss_family == AF_INET) {
        peer_ip = reinterpret_cast<const sockaddr_in*>(&peer)->sin_addr.s_addr;
    } else if (peer.ss_family == AF_INET6) {
        const auto* peer_v6 = reinterpret_cast<const sockaddr_in6*>(&peer);
        if (!IN6_IS_ADDR_V4MAPPED(&peer_v6->sin6_addr)) {
            return false;
        }
        std::memcpy(&peer_ip, &peer_v6->sin6_addr.s6_addr[12], sizeof(peer_ip));
    } else {
        return false;
    }
    if ((lwip_ntohl(peer_ip) & 0xff000000U) == 0x7f000000U) {
        return true;
    }

    esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {};
    if (station == nullptr || esp_netif_get_ip_info(station, &ip_info) != ESP_OK ||
        ip_info.ip.addr == 0 || ip_info.netmask.addr == 0) {
        return false;
    }
    return (peer_ip & ip_info.netmask.addr) == (ip_info.ip.addr & ip_info.netmask.addr);
}

bool LocalFaceAdminServer::HasValidToken(httpd_req_t* req) const {
    const size_t length = httpd_req_get_hdr_value_len(req, kTokenHeader);
    if (length != admin_token_.size()) {
        return false;
    }
    char value[40] = {};
    return httpd_req_get_hdr_value_str(req, kTokenHeader, value, sizeof(value)) == ESP_OK &&
        admin_token_ == value;
}

void LocalFaceAdminServer::SetSecurityHeaders(httpd_req_t* req) const {
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
        "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
        "connect-src 'self'; img-src 'self'; frame-ancestors 'none'; base-uri 'none'");
}

esp_err_t LocalFaceAdminServer::SendError(
        httpd_req_t* req, const char* status, const char* message) const {
    SetSecurityHeaders(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    char* json = cJSON_PrintUnformatted(root);
    const esp_err_t result = json == nullptr ? ESP_ERR_NO_MEM : httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return result;
}

esp_err_t LocalFaceAdminServer::SendForbidden(httpd_req_t* req) const {
    SetSecurityHeaders(req);
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "仅允许同一局域网内的管理请求");
}

esp_err_t LocalFaceAdminServer::RootHandler(httpd_req_t* req) {
    auto* self = static_cast<LocalFaceAdminServer*>(req->user_ctx);
    if (self == nullptr || !self->IsSameSubnetRequest(req)) {
        return self == nullptr ? ESP_FAIL : self->SendForbidden(req);
    }
    const std::string page = self->BuildPage();
    self->SetSecurityHeaders(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page.data(), page.size());
}

esp_err_t LocalFaceAdminServer::StatusHandler(httpd_req_t* req) {
    auto* self = static_cast<LocalFaceAdminServer*>(req->user_ctx);
    if (self == nullptr || !self->IsSameSubnetRequest(req)) {
        return self == nullptr ? ESP_FAIL : self->SendForbidden(req);
    }
    const LocalFaceStatus status = self->face_.GetStatus();
    const char* pending = status.enrollment_pending ? "enroll" :
        (status.clear_pending ? "clear" : "none");
    Settings voice("voice");
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "running", status.running);
    cJSON_AddBoolToObject(root, "camera_online", status.camera_online);
    cJSON_AddNumberToObject(root, "samples", status.enrolled_samples);
    cJSON_AddNumberToObject(root, "max_samples", status.max_samples);
    cJSON_AddNumberToObject(root, "detected_faces", status.detected_faces);
    cJSON_AddStringToObject(root, "pending", pending);
    cJSON_AddStringToObject(root, "operation", OperationName(status.operation));
    cJSON_AddStringToObject(root, "identity", IdentityName(status.identity));
    cJSON_AddBoolToObject(root, "local_only", true);
    cJSON* voice_json = cJSON_AddObjectToObject(root, "voice");
    const std::string wake_command = voice.GetString("wake_cmd", CONFIG_CUSTOM_WAKE_WORD);
    const std::string wake_text = voice.GetString("wake_text", CONFIG_CUSTOM_WAKE_WORD_DISPLAY);
    const std::string ack_mode = voice.GetString("ack_mode", "voice");
    cJSON_AddStringToObject(voice_json, "wake_command", wake_command.c_str());
    cJSON_AddStringToObject(voice_json, "wake_text", wake_text.c_str());
    cJSON_AddNumberToObject(voice_json, "threshold",
                            voice.GetInt("wake_threshold", CONFIG_CUSTOM_WAKE_WORD_THRESHOLD));
    cJSON_AddStringToObject(voice_json, "ack_mode", ack_mode.c_str());
    cJSON_AddBoolToObject(voice_json, "restart_required", voice.GetBool("restart", false));
    char* json = cJSON_PrintUnformatted(root);
    self->SetSecurityHeaders(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    const esp_err_t result = json == nullptr ? ESP_ERR_NO_MEM : httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return result;
}

esp_err_t LocalFaceAdminServer::PreviewHandler(httpd_req_t* req) {
    auto* self = static_cast<LocalFaceAdminServer*>(req->user_ctx);
    if (self == nullptr || !self->IsSameSubnetRequest(req)) {
        return self == nullptr ? ESP_FAIL : self->SendForbidden(req);
    }
    uint8_t* jpeg = nullptr;
    size_t jpeg_size = 0;
    if (!self->face_.CopyLatestJpeg(jpeg, jpeg_size)) {
        return self->SendError(req, "503 Service Unavailable", "摄像头暂时没有可用画面");
    }
    self->SetSecurityHeaders(req);
    httpd_resp_set_type(req, "image/jpeg");
    const esp_err_t result = httpd_resp_send(
        req, reinterpret_cast<const char*>(jpeg), jpeg_size);
    heap_caps_free(jpeg);
    return result;
}

esp_err_t LocalFaceAdminServer::EnrollHandler(httpd_req_t* req) {
    auto* self = static_cast<LocalFaceAdminServer*>(req->user_ctx);
    if (self == nullptr || !self->IsSameSubnetRequest(req) || !self->HasValidToken(req)) {
        return self == nullptr ? ESP_FAIL : self->SendForbidden(req);
    }
    self->face_.RequestEnrollDad();
    self->SetSecurityHeaders(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"pending\":\"enroll\"}");
}

esp_err_t LocalFaceAdminServer::ClearHandler(httpd_req_t* req) {
    auto* self = static_cast<LocalFaceAdminServer*>(req->user_ctx);
    if (self == nullptr || !self->IsSameSubnetRequest(req) || !self->HasValidToken(req)) {
        return self == nullptr ? ESP_FAIL : self->SendForbidden(req);
    }
    self->face_.RequestClearDad();
    self->SetSecurityHeaders(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"pending\":\"clear\"}");
}

esp_err_t LocalFaceAdminServer::VoiceSettingsHandler(httpd_req_t* req) {
    auto* self = static_cast<LocalFaceAdminServer*>(req->user_ctx);
    if (self == nullptr || !self->IsSameSubnetRequest(req) || !self->HasValidToken(req)) {
        return self == nullptr ? ESP_FAIL : self->SendForbidden(req);
    }
    if (req->content_len == 0 || req->content_len > 512) {
        return self->SendError(req, "400 Bad Request", "语音设置数据长度无效");
    }
    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int read = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (read <= 0) {
            return self->SendError(req, "400 Bad Request", "语音设置数据接收失败");
        }
        received += static_cast<size_t>(read);
    }
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (root == nullptr) {
        return self->SendError(req, "400 Bad Request", "语音设置不是有效 JSON");
    }
    cJSON* command_json = cJSON_GetObjectItem(root, "wake_command");
    cJSON* text_json = cJSON_GetObjectItem(root, "wake_text");
    cJSON* threshold_json = cJSON_GetObjectItem(root, "threshold");
    cJSON* ack_json = cJSON_GetObjectItem(root, "ack_mode");
    const std::string command = cJSON_IsString(command_json) ? command_json->valuestring : "";
    const std::string display_text = cJSON_IsString(text_json) ? text_json->valuestring : "";
    const int threshold = cJSON_IsNumber(threshold_json) ? threshold_json->valueint : -1;
    const std::string ack_mode = cJSON_IsString(ack_json) ? ack_json->valuestring : "";
    const bool valid = IsValidWakeCommand(command) && IsValidDisplayText(display_text) &&
        threshold >= 1 && threshold <= 99 && (ack_mode == "voice" || ack_mode == "tone");
    cJSON_Delete(root);
    if (!valid) {
        return self->SendError(req, "400 Bad Request",
            "唤醒拼音只能包含小写字母和单空格；文字不超过 48 字节；灵敏度为 1–99");
    }

    {
        Settings voice("voice", true);
        voice.SetString("wake_cmd", command);
        voice.SetString("wake_text", display_text);
        voice.SetInt("wake_threshold", threshold);
        voice.SetString("ack_mode", ack_mode);
        voice.SetBool("restart", true);
    }
    self->SetSecurityHeaders(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"restart_required\":true}");
}

esp_err_t LocalFaceAdminServer::RebootHandler(httpd_req_t* req) {
    auto* self = static_cast<LocalFaceAdminServer*>(req->user_ctx);
    if (self == nullptr || !self->IsSameSubnetRequest(req) || !self->HasValidToken(req)) {
        return self == nullptr ? ESP_FAIL : self->SendForbidden(req);
    }
    self->SetSecurityHeaders(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    const esp_err_t response = httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    xTaskCreate([](void*) {
        vTaskDelay(pdMS_TO_TICKS(700));
        esp_restart();
    }, "admin_reboot", 2048, nullptr, 2, nullptr);
    return response;
}

std::string LocalFaceAdminServer::BuildPage() const {
    std::string page = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>小喷一号 · 本地管理</title><style>
:root{color-scheme:dark;--bg:#0e1214;--card:#192023;--line:#303a3f;--green:#76dda7;--muted:#9ba9af;--red:#ff827a;--amber:#ffc66d}
*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at top,#26363c 0,#0e1214 43%);color:#f5f7f7;font:16px/1.5 system-ui,-apple-system,sans-serif}main{width:min(760px,calc(100% - 28px));margin:34px auto 60px}.eyebrow{color:var(--green);font-weight:800;letter-spacing:.13em;font-size:12px}h1{margin:5px 0 4px;font-size:34px}h2{margin:0 0 8px;font-size:21px}p{color:var(--muted)}
.card{background:rgba(25,32,35,.95);border:1px solid var(--line);border-radius:19px;padding:20px;margin:16px 0;box-shadow:0 18px 55px #0005}.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:10px}.item{background:#111719;border-radius:13px;padding:13px}.label,label{color:var(--muted);font-size:13px}.value{font-size:17px;font-weight:760;margin-top:3px}.dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--red);margin-right:7px}.dot.on{background:var(--green);box-shadow:0 0 10px var(--green)}
.preview{position:relative;aspect-ratio:4/3;background:#0b0e10;border-radius:15px;overflow:hidden;border:1px solid var(--line);display:grid;place-items:center}.preview img{width:100%;height:100%;object-fit:contain}.badge{position:absolute;left:10px;bottom:10px;background:#08110dcc;color:var(--green);border:1px solid #4e9870;border-radius:999px;padding:5px 10px;font-size:13px;font-weight:750}.preview-empty{position:absolute;color:var(--muted)}
.actions{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:14px}button,.linkbtn{border:0;border-radius:13px;padding:13px 15px;font:inherit;font-weight:760;cursor:pointer;background:var(--green);color:#102018;text-align:center;text-decoration:none}button.secondary,.linkbtn.secondary{background:#343e43;color:#fff}button.danger{background:#56302f;color:#ffd8d5}button:disabled{opacity:.45;cursor:wait}.notice{min-height:24px;color:var(--green);font-weight:680}.notice.warn{color:var(--amber)}
.form{display:grid;grid-template-columns:1fr 1fr;gap:13px}.field{display:grid;gap:6px}.field.full{grid-column:1/-1}input,select{width:100%;border:1px solid var(--line);border-radius:11px;background:#101517;color:#fff;padding:12px;font:inherit}.hint{font-size:13px;margin:5px 0}.privacy{font-size:14px;border-left:3px solid var(--green);padding-left:13px}code{color:#d7e1e4}@media(max-width:620px){main{margin:22px auto}.grid{grid-template-columns:1fr 1fr}.actions,.form{grid-template-columns:1fr}.field.full{grid-column:auto}h1{font-size:28px}}
</style></head><body><main><div class="eyebrow">LOCAL DEVICE ADMIN</div><h1>小喷一号</h1><p>本地人脸与语音设置 · 仅限当前局域网</p>
<section class="card"><div class="grid"><div class="item"><div class="label">摄像头</div><div class="value"><span id="cameraDot" class="dot"></span><span id="camera">读取中</span></div></div><div class="item"><div class="label">画面人脸</div><div class="value" id="faces">—</div></div><div class="item"><div class="label">爸爸样本</div><div class="value" id="samples">—</div></div><div class="item"><div class="label">当前识别</div><div class="value" id="identity">—</div></div></div></section>
<section class="card"><h2>摄像头预览</h2><p class="hint">这是 S3 识别任务已抓取的最近一帧，不会额外请求摄像头。</p><div class="preview"><span id="previewEmpty" class="preview-empty">等待摄像头画面…</span><img id="preview" alt="本地摄像头预览"><span id="faceBadge" class="badge">等待检测</span></div></section>
<section class="card"><h2>爸爸人脸样本</h2><p>点击录入后有 20 秒对准时间。画面中只能有爸爸一人；保持正脸约 2 秒，成功后样本数会增加。</p><div class="actions"><button id="enroll">录入爸爸样本</button><button id="clear" class="danger">清空全部样本</button></div><p id="faceNotice" class="notice"></p></section>
<section class="card"><h2>本地语音设置</h2><div class="form"><div class="field"><label for="wakeText">显示唤醒词</label><input id="wakeText" maxlength="16" placeholder="小喷小喷"></div><div class="field"><label for="wakeCommand">识别拼音</label><input id="wakeCommand" maxlength="63" placeholder="xiao pen xiao pen" autocapitalize="none"></div><div class="field"><label for="threshold">检测阈值：<strong id="thresholdValue">20</strong>%</label><input id="threshold" type="range" min="1" max="60" value="20"></div><div class="field"><label for="ackMode">唤醒后回应</label><select id="ackMode"><option value="voice">我在呢</option><option value="tone">系统提示音</option></select></div></div><p class="hint">阈值越小越敏感，也越容易误唤醒。中文拼音使用小写字母并以单空格分隔，例如 <code>xiao pen xiao pen</code>。</p><div class="actions"><button id="saveVoice">保存语音设置</button><button id="reboot" class="secondary">重启并应用</button></div><p id="voiceNotice" class="notice"></p></section>
<section class="card"><h2>模型与角色 Prompt</h2><p>对话大模型、角色 Prompt 和云端音色继续在小智控制台管理。小喷一号不保存你的控制台账号、Cookie 或高权限密钥。</p><a class="linkbtn secondary" href="https://xiaozhi.me" target="_blank" rel="noopener noreferrer">打开小智控制台</a></section>
<section class="card privacy"><strong>隐私说明</strong><br>预览来自设备 PSRAM 中的临时 JPEG 缓存，只发送给同网段浏览器；不会写入 Flash、不会上传云端。Flash 中仅保存人脸特征向量。</section>
</main><script>
const token='__ADMIN_TOKEN__',q=s=>document.querySelector(s),identities={none:'未识别',dad:'爸爸',stranger:'陌生人'},operations={idle:'空闲',waiting_for_face:'等待单人脸，请对准摄像头',multiple_faces:'检测到多人，请只保留爸爸',enrolled:'录入成功',timed_out:'录入超时，请重试',sample_limit:'样本已满',enroll_failed:'录入失败',clearing:'正在清空',cleared:'样本已清空',clear_failed:'清空失败'};let voiceLoaded=false,lastOperation='';
async function getStatus(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw Error();const s=await r.json();q('#camera').textContent=s.camera_online?'在线':'离线';q('#cameraDot').className='dot'+(s.camera_online?' on':'');q('#faces').textContent=s.detected_faces+' 张';q('#faceBadge').textContent=s.detected_faces===1?'检测到 1 张脸':s.detected_faces>1?'检测到 '+s.detected_faces+' 张脸':'未检测到人脸';q('#samples').textContent=s.samples+' / '+s.max_samples;q('#identity').textContent=identities[s.identity]||'—';q('#enroll').disabled=!s.camera_online||s.pending!=='none'||s.samples>=s.max_samples;q('#clear').disabled=s.pending!=='none'||s.samples===0;if(!s.camera_online)q('#faceNotice').textContent='S3 暂时无法连接摄像头，录入已禁用';const op=operations[s.operation]||s.operation;if(s.operation!=='idle'&&(s.operation!==lastOperation||s.pending!=='none'))q('#faceNotice').textContent=op;lastOperation=s.operation;if(!voiceLoaded){q('#wakeText').value=s.voice.wake_text;q('#wakeCommand').value=s.voice.wake_command;q('#threshold').value=s.voice.threshold;q('#thresholdValue').textContent=s.voice.threshold;q('#ackMode').value=s.voice.ack_mode;voiceLoaded=true}q('#reboot').disabled=!s.voice.restart_required;if(s.voice.restart_required)q('#voiceNotice').textContent='设置已保存，需要重启后生效';return s}catch(e){q('#camera').textContent='连接失败';q('#cameraDot').className='dot';return null}}
function refreshPreview(){const img=q('#preview');img.src='/api/preview.jpg?t='+Date.now();img.onload=()=>{q('#previewEmpty').style.display='none'};img.onerror=()=>{q('#previewEmpty').style.display='block'}}
async function post(path,body){const options={method:'POST',headers:{'X-Admin-Token':token}};if(body){options.headers['Content-Type']='application/json';options.body=JSON.stringify(body)}const r=await fetch(path,options);const data=await r.json().catch(()=>({}));if(!r.ok)throw Error(data.error||'请求失败');return data}
q('#enroll').onclick=async()=>{q('#faceNotice').textContent='正在启动 20 秒录入窗口…';try{await post('/api/enroll');q('#faceNotice').textContent='已开始，请让爸爸一人正对摄像头';await getStatus()}catch(e){q('#faceNotice').textContent=e.message}};
q('#clear').onclick=async()=>{if(!confirm('确定清空全部爸爸人脸样本吗？'))return;try{await post('/api/clear');q('#faceNotice').textContent='清空指令已提交';await getStatus()}catch(e){q('#faceNotice').textContent=e.message}};
q('#threshold').oninput=e=>q('#thresholdValue').textContent=e.target.value;
q('#saveVoice').onclick=async()=>{const body={wake_text:q('#wakeText').value.trim(),wake_command:q('#wakeCommand').value.trim().toLowerCase().replace(/\s+/g,' '),threshold:Number(q('#threshold').value),ack_mode:q('#ackMode').value};q('#voiceNotice').textContent='正在保存…';try{await post('/api/voice',body);q('#voiceNotice').textContent='已保存，请点击“重启并应用”';q('#reboot').disabled=false}catch(e){q('#voiceNotice').textContent=e.message}};
q('#reboot').onclick=async()=>{if(!confirm('现在重启小喷一号并应用语音设置吗？'))return;try{await post('/api/reboot');q('#voiceNotice').textContent='设备正在重启，页面约 15 秒后恢复';q('#reboot').disabled=true}catch(e){q('#voiceNotice').textContent=e.message}};
getStatus();refreshPreview();setInterval(getStatus,1500);setInterval(refreshPreview,2000);
</script></body></html>)HTML";
    const size_t marker = page.find("__ADMIN_TOKEN__");
    if (marker != std::string::npos) {
        page.replace(marker, std::strlen("__ADMIN_TOKEN__"), admin_token_);
    }
    return page;
}
