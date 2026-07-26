#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class HumanFaceDetect;
class HumanFaceRecognizer;

enum class LocalFaceIdentity : uint8_t {
    None,
    Dad,
    Stranger,
};

enum class LocalFaceOperation : uint8_t {
    Idle,
    WaitingForFace,
    MultipleFaces,
    Enrolled,
    TimedOut,
    SampleLimit,
    EnrollFailed,
    Clearing,
    Cleared,
    ClearFailed,
};

struct LocalFaceStatus {
    bool running = false;
    bool camera_online = false;
    bool enrollment_pending = false;
    bool clear_pending = false;
    int enrolled_samples = 0;
    int max_samples = 0;
    int detected_faces = 0;
    LocalFaceIdentity identity = LocalFaceIdentity::None;
    LocalFaceOperation operation = LocalFaceOperation::Idle;
};

class LocalFaceRecognition {
public:
    LocalFaceRecognition();
    ~LocalFaceRecognition();

    bool Start();
    void RequestEnrollDad();
    void RequestClearDad();
    void TestAnnouncement(LocalFaceIdentity identity);
    int EnrolledSampleCount() const { return enrolled_samples_.load(); }
    LocalFaceStatus GetStatus() const;
    bool CopyLatestJpeg(uint8_t*& data, size_t& size);

private:
    enum class Command : uint8_t {
        None,
        EnrollDad,
        ClearDad,
    };

    using Identity = LocalFaceIdentity;

    static void TaskEntry(void* arg);
    void TaskLoop();
    bool InitializeStorageAndModels();
    bool FetchLocalJpeg(uint8_t*& data, size_t& size);
    bool IsAllowedLocalUrl(const char* url) const;
    void ProcessFrame(const uint8_t* jpeg, size_t jpeg_size);
    void UpdatePreview(const uint8_t* jpeg, size_t jpeg_size);
    void ProcessIdentity(Identity identity);
    void ResetPresence();
    void Announce(Identity identity, bool bypass_cooldown = false);
    void Notify(const char* message);

    std::atomic<bool> running_{false};
    std::atomic<Command> command_{Command::None};
    std::atomic<int> enrolled_samples_{0};
    std::atomic<int> detected_faces_{0};
    std::atomic<int64_t> last_camera_success_us_{0};
    std::atomic<int64_t> enroll_deadline_us_{0};
    std::atomic<Identity> confirmed_identity_{Identity::None};
    std::atomic<LocalFaceOperation> operation_{LocalFaceOperation::Idle};
    TaskHandle_t task_handle_ = nullptr;
    HumanFaceDetect* detector_ = nullptr;
    HumanFaceRecognizer* recognizer_ = nullptr;
    SemaphoreHandle_t preview_mutex_ = nullptr;
    uint8_t* latest_jpeg_ = nullptr;
    size_t latest_jpeg_size_ = 0;

    Identity candidate_identity_ = Identity::None;
    Identity present_identity_ = Identity::None;
    int candidate_frames_ = 0;
    int no_face_frames_ = 0;
    int64_t last_dad_announcement_us_ = 0;
    int64_t last_stranger_announcement_us_ = 0;
};
