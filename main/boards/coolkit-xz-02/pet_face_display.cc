#include "pet_face_display.h"

#include <algorithm>
#include <cstring>

#include <esp_log.h>

namespace {

constexpr char kTag[] = "PetFaceDisplay";
constexpr uint32_t kAnimationPeriodMs = 80;
constexpr uint32_t kDefaultBackground = 0x050A14;
constexpr uint32_t kDefaultAccent = 0x00D8FF;
constexpr uint32_t kFaceColor = 0xF5FBFF;
constexpr uint32_t kPupilColor = 0x07111F;
constexpr uint32_t kCheekColor = 0xFF6FAE;
constexpr uint32_t kTearColor = 0x48DDF8;

bool IsOneOf(const std::string& value,
             std::initializer_list<const char*> candidates) {
    for (const char* candidate : candidates) {
        if (value == candidate) {
            return true;
        }
    }
    return false;
}

}  // namespace

PetFaceDisplay::PetFaceDisplay(esp_lcd_panel_io_handle_t panel_io,
                               esp_lcd_panel_handle_t panel,
                               int width,
                               int height,
                               int offset_x,
                               int offset_y,
                               bool mirror_x,
                               bool mirror_y,
                               bool swap_xy)
    : SpiLcdDisplay(panel_io,
                    panel,
                    width,
                    height,
                    offset_x,
                    offset_y,
                    mirror_x,
                    mirror_y,
                    swap_xy) {
}

PetFaceDisplay::~PetFaceDisplay() {
    if (animation_timer_ != nullptr) {
        lv_timer_delete(animation_timer_);
        animation_timer_ = nullptr;
    }
    if (pet_root_ != nullptr && lv_obj_is_valid(pet_root_)) {
        lv_obj_delete(pet_root_);
        pet_root_ = nullptr;
    }
}

lv_obj_t* PetFaceDisplay::CreateShape(lv_obj_t* parent, lv_color_t color) {
    lv_obj_t* object = lv_obj_create(parent);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(object, color, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    return object;
}

void PetFaceDisplay::ConfigureShape(lv_obj_t* object,
                                    int x,
                                    int y,
                                    int width,
                                    int height,
                                    int rotation) {
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_transform_pivot_x(object, width / 2, 0);
    lv_obj_set_style_transform_pivot_y(object, height / 2, 0);
    lv_obj_set_style_transform_rotation(object, rotation, 0);
}

void PetFaceDisplay::SetVisible(lv_obj_t* object, bool visible) {
    if (visible) {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

void PetFaceDisplay::SetupUI() {
    if (pet_root_ != nullptr) {
        return;
    }

    SpiLcdDisplay::SetupUI();
    DisplayLockGuard lock(this);

    lv_obj_t* screen = lv_screen_active();
    pet_root_ = lv_obj_create(screen);
    lv_obj_remove_flag(pet_root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(pet_root_, 0, 0);
    lv_obj_set_size(pet_root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_pad_all(pet_root_, 0, 0);
    lv_obj_set_style_border_width(pet_root_, 0, 0);
    lv_obj_set_style_radius(pet_root_, 0, 0);
    lv_obj_set_style_bg_color(pet_root_, lv_color_hex(kDefaultBackground), 0);
    lv_obj_set_style_bg_opa(pet_root_, LV_OPA_COVER, 0);

    glow_[0] = CreateShape(pet_root_, lv_color_hex(kDefaultAccent));
    glow_[1] = CreateShape(pet_root_, lv_color_hex(kDefaultAccent));
    ConfigureShape(glow_[0], -32, 38, 112, 112);
    ConfigureShape(glow_[1], 162, 104, 112, 112);
    lv_obj_set_style_bg_opa(glow_[0], LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(glow_[1], LV_OPA_20, 0);

    for (lv_obj_t*& eye : eyes_) {
        eye = CreateShape(pet_root_, lv_color_hex(kFaceColor));
    }
    for (lv_obj_t*& pupil : pupils_) {
        pupil = CreateShape(pet_root_, lv_color_hex(kPupilColor));
    }
    for (lv_obj_t*& stroke : eye_strokes_) {
        stroke = CreateShape(pet_root_, lv_color_hex(kFaceColor));
    }
    for (lv_obj_t*& brow : brows_) {
        brow = CreateShape(pet_root_, lv_color_hex(kFaceColor));
    }
    for (lv_obj_t*& mouth : mouth_strokes_) {
        mouth = CreateShape(pet_root_, lv_color_hex(kFaceColor));
    }

    mouth_o_ = lv_obj_create(pet_root_);
    lv_obj_remove_flag(mouth_o_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(mouth_o_, 0, 0);
    lv_obj_set_style_radius(mouth_o_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(mouth_o_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mouth_o_, 8, 0);
    lv_obj_set_style_border_color(mouth_o_, lv_color_hex(kFaceColor), 0);

    for (lv_obj_t*& cheek : cheeks_) {
        cheek = CreateShape(pet_root_, lv_color_hex(kCheekColor));
        lv_obj_set_style_bg_opa(cheek, LV_OPA_70, 0);
    }
    for (lv_obj_t*& tear : tears_) {
        tear = CreateShape(pet_root_, lv_color_hex(kTearColor));
    }
    sweat_ = CreateShape(pet_root_, lv_color_hex(kTearColor));

    status_overlay_ = lv_label_create(pet_root_);
    lv_obj_set_width(status_overlay_, 190);
    lv_obj_set_style_text_align(status_overlay_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_overlay_, lv_color_hex(0x9BB2C8), 0);
    lv_obj_set_style_text_opa(status_overlay_, LV_OPA_80, 0);
    lv_label_set_long_mode(status_overlay_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(status_overlay_, "");
    lv_obj_align(status_overlay_, LV_ALIGN_TOP_MID, 0, 8);

    ApplyEmotion();
    lv_obj_move_foreground(status_overlay_);

    animation_timer_ =
        lv_timer_create(AnimationTimerCallback, kAnimationPeriodMs, this);
    ESP_LOGI(kTag, "Full-screen procedural pet face initialized");
}

void PetFaceDisplay::SetEmotion(const char* emotion) {
    const char* next_emotion =
        (emotion == nullptr || emotion[0] == '\0') ? "neutral" : emotion;
    if (pet_root_ == nullptr) {
        emotion_ = next_emotion;
        return;
    }

    DisplayLockGuard lock(this);
    emotion_ = next_emotion;
    blink_ticks_ = 0;
    ApplyEmotion();
}

void PetFaceDisplay::SetStatus(const char* status) {
    LvglDisplay::SetStatus(status);
    DisplayLockGuard lock(this);
    status_text_ = status == nullptr ? "" : status;
    if (status_overlay_ != nullptr && notification_ticks_ == 0) {
        lv_label_set_text(status_overlay_, status_text_.c_str());
    }
}

void PetFaceDisplay::ShowNotification(const std::string& notification,
                                      int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void PetFaceDisplay::ShowNotification(const char* notification,
                                      int duration_ms) {
    LvglDisplay::ShowNotification(notification, duration_ms);
    if (status_overlay_ == nullptr) {
        return;
    }

    DisplayLockGuard lock(this);
    lv_label_set_text(status_overlay_, notification == nullptr ? "" : notification);
    notification_ticks_ =
        std::max<uint32_t>(1, static_cast<uint32_t>(duration_ms) / kAnimationPeriodMs);
}

void PetFaceDisplay::SetBackground(uint32_t background, uint32_t accent) {
    lv_obj_set_style_bg_color(pet_root_, lv_color_hex(background), 0);
    lv_obj_set_style_bg_color(glow_[0], lv_color_hex(accent), 0);
    lv_obj_set_style_bg_color(glow_[1], lv_color_hex(accent), 0);
}

void PetFaceDisplay::ResetFace() {
    blinkable_ = true;
    gazeable_ = true;
    pupil_size_ = 20;
    SetBackground(kDefaultBackground, kDefaultAccent);

    for (lv_obj_t* object : eyes_) {
        SetVisible(object, false);
    }
    for (lv_obj_t* object : pupils_) {
        SetVisible(object, false);
    }
    for (lv_obj_t* object : eye_strokes_) {
        SetVisible(object, false);
    }
    for (lv_obj_t* object : brows_) {
        SetVisible(object, false);
        lv_obj_set_style_bg_color(object, lv_color_hex(kFaceColor), 0);
    }
    for (lv_obj_t* object : mouth_strokes_) {
        SetVisible(object, false);
        lv_obj_set_style_bg_color(object, lv_color_hex(kFaceColor), 0);
    }
    SetVisible(mouth_o_, false);
    for (lv_obj_t* object : cheeks_) {
        SetVisible(object, false);
    }
    for (lv_obj_t* object : tears_) {
        SetVisible(object, false);
    }
    SetVisible(sweat_, false);

    ConfigureShape(cheeks_[0], 18, 145, 30, 14);
    ConfigureShape(cheeks_[1], 192, 145, 30, 14);
    ConfigureShape(tears_[0], 55, 126, 11, 34);
    ConfigureShape(tears_[1], 174, 126, 11, 34);
    ConfigureShape(sweat_, 194, 54, 14, 32, -120);
}

void PetFaceDisplay::DrawOpenEyes(int pupil_size) {
    pupil_size_ = pupil_size;
    ConfigureShape(eyes_[0], 32, 55, 64, 78);
    ConfigureShape(eyes_[1], 144, 55, 64, 78);
    ConfigureShape(pupils_[0], 54, 78, pupil_size, pupil_size + 8);
    ConfigureShape(pupils_[1], 166, 78, pupil_size, pupil_size + 8);
    SetVisible(eyes_[0], true);
    SetVisible(eyes_[1], true);
    SetVisible(pupils_[0], true);
    SetVisible(pupils_[1], true);
}

void PetFaceDisplay::DrawClosedEyes(bool happy) {
    blinkable_ = false;
    gazeable_ = false;
    const int left_rotation = happy ? -230 : 0;
    const int right_rotation = happy ? 230 : 0;
    ConfigureShape(eye_strokes_[0], 35, happy ? 82 : 91, 34, 9,
                   left_rotation);
    ConfigureShape(eye_strokes_[1], 65, happy ? 82 : 91, 34, 9,
                   -left_rotation);
    ConfigureShape(eye_strokes_[2], 141, happy ? 82 : 91, 34, 9,
                   right_rotation);
    ConfigureShape(eye_strokes_[3], 171, happy ? 82 : 91, 34, 9,
                   -right_rotation);
    SetVisible(eye_strokes_[0], true);
    SetVisible(eye_strokes_[2], true);
    SetVisible(eye_strokes_[1], happy);
    SetVisible(eye_strokes_[3], happy);
}

void PetFaceDisplay::DrawWink() {
    DrawOpenEyes();
    SetVisible(eyes_[0], false);
    SetVisible(pupils_[0], false);
    ConfigureShape(eye_strokes_[0], 38, 89, 56, 10, -80);
    SetVisible(eye_strokes_[0], true);
    gazeable_ = false;
}

void PetFaceDisplay::DrawSmile() {
    ConfigureShape(mouth_strokes_[0], 76, 167, 48, 10, 210);
    ConfigureShape(mouth_strokes_[1], 116, 167, 48, 10, -210);
    SetVisible(mouth_strokes_[0], true);
    SetVisible(mouth_strokes_[1], true);
}

void PetFaceDisplay::DrawSadMouth() {
    ConfigureShape(mouth_strokes_[0], 76, 178, 48, 10, -210);
    ConfigureShape(mouth_strokes_[1], 116, 178, 48, 10, 210);
    SetVisible(mouth_strokes_[0], true);
    SetVisible(mouth_strokes_[1], true);
}

void PetFaceDisplay::DrawFlatMouth(int width) {
    ConfigureShape(mouth_strokes_[0], (240 - width) / 2, 174, width, 10);
    SetVisible(mouth_strokes_[0], true);
}

void PetFaceDisplay::DrawOMouth() {
    lv_obj_set_pos(mouth_o_, 99, 157);
    lv_obj_set_size(mouth_o_, 42, 52);
    SetVisible(mouth_o_, true);
}

void PetFaceDisplay::SetPupilOffset(int x, int y) {
    const int pupil_x = (64 - pupil_size_) / 2;
    const int pupil_y = (78 - (pupil_size_ + 8)) / 2;
    lv_obj_set_pos(pupils_[0], 32 + pupil_x + x, 55 + pupil_y + y);
    lv_obj_set_pos(pupils_[1], 144 + pupil_x + x, 55 + pupil_y + y);
}

void PetFaceDisplay::ApplyEmotion() {
    ResetFace();

    if (IsOneOf(emotion_, {"happy", "laughing", "funny", "delicious", "loving"})) {
        SetBackground(0x07121B, 0xFF5FA2);
        DrawClosedEyes(true);
        DrawSmile();
        SetVisible(cheeks_[0], true);
        SetVisible(cheeks_[1], true);
        return;
    }

    if (IsOneOf(emotion_, {"sad", "crying"})) {
        SetBackground(0x07101C, 0x3B8FFF);
        DrawOpenEyes(18);
        ConfigureShape(brows_[0], 31, 48, 58, 8, -130);
        ConfigureShape(brows_[1], 151, 48, 58, 8, 130);
        SetVisible(brows_[0], true);
        SetVisible(brows_[1], true);
        DrawSadMouth();
        SetPupilOffset(0, 8);
        if (emotion_ == "crying") {
            SetVisible(tears_[0], true);
            SetVisible(tears_[1], true);
        }
        gazeable_ = false;
        return;
    }

    if (emotion_ == "angry") {
        SetBackground(0x180608, 0xFF3B30);
        DrawOpenEyes(16);
        ConfigureShape(brows_[0], 32, 51, 62, 10, 190);
        ConfigureShape(brows_[1], 146, 51, 62, 10, -190);
        SetVisible(brows_[0], true);
        SetVisible(brows_[1], true);
        DrawFlatMouth(70);
        SetPupilOffset(0, 4);
        gazeable_ = false;
        return;
    }

    if (IsOneOf(emotion_, {"surprised", "shocked"})) {
        SetBackground(0x09091A, 0x9C6BFF);
        DrawOpenEyes(emotion_ == "shocked" ? 10 : 14);
        DrawOMouth();
        gazeable_ = false;
        return;
    }

    if (IsOneOf(emotion_, {"sleepy", "relaxed"})) {
        SetBackground(0x0D0A1A, 0x745CFF);
        DrawClosedEyes(false);
        DrawFlatMouth(32);
        return;
    }

    if (IsOneOf(emotion_, {"thinking", "confused"})) {
        SetBackground(0x061018, 0xF7C948);
        DrawOpenEyes(17);
        ConfigureShape(brows_[0], 34, 45, 56, 8, -100);
        ConfigureShape(brows_[1], 150, 52, 56, 8, 120);
        SetVisible(brows_[0], true);
        SetVisible(brows_[1], true);
        ConfigureShape(mouth_strokes_[0], 94, 175, 52, 10,
                       emotion_ == "confused" ? 110 : -110);
        SetVisible(mouth_strokes_[0], true);
        SetPupilOffset(9, -4);
        SetVisible(sweat_, emotion_ == "confused");
        gazeable_ = false;
        return;
    }

    if (IsOneOf(emotion_, {"winking", "kissy", "silly"})) {
        SetBackground(0x07111A, 0xFF6FAE);
        DrawWink();
        if (emotion_ == "kissy") {
            DrawOMouth();
            lv_obj_set_size(mouth_o_, 30, 24);
            lv_obj_set_pos(mouth_o_, 105, 170);
        } else {
            DrawSmile();
        }
        SetVisible(cheeks_[0], true);
        SetVisible(cheeks_[1], true);
        return;
    }

    if (IsOneOf(emotion_, {"cool", "confident"})) {
        SetBackground(0x040B12, 0x28E0A9);
        DrawClosedEyes(false);
        ConfigureShape(eye_strokes_[0], 30, 78, 67, 16, 70);
        ConfigureShape(eye_strokes_[2], 143, 78, 67, 16, -70);
        DrawSmile();
        return;
    }

    if (emotion_ == "embarrassed") {
        SetBackground(0x120916, 0xFF6FAE);
        DrawOpenEyes(16);
        SetPupilOffset(0, 7);
        DrawFlatMouth(34);
        SetVisible(cheeks_[0], true);
        SetVisible(cheeks_[1], true);
        gazeable_ = false;
        return;
    }

    DrawOpenEyes();
    DrawFlatMouth();
    SetPupilOffset(0, 0);
}

void PetFaceDisplay::AnimationTimerCallback(lv_timer_t* timer) {
    auto* display = static_cast<PetFaceDisplay*>(lv_timer_get_user_data(timer));
    if (display != nullptr) {
        display->Animate();
    }
}

void PetFaceDisplay::Animate() {
    ++animation_tick_;

    if (notification_ticks_ > 0) {
        --notification_ticks_;
        if (notification_ticks_ == 0 && status_overlay_ != nullptr) {
            lv_label_set_text(status_overlay_, status_text_.c_str());
        }
    }

    if (blink_ticks_ > 0) {
        --blink_ticks_;
        if (blink_ticks_ == 0) {
            ApplyEmotion();
        }
        return;
    }

    if (blinkable_ && animation_tick_ % 55 == 0) {
        blink_ticks_ = 2;
        for (lv_obj_t* object : eyes_) {
            SetVisible(object, false);
        }
        for (lv_obj_t* object : pupils_) {
            SetVisible(object, false);
        }
        ConfigureShape(eye_strokes_[0], 35, 91, 64, 9);
        ConfigureShape(eye_strokes_[2], 141, 91, 64, 9);
        SetVisible(eye_strokes_[0], true);
        SetVisible(eye_strokes_[2], true);
        return;
    }

    if (gazeable_ && animation_tick_ % 24 == 0) {
        static constexpr int kGazeOffsets[] = {-7, 0, 7, 0};
        const size_t phase = (animation_tick_ / 24) % 4;
        SetPupilOffset(kGazeOffsets[phase], phase == 3 ? 3 : 0);
    }

    if (IsOneOf(emotion_, {"surprised", "shocked"}) && mouth_o_ != nullptr) {
        const int pulse = ((animation_tick_ / 4) % 2 == 0) ? 0 : 3;
        lv_obj_set_pos(mouth_o_, 99, 157 - pulse);
        lv_obj_set_size(mouth_o_, 42, 52 + pulse * 2);
    }
}
