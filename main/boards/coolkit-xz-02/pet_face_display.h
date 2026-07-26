#pragma once

#include "display/lcd_display.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>

class PetFaceDisplay : public SpiLcdDisplay {
public:
    PetFaceDisplay(esp_lcd_panel_io_handle_t panel_io,
                   esp_lcd_panel_handle_t panel,
                   int width,
                   int height,
                   int offset_x,
                   int offset_y,
                   bool mirror_x,
                   bool mirror_y,
                   bool swap_xy);
    ~PetFaceDisplay() override;

    void SetupUI() override;
    void SetEmotion(const char* emotion) override;
    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override;

private:
    static void AnimationTimerCallback(lv_timer_t* timer);

    lv_obj_t* CreateShape(lv_obj_t* parent, lv_color_t color);
    void ConfigureShape(lv_obj_t* object,
                        int x,
                        int y,
                        int width,
                        int height,
                        int rotation = 0);
    void SetVisible(lv_obj_t* object, bool visible);
    void ResetFace();
    void ApplyEmotion();
    void DrawOpenEyes(int pupil_size = 20);
    void DrawClosedEyes(bool happy);
    void DrawWink();
    void DrawSmile();
    void DrawSadMouth();
    void DrawFlatMouth(int width = 56);
    void DrawOMouth();
    void SetPupilOffset(int x, int y);
    void SetBackground(uint32_t background, uint32_t accent);
    void Animate();

    lv_obj_t* pet_root_ = nullptr;
    lv_obj_t* status_overlay_ = nullptr;
    std::array<lv_obj_t*, 2> glow_ = {};
    std::array<lv_obj_t*, 2> eyes_ = {};
    std::array<lv_obj_t*, 2> pupils_ = {};
    std::array<lv_obj_t*, 4> eye_strokes_ = {};
    std::array<lv_obj_t*, 2> brows_ = {};
    std::array<lv_obj_t*, 2> mouth_strokes_ = {};
    lv_obj_t* mouth_o_ = nullptr;
    std::array<lv_obj_t*, 2> cheeks_ = {};
    std::array<lv_obj_t*, 2> tears_ = {};
    lv_obj_t* sweat_ = nullptr;

    lv_timer_t* animation_timer_ = nullptr;
    std::string emotion_ = "neutral";
    std::string status_text_;
    uint32_t notification_ticks_ = 0;
    uint32_t animation_tick_ = 0;
    uint8_t blink_ticks_ = 0;
    bool blinkable_ = true;
    bool gazeable_ = true;
    int pupil_size_ = 20;
};
