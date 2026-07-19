#pragma once

#include <atomic>
#include <string>

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class LocalFaceRecognition;

class LocalFaceAdminServer {
public:
    explicit LocalFaceAdminServer(LocalFaceRecognition& face);
    ~LocalFaceAdminServer();

    bool Start();
    void Stop();

private:
    static void StartTaskEntry(void* arg);
    void StartTaskLoop();
    bool StartHttpServer();

    static esp_err_t RootHandler(httpd_req_t* req);
    static esp_err_t StatusHandler(httpd_req_t* req);
    static esp_err_t PreviewHandler(httpd_req_t* req);
    static esp_err_t EnrollHandler(httpd_req_t* req);
    static esp_err_t ClearHandler(httpd_req_t* req);
    static esp_err_t VoiceSettingsHandler(httpd_req_t* req);
    static esp_err_t RebootHandler(httpd_req_t* req);

    bool IsSameSubnetRequest(httpd_req_t* req) const;
    bool HasValidToken(httpd_req_t* req) const;
    esp_err_t SendForbidden(httpd_req_t* req) const;
    esp_err_t SendError(httpd_req_t* req, const char* status, const char* message) const;
    void SetSecurityHeaders(httpd_req_t* req) const;
    std::string BuildPage() const;

    LocalFaceRecognition& face_;
    std::atomic<bool> running_{false};
    TaskHandle_t start_task_handle_ = nullptr;
    httpd_handle_t server_handle_ = nullptr;
    std::string admin_token_;
};
