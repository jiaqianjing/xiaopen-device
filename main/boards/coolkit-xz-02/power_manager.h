#pragma once

// XZ-02 has the same charging input and ADC battery divider as the official
// Xingzhi 1.54 Wi-Fi board. Keep its proven power-management implementation
// as the hardware baseline while this board adds its own application logic.
#include "../xingzhi-cube-1.54tft-wifi/power_manager.h"
