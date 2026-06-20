#include "wifi_server.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#define SRV_PORT        37572
#define PIN_TIMEOUT_MS  60000u
#define MAX_ATTEMPTS    3
#define RX_RING_SIZE    2048
#define TASK_STACK      8192
#define NVS_NS          "sinergy"

// ── Persisted config ──────────────────────────────────────────────────────────
static WifiMode  _mode    = WMODE_OFF;
static WifiLogFn _logFn   = nullptr;
static char      _ssid[64]  = {};
static char      _pass[128] = {};
static char      _apSsid[32] = {};
static char      _apPass[16] = {};
static char      _pairedToken[65] = {}; // 64-char hex token, empty = unpaired

// ── Runtime state ─────────────────────────────────────────────────────────────
static char              _ip[24]        = {};
static volatile bool     _up            = false;
static volatile bool     _connectMode   = false; // AP up for credential entry
static volatile bool     _clientConn    = false;
static volatile bool     _clientAuthed  = false;
static volatile uint32_t _pin           = 0;
static volatile uint32_t _pinExpiry     = 0;
static volatile uint8_t  _pinAttempts   = 0;

// ── RX ring buffer ─────────────────────────────────────────────────────────────
static uint8_t           _rxBuf[RX_RING_SIZE];
static volatile uint16_t _rxHead = 0, _rxTail = 0;
static SemaphoreHandle_t _rxMtx;
static SemaphoreHandle_t _txMtx;

static WiFiServer  _tcpSrv(SRV_PORT);
static WiFiClient  _client;
static WebServer*  _webSrv = nullptr;
static DNSServer*  _dnsSrv = nullptr;

// ── Helpers ───────────────────────────────────────────────────────────────────
static void wlog(const char* msg) { if (_logFn) _logFn(msg); }

static void wlogf(const char* fmt, ...) {
  char buf[96]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
  if (_logFn) _logFn(buf);
}

static uint32_t genPin() { return 100000u + (esp_random() % 900000u); }

static void rxPush(uint8_t b) {
  uint16_t next = (_rxHead + 1) % RX_RING_SIZE;
  if (next != _rxTail) { _rxBuf[_rxHead] = b; _rxHead = next; }
}

// ── NVS ───────────────────────────────────────────────────────────────────────
static void loadNvs() {
  Preferences p; p.begin(NVS_NS, true);
  p.getString("ssid",       _ssid,        sizeof(_ssid));
  p.getString("pass",       _pass,        sizeof(_pass));
  p.getString("appass",     _apPass,      sizeof(_apPass));
  p.getString("pairtoken",  _pairedToken, sizeof(_pairedToken));
  bool cm = p.getBool("conmode", false);
  p.end();
  _connectMode = cm;
}

static void savePairedToken(const char* token) {
  strncpy(_pairedToken, token, sizeof(_pairedToken) - 1);
  _pairedToken[sizeof(_pairedToken) - 1] = 0;
  Preferences p; p.begin(NVS_NS, false);
  p.putString("pairtoken", _pairedToken);
  p.end();
}

static void saveStaCreds(const char* ssid, const char* pass) {
  Preferences p; p.begin(NVS_NS, false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
}

static void ensureApPass() {
  if (_apPass[0]) return;
  const char* chars = "abcdefghjkmnpqrstuvwxyz23456789"; // 31 chars, no ambiguous
  uint8_t rnd[8]; esp_fill_random(rnd, sizeof(rnd));
  for (int i = 0; i < 8; i++) _apPass[i] = chars[rnd[i] % 31];
  _apPass[8] = 0;
  Preferences p; p.begin(NVS_NS, false); p.putString("appass", _apPass); p.end();
}

static void buildApSsid() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(_apSsid, sizeof(_apSsid), "Sinergy-%02X%02X", mac[4], mac[5]);
}

// ── HTTP config page (AP mode only) ──────────────────────────────────────────
static const char HTML_FORM[] PROGMEM = R"raw(<!DOCTYPE html><html>
<head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Sinergy Setup</title>
<style>
*{box-sizing:border-box}
body{font-family:sans-serif;max-width:380px;margin:32px auto;padding:0 16px;background:#1d2021;color:#ebdbb2}
h1{color:#fe8019;margin-bottom:4px}h2{color:#a89984;font-size:1rem;margin-top:0}
label{display:block;margin-top:14px;font-size:.85rem;color:#a89984}
input{width:100%;padding:9px 10px;margin-top:4px;background:#3c3836;color:#ebdbb2;
  border:1px solid #504945;border-radius:6px;font-size:1rem}
button{margin-top:20px;width:100%;padding:11px;background:#98971a;color:#1d2021;
  border:none;border-radius:6px;font-size:1rem;cursor:pointer;font-weight:600}
button:active{background:#b8bb26}
</style></head>
<body>
<h1>Sinergy</h1><h2>WiFi Setup</h2>
<form method='POST' action='/save'>
<label>Network (SSID)<input name='ssid' autocomplete='off' placeholder='Your WiFi name'></label>
<label>Password<input type='password' name='pass' placeholder='WiFi password'></label>
<button type='submit'>Save &amp; Connect</button>
</form></body></html>)raw";

static const char HTML_OK[] PROGMEM = R"raw(<!DOCTYPE html><html>
<head><meta charset='utf-8'><title>Sinergy</title>
<style>body{font-family:sans-serif;max-width:380px;margin:32px auto;padding:0 16px;background:#1d2021;color:#ebdbb2}
h1{color:#fe8019}.ok{color:#b8bb26;font-size:1.1rem}</style></head>
<body><h1>Sinergy</h1>
<p class='ok'>Saved! Switch the device to STA mode via the Settings menu to connect.</p>
</body></html>)raw";

static void sendForm() {
  // Captive-portal headers so iOS/Android recognize and auto-open the page.
  _webSrv->sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  _webSrv->send_P(200, "text/html", HTML_FORM);
}

static void startWebServer() {
  if (_dnsSrv) { _dnsSrv->stop(); delete _dnsSrv; }
  _dnsSrv = new DNSServer();
  _dnsSrv->setErrorReplyCode(DNSReplyCode::NoError);
  _dnsSrv->start(53, "*", WiFi.softAPIP());

  if (_webSrv) { _webSrv->stop(); delete _webSrv; }
  _webSrv = new WebServer(80);

  _webSrv->on("/", HTTP_GET, sendForm);

  // Captive portal detection endpoints (iOS, Android, Windows)
  _webSrv->on("/generate_204",           HTTP_GET, sendForm);
  _webSrv->on("/gen_204",                HTTP_GET, sendForm);
  _webSrv->on("/hotspot-detect.html",    HTTP_GET, sendForm);
  _webSrv->on("/connecttest.txt",        HTTP_GET, sendForm);
  _webSrv->on("/ncsi.txt",               HTTP_GET, sendForm);
  _webSrv->on("/redirect",               HTTP_GET, sendForm);
  _webSrv->on("/canonical.html",         HTTP_GET, sendForm);

  _webSrv->on("/save", HTTP_POST, []() {
    String ssid = _webSrv->arg("ssid");
    String pass = _webSrv->arg("pass");
    if (ssid.length() > 0) {
      saveStaCreds(ssid.c_str(), pass.c_str());
      strncpy(_ssid, ssid.c_str(), sizeof(_ssid) - 1);
      strncpy(_pass, pass.c_str(), sizeof(_pass) - 1);
      wlogf("wifi: STA creds saved for '%s', rebooting STA", _ssid);
      _webSrv->send_P(200, "text/html", HTML_OK);
      vTaskDelay(pdMS_TO_TICKS(800));
      // Save STA mode to NVS so device boots into STA next time
      Preferences p; p.begin(NVS_NS, false); p.putUChar("mode", (uint8_t)WMODE_STA); p.end();
      ESP.restart();
    } else {
      _webSrv->send_P(200, "text/html", HTML_OK);
    }
  });
  _webSrv->onNotFound([]() {
    _webSrv->sendHeader("Location", "http://192.168.4.1/");
    _webSrv->send(302);
  });
  _webSrv->begin();
  wlog("wifi: config page -> http://192.168.4.1");
}

// ── TCP handshake ─────────────────────────────────────────────────────────────
static void handleHandshake(WiFiClient& c, const char* line) {
  JsonDocument doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok) return;
  const char* cmd = doc["cmd"] | "";

  // ── Token auth (paired device) ────────────────────────────────────────────
  if (strcmp(cmd, "handshake") == 0 && doc["token"].is<const char*>()) {
    const char* token = doc["token"] | "";
    if (_pairedToken[0] && strcmp(token, _pairedToken) == 0) {
      _clientAuthed = true;
      _pin = 0;
      c.print("{\"ack\":\"handshake\",\"ok\":true,\"method\":\"token\"}\n");
      wlog("wifi: client authenticated via saved token");
    } else {
      // Bad token — fall back to PIN flow
      _pairedToken[0] = 0; // clear bad token, require re-pair
      savePairedToken("");
      char resp[80];
      snprintf(resp, sizeof(resp),
        "{\"ack\":\"handshake\",\"ok\":false,\"error\":\"bad_token\",\"attempts_left\":%d}\n",
        MAX_ATTEMPTS - (int)_pinAttempts);
      c.print(resp);
      wlog("wifi: bad token — PIN required");
    }
    return;
  }

  // ── Save pairing token (sent by extension after successful PIN auth) ───────
  if (strcmp(cmd, "pair") == 0) {
    if (!_clientAuthed) return;
    const char* token = doc["token"] | "";
    if (strlen(token) >= 32) {
      savePairedToken(token);
      c.print("{\"ack\":\"pair\",\"ok\":true}\n");
      wlog("wifi: pairing token saved");
    }
    return;
  }

  // ── PIN auth ──────────────────────────────────────────────────────────────
  if (strcmp(cmd, "handshake") != 0) return;

  const char* pinStr = doc["pin"] | "0";
  uint32_t entered = (uint32_t)atol(pinStr);

  if (entered == _pin && millis() < _pinExpiry) {
    _clientAuthed = true;
    _pin = 0;
    c.print("{\"ack\":\"handshake\",\"ok\":true,\"method\":\"pin\"}\n");
    wlog("wifi: client authenticated via PIN");
  } else {
    _pinAttempts++;
    int left = MAX_ATTEMPTS - (int)_pinAttempts;
    char resp[80];
    snprintf(resp, sizeof(resp),
      "{\"ack\":\"handshake\",\"ok\":false,\"error\":\"invalid_pin\",\"attempts_left\":%d}\n",
      left < 0 ? 0 : left);
    c.print(resp);
    if (_pinAttempts >= MAX_ATTEMPTS) {
      wlog("wifi: max PIN attempts — disconnecting");
      c.stop();
    }
  }
}

// ── Server task ───────────────────────────────────────────────────────────────
static void serverTask(void*) {
  if (_mode == WMODE_STA) {
    if (!_ssid[0]) { wlog("wifi: no STA credentials stored"); vTaskDelete(nullptr); return; }
    wlogf("wifi: connecting to '%s'...", _ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(_ssid, _pass);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
      if (millis() - t0 > 20000) {
        wlog("wifi: connect timeout"); vTaskDelete(nullptr); return;
      }
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    strncpy(_ip, WiFi.localIP().toString().c_str(), sizeof(_ip) - 1);
    _up = true;
    wlogf("wifi: connected  IP %s", _ip);
    wlogf("wifi: TCP server on :%u", SRV_PORT);

  } else { // WMODE_AP
    buildApSsid(); ensureApPass();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(_apSsid, _apPass);
    vTaskDelay(pdMS_TO_TICKS(200)); // let AP settle
    strncpy(_ip, WiFi.softAPIP().toString().c_str(), sizeof(_ip) - 1);
    _up = true;
    wlogf("wifi: AP '%s'", _apSsid);
    wlogf("wifi: pass '%s'", _apPass);
    wlogf("wifi: IP %s  port %u", _ip, SRV_PORT);
    startWebServer();
  }

  _tcpSrv.begin();

  char lineBuf[512];
  int  lineLen = 0;

  for (;;) {
    if (_dnsSrv) _dnsSrv->processNextRequest();
    if (_webSrv) _webSrv->handleClient();

    // Accept new client if none connected
    if (!_client || !_client.connected()) {
      if (_clientConn) {
        _clientConn   = false;
        _clientAuthed = false;
        _pin          = 0;
        lineLen       = 0;
        wlog("wifi: client disconnected");
      }
      WiFiClient c = _tcpSrv.accept();
      if (c) {
        _client       = c;
        _clientConn   = true;
        _clientAuthed = false;
        _pin          = genPin();
        _pinExpiry    = millis() + PIN_TIMEOUT_MS;
        _pinAttempts  = 0;
        lineLen       = 0;
        xSemaphoreTake(_rxMtx, portMAX_DELAY);
        _rxHead = _rxTail = 0;
        xSemaphoreGive(_rxMtx);
        bool paired = _pairedToken[0] != 0;
        char hello[100];
        snprintf(hello, sizeof(hello),
          "{\"cmd\":\"hello\",\"version\":1,\"name\":\"%s\",\"paired\":%s}\n",
          _apSsid[0] ? _apSsid : "Sinergy",
          paired ? "true" : "false");
        _client.print(hello);
        if (paired)
          wlog("wifi: client connected — awaiting token");
        else
          wlog("wifi: client connected — enter PIN");
      } else {
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      }
    }

    // PIN timeout
    if (!_clientAuthed && _pin && millis() > _pinExpiry) {
      wlog("wifi: PIN timeout — disconnecting");
      _client.stop();
      continue;
    }

    // Read incoming bytes
    while (_client.available()) {
      char c = (char)_client.read();
      if (c == '\n' || c == '\r') {
        if (lineLen > 0) {
          lineBuf[lineLen] = 0; lineLen = 0;
          if (!_clientAuthed) {
            handleHandshake(_client, lineBuf);
          } else {
            // Intercept pair command before pushing to RX ring
            JsonDocument pairDoc;
            bool isPair = (deserializeJson(pairDoc, lineBuf) == DeserializationError::Ok)
                          && strcmp(pairDoc["cmd"] | "", "pair") == 0;
            if (isPair) {
              handleHandshake(_client, lineBuf);
            } else {
              xSemaphoreTake(_rxMtx, portMAX_DELAY);
              for (int i = 0; lineBuf[i]; i++) rxPush((uint8_t)lineBuf[i]);
              rxPush('\n');
              xSemaphoreGive(_rxMtx);
            }
          }
        }
      } else if (lineLen < (int)sizeof(lineBuf) - 1) {
        lineBuf[lineLen++] = c;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ── Public API ────────────────────────────────────────────────────────────────
void wifiInit(WifiMode mode, WifiLogFn logFn, bool connectMode) {
  _logFn = logFn;
  _rxMtx = xSemaphoreCreateMutex();
  _txMtx = xSemaphoreCreateMutex();
  loadNvs(); // loads _connectMode from NVS
  _mode = mode;
  // Caller can force connectMode; also respect what was persisted to NVS
  if (connectMode && mode == WMODE_AP) _connectMode = true;
  if (mode != WMODE_AP) _connectMode = false;
  if (mode != WMODE_OFF) {
    xTaskCreatePinnedToCore(serverTask, "wifiSrv", TASK_STACK, nullptr, 1, nullptr, 0);
  }
}

void wifiSetMode(WifiMode mode, bool connectMode) {
  Preferences p; p.begin(NVS_NS, false);
  p.putUChar("mode", (uint8_t)mode);
  p.putBool("conmode", connectMode && (mode == WMODE_AP));
  p.end();
  vTaskDelay(pdMS_TO_TICKS(200));
  ESP.restart();
}

WifiMode    wifiGetMode()          { return _mode; }
bool        wifiConnectMode()      { return _connectMode; }
bool        wifiUp()               { return _up; }
const char* wifiIp()               { return _ip; }
const char* wifiSsid()             { return _mode == WMODE_AP ? _apSsid : _ssid; }
const char* wifiApPass()           { return _apPass; }
bool        wifiClientConnected()  { return _clientConn; }
bool        wifiClientAuthed()     { return _clientAuthed; }
uint32_t    wifiPinCode()          { return _pin; }

void wifiSetCredentials(const char* ssid, const char* pass) {
  saveStaCreds(ssid, pass);
  strncpy(_ssid, ssid, sizeof(_ssid) - 1);
  strncpy(_pass, pass, sizeof(_pass) - 1);
  if (_mode == WMODE_STA && _up) {
    wlogf("wifi: credentials updated — reconnecting to '%s'", ssid);
    WiFi.disconnect();
    _up = false; _ip[0] = 0;
    WiFi.begin(ssid, pass);
  } else {
    wlogf("wifi: STA credentials saved for '%s'", ssid);
  }
}

size_t wifiAvailable() {
  uint16_t h = _rxHead, t = _rxTail;
  return (h >= t) ? h - t : RX_RING_SIZE - t + h;
}

int wifiRead() {
  if (_rxHead == _rxTail) return -1;
  xSemaphoreTake(_rxMtx, portMAX_DELAY);
  int b = _rxBuf[_rxTail];
  _rxTail = (_rxTail + 1) % RX_RING_SIZE;
  xSemaphoreGive(_rxMtx);
  return b;
}

size_t wifiWrite(const uint8_t* data, size_t len) {
  if (!_clientAuthed) return 0;
  xSemaphoreTake(_txMtx, portMAX_DELAY);
  size_t n = _client.write(data, len);
  xSemaphoreGive(_txMtx);
  return n;
}
