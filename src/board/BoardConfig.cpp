#include "board/BoardConfig.h"
#include <Wire.h>
#include <algorithm>

namespace BoardConfig {

namespace {

constexpr uint8_t kTca9554OutputReg = 0x01;
constexpr uint8_t kTca9554ConfigReg = 0x03;
bool gBatteryPowerHoldEnabled = false;

bool tca9554Read(uint8_t reg, uint8_t& value) {
  Wire1.beginTransmission(TCA9554_ADDRESS);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom((uint8_t)TCA9554_ADDRESS, (uint8_t)1) != 1) return false;
  value = Wire1.read();
  return true;
}

bool tca9554Write(uint8_t reg, uint8_t value) {
  Wire1.beginTransmission(TCA9554_ADDRESS);
  Wire1.write(reg);
  Wire1.write(value);
  return Wire1.endTransmission(true) == 0;
}

bool configureTca9554OutputPin(uint8_t pin, bool high) {
  uint8_t output = 0;
  if (!tca9554Read(kTca9554OutputReg, output)) return false;
  const uint8_t mask = (uint8_t)(1U << pin);
  if (high) output |= mask; else output &= (uint8_t)(~mask);
  if (!tca9554Write(kTca9554OutputReg, output)) return false;
  uint8_t config = 0xFF;
  if (!tca9554Read(kTca9554ConfigReg, config)) return false;
  config &= (uint8_t)(~mask);
  return tca9554Write(kTca9554ConfigReg, config);
}

uint8_t batteryPercentForVoltage(float v) {
  struct Point { float v; uint8_t pct; };
  constexpr Point kCurve[] = {
    {3.30f,0},{3.50f,5},{3.60f,10},{3.70f,20},{3.75f,30},
    {3.80f,40},{3.85f,50},{3.90f,60},{3.95f,70},{4.00f,80},{4.10f,90},{4.20f,100}
  };
  constexpr int N = sizeof(kCurve)/sizeof(kCurve[0]);
  if (v <= kCurve[0].v) return kCurve[0].pct;
  if (v >= kCurve[N-1].v) return kCurve[N-1].pct;
  for (int i = 1; i < N; i++) {
    if (v > kCurve[i].v) continue;
    float ratio = (v - kCurve[i-1].v) / (kCurve[i].v - kCurve[i-1].v);
    int pct = (int)(kCurve[i-1].pct + (kCurve[i].pct - kCurve[i-1].pct) * ratio + 0.5f);
    return (uint8_t)std::max(0, std::min(100, pct));
  }
  return 0;
}

} // namespace

void begin() {
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  pinMode(PIN_PWR_BUTTON,  INPUT_PULLUP);
  pinMode(PIN_LCD_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_LCD_BACKLIGHT, LOW);

  Wire1.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire1.setClock(300000);
  Wire1.setTimeOut(10);

  if (configureTca9554OutputPin(TCA9554_PIN_SYS_EN, true)) {
    gBatteryPowerHoldEnabled = true;
  }
  // keep battery ADC gate off by default
  configureTca9554OutputPin(TCA9554_PIN_BATTERY_ADC_ENABLE, true);

  pinMode(PIN_BATTERY_ADC, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
}

bool releaseBatteryPowerHold() {
  return configureTca9554OutputPin(TCA9554_PIN_SYS_EN, false);
}

bool readBatteryStatus(BatteryStatus& status) {
  status = BatteryStatus{};
  configureTca9554OutputPin(TCA9554_PIN_BATTERY_ADC_ENABLE, false);
  delay(3);
  uint32_t mv = 0; uint8_t n = 0;
  for (uint8_t i = 0; i < 8; i++) {
    uint32_t s = analogReadMilliVolts(PIN_BATTERY_ADC);
    if (s > 0) { mv += s; n++; }
    delayMicroseconds(250);
  }
  configureTca9554OutputPin(TCA9554_PIN_BATTERY_ADC_ENABLE, true);
  float pinMv = n ? (float)mv / n : 0;
  status.voltage = pinMv * 0.003f;
  status.present = status.voltage >= 2.5f && status.voltage <= 4.6f;
  if (!status.present) return false;
  status.percent = batteryPercentForVoltage(status.voltage);
  return true;
}

} // namespace BoardConfig
