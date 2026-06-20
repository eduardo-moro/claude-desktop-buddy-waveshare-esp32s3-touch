#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "wifi_server.h"
#include "xfer.h"

struct TamaState
{
  uint8_t sessionsTotal;
  uint8_t sessionsRunning;
  uint8_t sessionsWaiting;
  bool recentlyCompleted;
  uint32_t tokensToday;
  uint32_t lastUpdated;
  char msg[24];
  bool connected;
  char lines[8][92];
  uint8_t nLines;
  uint16_t lineGen; // bumps when lines change — lets UI reset scroll
  // Incoming palette from VS Code theme sync
  uint16_t palBody, palBg, palText, palTextDim, palInk;
  uint16_t palGreen, palHot, palPanel;
  uint16_t paletteGen; // bumps when new palette arrives

  char promptId[40]; // pending permission request ID; empty = no prompt
  char promptTool[20];
  char promptHint[44];
  char mediaLine[96];   // last media event, formatted for terminal
  char mediaStatus[16]; // "Playing" / "Paused" / "Stopped"
  char mediaTitle[64];
  char mediaArtist[48];
  uint32_t mediaPos; // playback position, seconds
  uint32_t mediaDur; // track duration, seconds (0 = unknown)
  uint16_t mediaGen; // bumps on each media update
};

// ---------------------------------------------------------------------------
// Three modes, checked in priority order:
//   demo   → auto-cycle fake scenarios every 8s, ignore live data
//   live   → JSON arrived in the last 10s over USB or BT
//   asleep → no data, all zeros, "No Claude connected"
// ---------------------------------------------------------------------------

static uint32_t _lastLiveMs = 0;
static bool _demoMode = false;
static uint8_t _demoIdx = 0;
static uint32_t _demoNext = 0;

struct _Fake
{
  const char *n;
  uint8_t t, r, w;
  bool c;
  uint32_t tok;
};
static const _Fake _FAKES[] = {
    {"asleep", 0, 0, 0, false, 0},
    {"one idle", 1, 0, 0, false, 12000},
    {"busy", 4, 3, 0, false, 89000},
    {"attention", 2, 1, 1, false, 45000},
    {"completed", 1, 0, 0, true, 142000},
};

inline void dataSetDemo(bool on)
{
  _demoMode = on;
  if (on)
  {
    _demoIdx = 0;
    _demoNext = millis();
  }
}
inline bool dataDemo() { return _demoMode; }

inline bool dataConnected()
{
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}

inline const char *dataScenarioName()
{
  if (_demoMode)
    return _FAKES[_demoIdx].n;
  if (dataConnected())
    return "usb";
  return "none";
}

// Set true once the bridge sends a time sync — until then the RTC may
// hold whatever was on the coin cell (or 2000-01-01 if it lost power).
static bool _rtcValid = false;
inline bool dataRtcValid() { return _rtcValid; }

static void _sendAll(const char *json)
{
  size_t n = strlen(json);
  Serial.write(json, n);
  wifiWrite((const uint8_t *)json, n);
}

static void _applyJson(const char *line, TamaState *out)
{
  JsonDocument doc;
  if (deserializeJson(doc, line))
    return;
  if (xferCommand(doc))
  {
    _lastLiveMs = millis();
    return;
  }

  // Media update from VS Code extension
  const char *mcmd = doc["cmd"] | "";
  if (strcmp(mcmd, "media") == 0)
  {
    const char *status = doc["status"] | "";
    const char *title = doc["title"] | "";
    const char *artist = doc["artist"] | "";
    strncpy(out->mediaStatus, status, sizeof(out->mediaStatus) - 1);
    out->mediaStatus[sizeof(out->mediaStatus) - 1] = 0;
    strncpy(out->mediaTitle, title, sizeof(out->mediaTitle) - 1);
    out->mediaTitle[sizeof(out->mediaTitle) - 1] = 0;
    strncpy(out->mediaArtist, artist, sizeof(out->mediaArtist) - 1);
    out->mediaArtist[sizeof(out->mediaArtist) - 1] = 0;
    out->mediaPos = doc["pos"] | (uint32_t)0;
    out->mediaDur = doc["dur"] | (uint32_t)0;

    const char *label = strcmp(status, "Playing") == 0  ? ">>"
                        : strcmp(status, "Paused") == 0 ? "||"
                                                        : "--";
    if (title[0])
      snprintf(out->mediaLine, sizeof(out->mediaLine), "%s: %s%s%s", label, title, artist[0] ? " - " : "", artist);
    else
      snprintf(out->mediaLine, sizeof(out->mediaLine), "%s", label);
    out->mediaGen++;
    return;
  }

  // Palette sync — store fields in TamaState; main.cpp applies & saves
  const char *pcmd = doc["cmd"] | "";
  if (strcmp(pcmd, "palette") == 0)
  {
    out->palBody = doc["body"] | out->palBody;
    out->palBg = doc["bg"] | out->palBg;
    out->palText = doc["text"] | out->palText;
    out->palTextDim = doc["textDim"] | out->palTextDim;
    out->palInk = doc["ink"] | out->palInk;
    out->palGreen = doc["green"] | out->palGreen;
    out->palHot = doc["hot"] | out->palHot;
    out->palPanel = doc["panel"] | out->palPanel;
    out->paletteGen++;
    _lastLiveMs = millis();
    _sendAll("{\"ack\":\"palette\",\"ok\":true}\n");
    return;
  }

  // WiFi credential update — any channel can send this
  const char *wcmd = doc["cmd"] | "";
  if (strcmp(wcmd, "wifi") == 0)
  {
    const char *ssid = doc["ssid"] | "";
    const char *pass = doc["pass"] | "";
    if (ssid[0])
      wifiSetCredentials(ssid, pass);
    _sendAll("{\"ack\":\"wifi\",\"ok\":true}\n");
    _lastLiveMs = millis();
    return;
  }

  // Bridge sends {"time":[epoch_sec, tz_offset_sec]}; gmtime_r on the
  // adjusted epoch yields local components including weekday.
  JsonArray t = doc["time"];
  if (!t.isNull() && t.size() == 2)
  {
    time_t local = (time_t)t[0].as<uint32_t>() + (int32_t)t[1];
    struct tm lt;
    gmtime_r(&local, &lt);
    // No RTC on this board — just mark valid so UI can show time if needed
    _rtcValid = true;
    _lastLiveMs = millis();
    return;
  }

  out->sessionsTotal = doc["total"] | out->sessionsTotal;
  out->sessionsRunning = doc["running"] | out->sessionsRunning;
  out->sessionsWaiting = doc["waiting"] | out->sessionsWaiting;
  out->recentlyCompleted = doc["completed"] | false;
  uint32_t bridgeTokens = doc["tokens"] | 0;
  if (doc["tokens"].is<uint32_t>())
    statsOnBridgeTokens(bridgeTokens);
  out->tokensToday = doc["tokens_today"] | out->tokensToday;
  const char *m = doc["msg"];
  if (m)
  {
    strncpy(out->msg, m, sizeof(out->msg) - 1);
    out->msg[sizeof(out->msg) - 1] = 0;
  }
  JsonArray la = doc["entries"];
  if (!la.isNull())
  {
    uint8_t n = 0;
    for (JsonVariant v : la)
    {
      if (n >= 8)
        break;
      const char *s = v.as<const char *>();
      strncpy(out->lines[n], s ? s : "", 91);
      out->lines[n][91] = 0;
      n++;
    }
    if (n != out->nLines || (n > 0 && strcmp(out->lines[n - 1], out->msg) != 0))
    {
      out->lineGen++;
    }
    out->nLines = n;
  }
  JsonObject pr = doc["prompt"];
  if (!pr.isNull())
  {
    const char *pid = pr["id"];
    const char *pt = pr["tool"];
    const char *ph = pr["hint"];
    strncpy(out->promptId, pid ? pid : "", sizeof(out->promptId) - 1);
    out->promptId[sizeof(out->promptId) - 1] = 0;
    strncpy(out->promptTool, pt ? pt : "", sizeof(out->promptTool) - 1);
    out->promptTool[sizeof(out->promptTool) - 1] = 0;
    strncpy(out->promptHint, ph ? ph : "", sizeof(out->promptHint) - 1);
    out->promptHint[sizeof(out->promptHint) - 1] = 0;
  }
  else
  {
    out->promptId[0] = 0;
    out->promptTool[0] = 0;
    out->promptHint[0] = 0;
  }
  out->lastUpdated = millis();
  _lastLiveMs = millis();
}

template <size_t N>
struct _LineBuf
{
  char buf[N];
  uint16_t len = 0;
  void feed(Stream &s, TamaState *out)
  {
    while (s.available())
    {
      char c = s.read();
      if (c == '\n' || c == '\r')
      {
        if (len > 0)
        {
          buf[len] = 0;
          if (buf[0] == '{')
            _applyJson(buf, out);
          len = 0;
        }
      }
      else if (len < N - 1)
      {
        buf[len++] = c;
      }
    }
  }
};

static _LineBuf<1024> _usbLine, _wifiLine;

inline void dataPoll(TamaState *out)
{
  uint32_t now = millis();

  if (_demoMode)
  {
    if (now >= _demoNext)
    {
      _demoIdx = (_demoIdx + 1) % 5;
      _demoNext = now + 8000;
    }
    const _Fake &s = _FAKES[_demoIdx];
    out->sessionsTotal = s.t;
    out->sessionsRunning = s.r;
    out->sessionsWaiting = s.w;
    out->recentlyCompleted = s.c;
    out->tokensToday = s.tok;
    out->lastUpdated = now;
    out->connected = true;
    snprintf(out->msg, sizeof(out->msg), "demo: %s", s.n);
    return;
  }

  _usbLine.feed(Serial, out);

  // WiFi TCP ring buffer.
  while (wifiAvailable())
  {
    int c = wifiRead();
    if (c < 0)
      break;
    if (c == '\n' || c == '\r')
    {
      if (_wifiLine.len > 0)
      {
        _wifiLine.buf[_wifiLine.len] = 0;
        if (_wifiLine.buf[0] == '{')
          _applyJson(_wifiLine.buf, out);
        _wifiLine.len = 0;
      }
    }
    else if (_wifiLine.len < sizeof(_wifiLine.buf) - 1)
    {
      _wifiLine.buf[_wifiLine.len++] = (char)c;
    }
  }

  out->connected = dataConnected();
  if (!out->connected)
  {
    out->sessionsTotal = 0;
    out->sessionsRunning = 0;
    out->sessionsWaiting = 0;
    out->recentlyCompleted = false;
    out->lastUpdated = now;
    strncpy(out->msg, "No Claude connected", sizeof(out->msg) - 1);
    out->msg[sizeof(out->msg) - 1] = 0;
  }
}
