#pragma once
#include <Arduino.h>

namespace BoardConfig {

constexpr int PIN_BOOT_BUTTON = 0;   // Brightness button → BtnA (confirm)
constexpr int PIN_PWR_BUTTON  = 16;  // Power button     → BtnB (deny)
constexpr int PIN_BATTERY_ADC = 4;

constexpr int PIN_LCD_CS       = 9;
constexpr int PIN_LCD_SCLK     = 10;
constexpr int PIN_LCD_DATA0    = 11;
constexpr int PIN_LCD_DATA1    = 12;
constexpr int PIN_LCD_DATA2    = 13;
constexpr int PIN_LCD_DATA3    = 14;
constexpr int PIN_LCD_RST      = 21;
constexpr int PIN_LCD_BACKLIGHT = 8;

constexpr int PANEL_NATIVE_WIDTH  = 172;
constexpr int PANEL_NATIVE_HEIGHT = 640;
constexpr int DISPLAY_WIDTH       = 640;
constexpr int DISPLAY_HEIGHT      = 172;

constexpr int PIN_I2C_SDA   = 47;
constexpr int PIN_I2C_SCL   = 48;

constexpr int PIN_TOUCH_SDA = 17;
constexpr int PIN_TOUCH_SCL = 18;

constexpr bool UI_ROTATED_180 = true;
enum class UiOrientation : uint8_t {
  Portrait,
  PortraitFlipped,
  Landscape,
  LandscapeFlipped,
};

constexpr int    TCA9554_ADDRESS           = 0x20;
constexpr uint8_t TCA9554_PIN_BATTERY_ADC_ENABLE = 1;
constexpr uint8_t TCA9554_PIN_SYS_EN      = 6;

void begin();
bool releaseBatteryPowerHold();

struct BatteryStatus {
  bool    present = false;
  float   voltage = 0.0f;
  uint8_t percent = 0;
};
bool readBatteryStatus(BatteryStatus& status);

} // namespace BoardConfig
