#pragma once
#include <stdint.h>
#include <stddef.h>

// WiFi connectivity for the Sinergy app.
// Two modes: STA (joins an existing network) and AP (hosts its own network).
// In AP mode a minimal HTTP server at 192.168.4.1 lets a phone configure STA
// credentials without needing the VS Code extension.
//
// A TCP server on port 37572 (both modes) accepts one client at a time and
// requires a 6-digit PIN handshake before data flows — PIN is shown on screen.
//
// Wire protocol: UTF-8 JSON, one object per line (\n), same as the BLE bridge.

enum WifiMode : uint8_t { WMODE_OFF = 0, WMODE_STA = 1, WMODE_AP = 2 };

typedef void (*WifiLogFn)(const char*);

// Call once at boot. Starts the background FreeRTOS task when mode != OFF.
// connectMode=true: AP is up for credential entry (show password prominently, reboot STA after save).
void wifiInit(WifiMode mode, WifiLogFn logFn, bool connectMode = false);

// Change mode and restart the device to apply (saves to NVS first).
void wifiSetMode(WifiMode mode, bool connectMode = false);
WifiMode wifiGetMode();

// Store STA credentials in NVS and trigger reconnect (STA mode only).
// Also accepted via {"cmd":"wifi","ssid":"...","pass":"..."} over any channel.
void wifiSetCredentials(const char* ssid, const char* pass);

// ── Network state ─────────────────────────────────────────────────────────────
bool        wifiUp();           // true when IP is available (STA connected / AP started)
const char* wifiIp();           // current IP string, empty if not up
const char* wifiSsid();         // connected SSID (STA) or AP name
const char* wifiApPass();       // AP password — shown on screen so user can join
bool        wifiConnectMode();  // true when AP is up for credential setup (show password prominently)

// ── TCP client state ──────────────────────────────────────────────────────────
bool     wifiClientConnected();
bool     wifiClientAuthed();
uint32_t wifiPinCode();    // non-zero while PIN is pending (show on screen)

// ── RX ring buffer — drained by main thread, same pattern as BLE ──────────────
size_t wifiAvailable();
int    wifiRead();          // returns -1 if empty
size_t wifiWrite(const uint8_t* data, size_t len);
