#include <Arduino.h>
#include <LittleFS.h>
#include <stdarg.h>

#include "board/BoardConfig.h"
#include "input/ButtonHandler.h"
#include "input/TouchHandler.h"
#include "display/BuddyDisplay.h"
#include "wifi_server.h"
#include "data.h"
#include "buddy.h"
#include "display/axs15231b.h"

// U8g2 fonts (data only — no U8g2 display driver used)
#include <U8g2lib.h>
// Small: 5×7 full Latin (ã ç ó etc.)
// Medium: 7×13 full Latin
// Symbols fallback: 7×13 symbols (·, ✻, ✽, ✶, ✳, ✢ and more)
#define FONT_SM u8g2_font_5x7_tf
#define FONT_MD u8g2_font_7x13_tf
#define FONT_SYM u8g2_font_7x13_t_symbols

// Helper: set small or medium U8g2 font + symbols fallback, clear textSize
#define UI_FONT_SM()               \
  do                               \
  {                                \
    spr.setFont(FONT_SM);          \
    spr.setFontFallback(FONT_SYM); \
    spr.setTextSize(1);            \
  } while (0)
#define UI_FONT_MD()               \
  do                               \
  {                                \
    spr.setFont(FONT_MD);          \
    spr.setFontFallback(FONT_SYM); \
    spr.setTextSize(1);            \
  } while (0)
#define UI_FONT_OFF() \
  do                  \
  {                   \
    spr.clearFont();  \
  } while (0)

// ── Character / GIF engine ────────────────────────────────────────────────────
#include "character.h"

static bool gifAvailable = false; // true when a GIF character is installed
static bool buddyMode = true;     // true = ASCII sprites, false = GIF character
// uiPal() / uiGreen() / uiHot() / uiPanel() / uiInk() come from palette.h

// Cycle through ASCII species, wrapping back to GIF mode when available
static void cycleSpecies()
{
  uint8_t n = buddySpeciesCount();
  if (!buddyMode)
  {
    buddyMode = true;
    buddySetSpeciesIdx(0);
  }
  else if (buddySpeciesIdx() + 1 >= n && gifAvailable)
  {
    buddyMode = false;
    speciesIdxSave(0xFF); // sentinel: GIF mode
  }
  else
  {
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode)
    buddyInvalidate();
}

// ── Stats / settings / palette (header-only, included once here) ─────────────
#include "stats.h"
#include "palette.h"
#include "xfer.h"

// ── Display layout ───────────────────────────────────────────────────────────
static constexpr int W = BuddyDisplay::W; // 640
static constexpr int H = BuddyDisplay::H; // 172
static constexpr int BZ = 172;            // buddy zone (square: H×H)
static constexpr int TXZ = 176;           // text zone x start (BZ + 4px divider)
static constexpr int TW = W - TXZ;        // text zone width (464px)

// uiHot() / uiPanel() / uiGreen() are now dynamic — use uiHot(), uiPanel(), uiGreen()

// ── Persona ───────────────────────────────────────────────────────────────────
enum PersonaState
{
  P_SLEEP,
  P_IDLE,
  P_BUSY,
  P_ATTENTION,
  P_CELEBRATE,
  P_DIZZY,
  P_HEART
};
static const char *stateNames[] = {"sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart"};

TamaState tama;
PersonaState baseState = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t oneShotUntil = 0;
unsigned long t = 0;

// ── UI state ──────────────────────────────────────────────────────────────────
bool menuOpen = false;
uint8_t menuSel = 0;
uint8_t brightLevel = 3; // 0..4

enum DisplayMode
{
  DISP_NORMAL,
  DISP_PET,
  DISP_INFO,
  DISP_COUNT,
  DISP_APP,
};
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
const uint8_t INFO_PAGES = 6;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
uint32_t touchGuardUntil = 0; // ignore touch End events until this timestamp
bool screenOff = false;
bool responseSent = false;
bool swallowA = false;
bool swallowB = false;

static uint32_t screenOffMs()
{
  uint8_t m = settings().sleepMins;
  return m == 0 ? 0xFFFFFFFFu : (uint32_t)m * 60000u;
}

bool wifiOpen = false;
uint8_t wifiSel = 0;
bool settingsOpen = false;
uint8_t settingsSel = 0;
bool resetOpen = false;
uint8_t resetSel = 0;
static uint32_t resetConfirmUntil = 0;
static uint8_t resetConfirmIdx = 0xFF;

uint32_t promptArrivedMs = 0;

// ── Buttons & Touch ───────────────────────────────────────────────────────────
static ButtonHandler btnA(BoardConfig::PIN_BOOT_BUTTON);
static ButtonHandler btnB(BoardConfig::PIN_PWR_BUTTON);
static TouchHandler touch;

// BtnA long-press milestones
static bool brightnessFired = false; // fired at 600ms hold
static bool powerOffFired = false;   // fired at 3000ms hold

// ── Brightness ───────────────────────────────────────────────────────────────
static void applyBrightness()
{
  static const uint8_t levels[] = {20, 40, 60, 80, 100};
  axs15231bSetBrightnessPercent(levels[brightLevel]);
}

static void wake()
{
  lastInteractMs = millis();
  if (screenOff)
  {
    axs15231bWake();
    applyBrightness();
    screenOff = false;
  }
}

// ── WiFi PIN display ──────────────────────────────────────────────────────────
static void drawWifiPin()
{
  const Palette &p = uiPal();
  spr.fillSprite(p.bg);
  UI_FONT_SM();
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(TXZ + 8, 20);
  spr.print("SINERGY - enter PIN in VS Code:");
  UI_FONT_OFF();
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8];
  snprintf(b, sizeof(b), "%06lu", (unsigned long)wifiPinCode());
  spr.setCursor(TXZ + 20, 60);
  spr.print(b);
  spr.setTextSize(1);
  UI_FONT_SM();
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(TXZ + 8, 120);
  if (wifiGetMode() == WMODE_AP)
  {
    spr.printf("AP: %s  %s", wifiSsid(), wifiApPass());
    spr.setCursor(TXZ + 8, 132);
  }
  spr.printf("IP: %s", wifiIp());
}

// ── AP password display (no client connected yet) ─────────────────────────────
static void drawApPassword()
{
  const Palette &p = uiPal();
  spr.fillSprite(p.bg);
  UI_FONT_SM();
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(TXZ + 8, 20);
  spr.printf("Connect to: %s", wifiSsid());
  UI_FONT_OFF();
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(TXZ + 20, 60);
  spr.print(wifiApPass());
  spr.setTextSize(1);
  UI_FONT_SM();
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(TXZ + 8, 120);
  spr.printf("then open http://192.168.4.1");
}

// ── Send command ──────────────────────────────────────────────────────────────
static void sendCmd(const char *json)
{
  Serial.println(json);
  if (wifiClientAuthed())
  {
    size_t n = strlen(json);
    wifiWrite((const uint8_t *)json, n);
    wifiWrite((const uint8_t *)"\n", 1);
  }
}

// ── Menu zone helpers ─────────────────────────────────────────────────────────
// Hints at top-right matching physical button positions: A rightmost, B ~28px left.
static void drawZoneHints(const Palette &p, const char *aLbl = "A", const char *bLbl = "B")
{
  UI_FONT_SM();
  spr.setTextColor(p.textDim, p.bg);
  int aW = spr.getStrWidth(aLbl);
  int bW = spr.getStrWidth(bLbl);
  int ax = TXZ + TW - 36 - aW / 2;
  int bx = ax - 68 - bW / 2;
  spr.setCursor(ax, 2);
  spr.print(aLbl);
  spr.setCursor(bx, 2);
  spr.print(bLbl);
  spr.drawFastHLine(TXZ + 4, 14, TW - 8, p.textDim);
}

// ── Menus ─────────────────────────────────────────────────────────────────────
static const char *menuItems[] = {"wifi", "settings", "turn off", "help", "about", "demo", "close"};
static const uint8_t MENU_N = 7;

static const char *settingsItems[] = {
    "brightness", "sound", "wifi off", "led", "transcript", "ascii pet", "sleep", "reset", "back"};
static const uint8_t SETTINGS_N = 9;

static const char *resetItems[] = {"delete char", "factory reset", "back"};
static const uint8_t RESET_N = 3;

static void termPush(const char *s, bool fresh); // forward declare — defined below

static void applySetting(uint8_t idx)
{
  Settings &s = settings();
  switch (idx)
  {
  case 0:
    brightLevel = (brightLevel + 1) % 5;
    applyBrightness();
    return;
  case 1:
    s.sound = !s.sound;
    break;
  case 2:
    if (s.wifiMode != WMODE_OFF)
    {
      s.wifiMode = WMODE_OFF;
      settingsSave();
      termPush("wifi: off, restarting...", true);
      spr.flush();
      wifiSetMode(WMODE_OFF);
    }
    return;
  case 3:
    s.led = !s.led;
    break;
  case 4:
    s.hud = !s.hud;
    break;
  case 5:
    cycleSpecies();
    return;
  case 6:
  {
    static const uint8_t sleepOpts[] = {1, 2, 5, 10, 30, 0};
    static const uint8_t N = sizeof(sleepOpts);
    uint8_t cur = s.sleepMins;
    uint8_t next = sleepOpts[0];
    for (uint8_t i = 0; i < N; i++)
    {
      if (sleepOpts[i] == cur)
      {
        next = sleepOpts[(i + 1) % N];
        break;
      }
    }
    s.sleepMins = next;
  }
  break;
  case 7:
    resetOpen = true;
    resetSel = 0;
    resetConfirmIdx = 0xFF;
    return;
  case 8:
    settingsOpen = false;
    return;
  }
  settingsSave();
}

static void applyReset(uint8_t idx)
{
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;
  if (idx == 2)
  {
    resetOpen = false;
    return;
  }
  if (!armed)
  {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    return;
  }
  if (idx == 0)
  {
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory())
    {
      File e;
      while ((e = d.openNextFile()))
      {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        e.close();
        LittleFS.remove(path);
      }
      d.close();
    }
  }
  else
  {
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
  }
  delay(300);
  ESP.restart();
}

// ── Wifi submenu ──────────────────────────────────────────────────────────────
static const char *wifiItems[] = {"wifi STA", "wifi AP", "wifi connect", "back"};
static const uint8_t WIFI_N = 4;

static void applyWifi(uint8_t idx)
{
  Settings &s = settings();
  switch (idx)
  {
  case 0: // STA — connect to saved credentials
    s.wifiMode = WMODE_STA;
    settingsSave();
    termPush("wifi: STA, restarting...", true);
    spr.flush();
    wifiSetMode(WMODE_STA);
    return;
  case 1: // AP — for extension direct connection (no credential prompt)
    s.wifiMode = WMODE_AP;
    settingsSave();
    termPush("wifi: AP, restarting...", true);
    spr.flush();
    wifiSetMode(WMODE_AP);
    return;
  case 2: // Connect — AP mode showing password prominently; reboots STA after save
    s.wifiMode = WMODE_AP;
    settingsSave();
    termPush("wifi: connect mode, restarting...", true);
    spr.flush();
    wifiSetMode(WMODE_AP, true);
    return;
  case 3:
    wifiOpen = false;
    return;
  }
}

static void drawWifi()
{
  const Palette &p = uiPal();
  spr.fillRect(TXZ, 0, TW, H, p.bg);
  UI_FONT_MD();
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(TXZ + 6, 2);
  spr.print("WiFi");
  UI_FONT_SM();
  for (int i = 0; i < WIFI_N; i++)
  {
    bool sel = (i == wifiSel);
    int y = 22 + i * 16;
    spr.setTextColor(sel ? p.text : p.textDim, p.bg);
    spr.setCursor(TXZ + 6, y);
    spr.print(sel ? "> " : "  ");
    spr.print(wifiItems[i]);
    // Show current mode indicator
    if (i < 3)
    {
      WifiMode cur = wifiGetMode();
      bool active = (i == 0 && cur == WMODE_STA) ||
                    (i == 1 && cur == WMODE_AP) ||
                    (i == 2 && cur == WMODE_AP);
      if (active)
      {
        spr.setTextColor(uiGreen(), p.bg);
        spr.print(" *");
      }
    }
  }
  drawZoneHints(p);
}

void menuConfirm()
{
  switch (menuSel)
  {
  case 0: // wifi
    wifiOpen = true;
    menuOpen = false;
    wifiSel = 0;
    break;
  case 1: // settings
    settingsOpen = true;
    menuOpen = false;
    settingsSel = 0;
    break;
  case 2: // turn off
    BoardConfig::releaseBatteryPowerHold();
    delay(200);
    ESP.restart();
    break;
  case 3: // help
  case 4: // about
    menuOpen = false;
    displayMode = DISP_INFO;
    infoPage = (menuSel == 3) ? 1 : 4;
    break;
  case 5: // demo
    dataSetDemo(!dataDemo());
    break;
  case 6: // close
    menuOpen = false;
    break;
  }
}

static void drawMenu()
{
  const Palette &p = uiPal();
  spr.fillRect(TXZ, 0, TW, H, p.bg);
  UI_FONT_SM();
  for (int i = 0; i < MENU_N; i++)
  {
    bool sel = (i == menuSel);
    int y = 20 + i * 14;
    spr.setTextColor(sel ? p.text : p.textDim, p.bg);
    spr.setCursor(TXZ + 6, y);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4)
    {
      spr.setTextColor(sel ? p.text : p.textDim, p.bg);
      spr.print(dataDemo() ? "  on" : "  off");
    }
  }
  drawZoneHints(p);
}

static void drawSettings()
{
  const Palette &p = uiPal();
  Settings &s = settings();
  spr.fillRect(TXZ, 0, TW, H, p.bg);
  UI_FONT_SM();
  // vals aligned to settingsItems: [sound, wifi(skip), led, hud]
  bool vals[] = {s.sound, s.led, s.hud};
  for (int i = 0; i < SETTINGS_N; i++)
  {
    bool sel = (i == settingsSel);
    int y = 20 + i * 14;
    spr.setTextColor(sel ? p.text : p.textDim, p.bg);
    spr.setCursor(TXZ + 6, y);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(TXZ + TW - 42, y);
    uint16_t valColor = sel ? p.text : p.textDim;
    if (i == 0)
    {
      static const uint8_t bLevels[] = {20, 40, 60, 80, 100};
      spr.setTextColor(valColor, p.bg);
      spr.printf("%u%%", bLevels[brightLevel]);
    }
    else if (i == 2) // wifi off
    {
      uint8_t wm = s.wifiMode;
      spr.setTextColor(wm ? valColor : uiHot(), p.bg);
      spr.print(wm ? " on" : "off");
    }
    else if (i == 1) // sound
    {
      spr.setTextColor(s.sound ? uiGreen() : valColor, p.bg);
      spr.print(s.sound ? " on" : "off");
    }
    else if (i == 3) // led
    {
      spr.setTextColor(s.led ? uiGreen() : valColor, p.bg);
      spr.print(s.led ? " on" : "off");
    }
    else if (i == 4) // hud/transcript
    {
      spr.setTextColor(s.hud ? uiGreen() : valColor, p.bg);
      spr.print(s.hud ? " on" : "off");
    }
    else if (i == 5) // ascii pet
    {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.setTextColor(valColor, p.bg);
      spr.printf("%u/%u", pos, total);
    }
    else if (i == 6) // sleep
    {
      spr.setTextColor(valColor, p.bg);
      uint8_t m = s.sleepMins;
      if (m == 0)
        spr.print("off");
      else
        spr.printf("%um", m);
    }
  }
  drawZoneHints(p, "Next", "Change");
}

static void drawReset()
{
  const Palette &p = uiPal();
  spr.fillRect(TXZ, 0, TW, H, p.bg);
  UI_FONT_MD();
  spr.setTextColor(uiHot(), p.bg);
  spr.setCursor(TXZ + 6, 2);
  spr.print("RESET");
  spr.drawFastHLine(TXZ + 4, 14, TW - 8, uiHot());
  UI_FONT_SM();
  for (int i = 0; i < RESET_N; i++)
  {
    bool sel = (i == resetSel);
    int y = 20 + i * 14;
    spr.setTextColor(sel ? p.text : p.textDim, p.bg);
    spr.setCursor(TXZ + 6, y);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) && (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed)
      spr.setTextColor(uiHot(), p.bg);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawZoneHints(p);
}

// ── Approval banner ───────────────────────────────────────────────────────────
static void drawApproval()
{
  const Palette &p = uiPal();
  int ay = H - 54;
  spr.fillRect(TXZ, ay, TW, H - ay, p.bg);
  spr.drawFastHLine(TXZ, ay, TW, p.textDim);
  UI_FONT_SM();
  spr.setTextColor(p.textDim, p.bg);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  spr.setCursor(TXZ + 4, ay + 2);
  if (waited >= 10)
    spr.setTextColor(uiHot(), p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);
  UI_FONT_MD();
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(TXZ + 4, ay + 12);
  spr.print(tama.promptTool);
  UI_FONT_SM();
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(TXZ + 4, ay + 32);
  spr.printf("%.50s", tama.promptHint);
  if (responseSent)
  {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(TXZ + 4, ay + 42);
    spr.print("sent...");
  }
  else
  {
    spr.setTextColor(uiGreen(), p.bg);
    spr.setCursor(TXZ + 4, ay + 42);
    spr.print("A:approve");
    spr.setTextColor(uiHot(), p.bg);
    spr.setCursor(TXZ + 100, ay + 42);
    spr.print("B:deny");
  }
}

// ── Terminal buffer ───────────────────────────────────────────────────────────
static constexpr int TERM_LINES = 18;
static constexpr int TERM_COLS = 76; // TW(464) / ~6px per char
static constexpr int TERM_LH = 9;    // px per line

static char termBuf[TERM_LINES][TERM_COLS + 1];
static bool termFresh[TERM_LINES]; // true = highlight as newest
static int termHead = 0;           // next write position (ring)
static int termCount = 0;          // total lines written

static void termPush(const char *s, bool fresh = true)
{
  // Clear freshness on all previous lines
  for (int i = 0; i < TERM_LINES; i++)
    termFresh[i] = false;
  snprintf(termBuf[termHead], sizeof(termBuf[0]), "%.*s", TERM_COLS, s);
  termFresh[termHead] = fresh;
  termHead = (termHead + 1) % TERM_LINES;
  termCount++;
}

// Feed new tama data into the terminal buffer
static void termUpdate()
{
  if (tama.lineGen == lastLineGen)
    return;
  lastLineGen = tama.lineGen;
  wake();
  if (tama.nLines > 0)
  {
    // Push only the newest line (last in tama.lines)
    termPush(tama.lines[tama.nLines - 1]);
  }
  else if (tama.msg[0])
  {
    termPush(tama.msg);
  }
}

// ── HUD: unix-terminal style, newest line at bottom ──────────────────────────
static void drawHUD()
{
  const Palette &p = uiPal();
  UI_FONT_SM();

  // How many lines fit in the text zone (reserve bottom area for approval prompt)
  bool hasPrompt = tama.promptId[0] && !responseSent;
  int reservedLines = hasPrompt ? 5 : 0;
  int visLines = H / TERM_LH - reservedLines;
  if (visLines < 1)
    visLines = 1;

  // Draw terminal lines — oldest at top, newest at bottom
  // Iterate the ring buffer oldest-first
  int total = termCount < TERM_LINES ? termCount : TERM_LINES;
  int startIdx = (termHead - total + TERM_LINES) % TERM_LINES; // oldest entry
  int dispStart = total > visLines ? total - visLines : 0;     // skip if more than fit

  int dispCount = total - dispStart; // number of lines we'll draw (≤ visLines)
  for (int i = dispStart; i < total; i++)
  {
    int slot = (startIdx + i) % TERM_LINES;
    int row = (i - dispStart); // 0 = oldest visible, dispCount-1 = newest
    // Pin newest line to bottom: last row sits at (visLines-1)*TERM_LH
    int screenY = (visLines - dispCount + row) * TERM_LH;
    if (screenY < 0)
      continue;

    bool isFresh = termFresh[slot];
    spr.setTextColor(isFresh ? p.text : p.textDim, p.bg);
    spr.setCursor(TXZ + 4, screenY);
    spr.print(termBuf[slot]);
  }

  // Approval prompt pinned at bottom
  if (hasPrompt)
    drawApproval();
}

// ── Media page ────────────────────────────────────────────────────────────────
static constexpr int MB_Y = 128; // button top y
static constexpr int MB_H = 36;  // button height
static constexpr int MB_W = 84;  // button width
static constexpr int MB_GAP = 8; // gap between buttons
// 5 buttons centred in TW:  5*84 + 4*8 = 452, margin = (464-452)/2 = 6
static constexpr int MB_X0 = TXZ + 6;
static constexpr int MB_R = 3; // corner radius
static constexpr uint8_t MBTNS_N = 5;

enum MediaAction : uint8_t
{
  MA_PREV,
  MA_VDOWN,
  MA_PLAY,
  MA_VUP,
  MA_NEXT
};
static const MediaAction MB_ACTIONS[MBTNS_N] = {MA_PREV, MA_VDOWN, MA_PLAY, MA_VUP, MA_NEXT};
static const char *const MB_LABELS[MBTNS_N] = {"<<", "V-", ">||", "V+", ">>"};

static inline int16_t mbX(uint8_t i) { return MB_X0 + i * (MB_W + MB_GAP); }

static void drawMedia()
{
  const Palette &p = uiPal();
  spr.fillRect(TXZ, 0, TW, H, p.bg);

  const char *st = tama.mediaStatus;
  bool playing = strcmp(st, "Playing") == 0;
  bool paused = strcmp(st, "Paused") == 0;
  uint16_t stColor = playing ? uiGreen() : paused ? uiInk()
                                                  : p.textDim;

  // Header
  UI_FONT_MD();
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(TXZ + 4, 2);
  spr.print("Media");

  UI_FONT_SM();
  spr.setTextColor(stColor, p.bg);
  int stW = spr.getStrWidth(st[0] ? st : "Stopped");
  spr.setCursor(TXZ + TW - stW - 4, 4);
  spr.print(st[0] ? st : "Stopped");

  spr.drawFastHLine(TXZ + 4, 16, TW - 8, p.textDim);

  int maxW = TW - 8;

  // Title
  UI_FONT_MD();
  spr.setCursor(TXZ + 4, 22);
  if (tama.mediaTitle[0])
  {
    spr.setTextColor(p.text, p.bg);
    if (spr.getStrWidth(tama.mediaTitle) <= maxW)
    {
      spr.print(tama.mediaTitle);
    }
    else
    {
      char tmp[65];
      strncpy(tmp, tama.mediaTitle, sizeof(tmp) - 1);
      tmp[sizeof(tmp) - 1] = 0;
      size_t len = strlen(tmp);
      while (len > 0 && spr.getStrWidth(tmp) > maxW)
      {
        tmp[--len] = 0;
      }
      if (len > 3)
      {
        tmp[len - 1] = '.';
        tmp[len - 2] = '.';
        tmp[len - 3] = '.';
      }
      spr.print(tmp);
    }
  }
  else
  {
    spr.setTextColor(p.textDim, p.bg);
    spr.print("Nothing playing");
  }

  // Artist
  UI_FONT_SM();
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(TXZ + 4, 40);
  if (tama.mediaArtist[0])
  {
    if (spr.getStrWidth(tama.mediaArtist) <= maxW)
    {
      spr.print(tama.mediaArtist);
    }
    else
    {
      char tmp[49];
      strncpy(tmp, tama.mediaArtist, sizeof(tmp) - 1);
      tmp[sizeof(tmp) - 1] = 0;
      size_t len = strlen(tmp);
      while (len > 0 && spr.getStrWidth(tmp) > maxW)
      {
        tmp[--len] = 0;
      }
      if (len > 3)
      {
        tmp[len - 1] = '.';
        tmp[len - 2] = '.';
        tmp[len - 3] = '.';
      }
      spr.print(tmp);
    }
  }

  // Progress bar (shown when duration is known)
  if (tama.mediaDur > 0)
  {
    int barX = TXZ + 4;
    int barY = 56;
    int barW = TW - 8;
    int barH = 4;
    spr.drawFastHLine(barX, barY, barW, p.textDim);
    uint32_t pos = tama.mediaPos <= tama.mediaDur ? tama.mediaPos : tama.mediaDur;
    int filled = (int)((uint64_t)pos * barW / tama.mediaDur);
    if (filled > 0)
      spr.drawFastHLine(barX, barY, filled, stColor);

    // Time labels: elapsed / remaining
    char tLeft[8], tRight[8];
    snprintf(tLeft, sizeof(tLeft), "%u:%02u", pos / 60, pos % 60);
    uint32_t rem = tama.mediaDur - pos;
    snprintf(tRight, sizeof(tRight), "-%u:%02u", rem / 60, rem % 60);
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(barX, barY + 6);
    spr.print(tLeft);
    int rW = spr.getStrWidth(tRight);
    spr.setCursor(barX + barW - rW, barY + 6);
    spr.print(tRight);
  }

  // Button bar
  UI_FONT_MD();
  for (uint8_t i = 0; i < MBTNS_N; i++)
  {
    int16_t bx = mbX(i);
    bool isPlay = MB_ACTIONS[i] == MA_PLAY;
    uint16_t col = isPlay ? stColor : p.textDim;
    spr.drawRect(bx, MB_Y, MB_W, MB_H, col);
    // clip the 4 corners (1px) for a subtle rounded look without arc artefacts
    spr.drawPixel(bx, MB_Y, p.bg);
    spr.drawPixel(bx + MB_W - 1, MB_Y, p.bg);
    spr.drawPixel(bx, MB_Y + MB_H - 1, p.bg);
    spr.drawPixel(bx + MB_W - 1, MB_Y + MB_H - 1, p.bg);
    const char *lbl = MB_LABELS[i];
    int lw = spr.getStrWidth(lbl);
    spr.setTextColor(col, p.bg);
    spr.setCursor(bx + (MB_W - lw) / 2, MB_Y + (MB_H - 13) / 2);
    spr.print(lbl);
  }
}

// ── Info pages ────────────────────────────────────────────────────────────────
static void drawInfo()
{
  const Palette &p = uiPal();
  spr.fillRect(TXZ, 0, TW, H, p.bg);

  // Small body text helper
  auto ln = [&](int &y, uint16_t c, const char *fmt, ...)
  {
    UI_FONT_SM();
    char b[80];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    spr.setTextColor(c, p.bg);
    spr.setCursor(TXZ + 4, y);
    spr.print(b);
    y += 10;
  };
  // Medium section header helper
  auto hd = [&](int &y, const char *label)
  {
    UI_FONT_MD();
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(TXZ + 4, y);
    spr.print(label);
    y += 16;
  };

  // Page header
  UI_FONT_MD();
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(TXZ + 4, 2);
  spr.print("Info");
  UI_FONT_SM();
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(TXZ + TW - 30, 2);
  spr.printf("%u/%u", infoPage + 1, INFO_PAGES);
  int y = 18;

  if (infoPage == 0)
  {
    hd(y, "ABOUT");
    ln(y, p.textDim, "Watches your Claude desktop sessions.");
    ln(y, p.textDim, "Sleeps when idle, wakes when busy,");
    ln(y, p.textDim, "gets impatient on pending approvals.");
    y += 4;
    ln(y, p.text, "A: approve prompt from device");
    y += 4;
    ln(y, p.textDim, "18 species. Settings > ascii pet to cycle.");
  }
  else if (infoPage == 1)
  {
    hd(y, "BUTTONS");
    ln(y, p.text, "A (brightness btn)");
    ln(y, p.textDim, "  short: next screen / approve");
    ln(y, p.textDim, "  hold 600ms: brightness up");
    ln(y, p.textDim, "  hold 3s: power off");
    y += 4;
    ln(y, p.text, "B (power btn)");
    ln(y, p.textDim, "  short: deny / next page / action");
    ln(y, p.textDim, "  hold: open/close menu");
  }
  else if (infoPage == 2)
  {
    hd(y, "CLAUDE");
    ln(y, p.textDim, "sessions  %u", tama.sessionsTotal);
    ln(y, p.textDim, "running   %u", tama.sessionsRunning);
    ln(y, p.textDim, "waiting   %u", tama.sessionsWaiting);
    y += 6;
    ln(y, p.text, "LINK");
    ln(y, p.textDim, "via       %s", dataScenarioName());
    ln(y, p.textDim, "state     %s", stateNames[activeState]);
    y += 4;
    ln(y, p.text, "WIFI");
    {
      static const char *wfNames[] = {"off", "STA", "AP"};
      uint8_t wm = settings().wifiMode;
      ln(y, p.textDim, "mode      %s", wfNames[wm]);
    }
    if (settings().wifiMode != WMODE_OFF)
    {
      ln(y, p.textDim, "ip        %s", wifiUp() ? wifiIp() : "...");
      if (settings().wifiMode == WMODE_AP && wifiUp())
      {
        ln(y, p.textDim, "ssid      %s", wifiSsid());
        ln(y, p.textDim, "pass      %s", wifiApPass());
      }
      if (wifiClientConnected())
      {
        ln(y, p.textDim, "client    %s", wifiClientAuthed() ? "auth ok" : "pin...");
      }
    }
  }
  else if (infoPage == 3)
  {
    BoardConfig::BatteryStatus bat;
    bool hasBat = BoardConfig::readBatteryStatus(bat);
    hd(y, "DEVICE");
    if (hasBat)
    {
      UI_FONT_MD();
      spr.setTextColor(p.text, p.bg);
      spr.setCursor(TXZ + 4, y);
      spr.printf("%u%%", bat.percent);
      UI_FONT_SM();
      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(TXZ + 50, y + 3);
      spr.printf("%.2fV", bat.voltage);
      y += 18;
    }
    ln(y, p.textDim, "heap   %uKB", ESP.getFreeHeap() / 1024);
    uint32_t up = millis() / 1000;
    ln(y, p.textDim, "uptime %luh%02lum", up / 3600, (up / 60) % 60);
    if (ownerName()[0])
      ln(y, p.textDim, "owner  %s", ownerName());
    {
      static const uint8_t bLv[] = {20, 40, 60, 80, 100};
      ln(y, p.textDim, "bright %u%%", bLv[brightLevel]);
    }
  }
  else if (infoPage == 4)
  {
    hd(y, "CREDITS");
    y += 2;
    ln(y, p.text, "Felix Rieseberg");
    y += 2;
    ln(y, p.textDim, "github.com/anthropics");
    ln(y, p.textDim, "/claude-desktop-buddy");
    y += 4;
    ln(y, p.textDim, "Waveshare ESP32-S3 port:");
    ln(y, p.textDim, "Eduardo Moro + Claude Sonnet 4.6");
  }
}

// ── Pet stats ─────────────────────────────────────────────────────────────────
static void drawPet()
{
  const Palette &p = uiPal();
  spr.fillRect(TXZ, 0, TW, H, p.bg);
  UI_FONT_MD();
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(TXZ + 4, 2);
  if (ownerName()[0])
  {
    spr.print(ownerName());
    spr.print("'s ");
  }
  spr.print(petName());
  UI_FONT_SM();
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(TXZ + TW - 30, 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);

  if (petPage == 0)
  {
    int y = 16;
    auto row = [&](const char *label, const char *fmt, ...)
    {
      char b[32];
      va_list a;
      va_start(a, fmt);
      vsnprintf(b, sizeof(b), fmt, a);
      va_end(a);
      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(TXZ + 4, y);
      spr.print(label);
      spr.setTextColor(p.text, p.bg);
      spr.setCursor(TXZ + 90, y);
      spr.print(b);
      y += 12;
    };
    row("mood", "%u/4", statsMoodTier());
    row("level", "%u", stats().level);
    row("approved", "%u", stats().approvals);
    row("denied", "%u", stats().denials);
    uint32_t tok = stats().tokens;
    if (tok >= 1000000)
      row("tokens", "%.1fM", tok / 1000000.0f);
    else if (tok >= 1000)
      row("tokens", "%.1fK", tok / 1000.0f);
    else
      row("tokens", "%lu", (unsigned long)tok);
    row("today tok", "%lu", (unsigned long)tama.tokensToday);
  }
  else
  {
    int y = 16;
    spr.setTextColor(p.textDim, p.bg);
    auto ln = [&](const char *s)
    { spr.setCursor(TXZ+4, y); spr.print(s); y += 10; };
    ln("MOOD: approve fast=up, deny lots=down");
    y += 4;
    ln("FED: 50K tokens = level up + confetti");
    y += 4;
    ln("A: screens  B: page  hold B: menu");
  }
}

// ── Buddy zone stat bars ─────────────────────────────────────────────────────
// Layout (buddy zone 172×172, art ends ~y=126, 46px remain):
//   y=126  approved N  tok NK
//   y=135  ♥♥♥♡ (mood)   ■■□□□ (energy)   — same line
//   y=145  ●●●●●○○○○○  (fed, 10 circles)
//   y=155  [Lv N]
static void tinyHeart(int x, int y, bool filled, uint16_t col)
{
  if (filled)
  {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  }
  else
  {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawBuddyStats()
{
  // Middle row — all three elements share the same vertical center (cy=136):
  //   LEFT:   5 energy circles, r=5, touching the left edge
  //   CENTER: 4 mood hearts, horizontally centered between energy and Lv
  //   RIGHT:  Lv badge, touching the right edge
  // Bottom row: approvals left, tokens right
  const Palette &p = uiPal();
  UI_FONT_OFF();
  spr.setTextSize(1);

  static constexpr int CY = 154;  // shared vertical center (just above bottom row)
  static constexpr int LVH = 10;  // Lv badge height
  static constexpr int ER = 4;    // energy circle radius — matches heart size
  static constexpr int ERST = 10; // energy circle stride (diameter + 2px gap)

  // Lv badge — right edge
  char lvbuf[8];
  snprintf(lvbuf, sizeof(lvbuf), "Lv %u", stats().level);
  int lvw = strlen(lvbuf) * 6 + 4;
  int lvX = BZ - 2 - lvw;
  spr.fillRoundRect(lvX, CY - LVH / 2, lvw, LVH, 2, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(lvX + 2, CY - LVH / 2 + 1);
  spr.print(lvbuf);

  // Energy circles — left edge, mirrored to Lv
  uint8_t energy = statsEnergyTier();
  static const uint16_t eCols[] = {BUDDY_RED, BUDDY_YEL, BUDDY_YEL, BUDDY_GREEN, BUDDY_GREEN};
  int eRightEdge = 2; // rightmost circle right edge, builds leftward
  for (int i = 4; i >= 0; i--)
  {
    int cx = BZ - 2 - lvw - 4 - ER - (4 - i) * ERST; // mirror of Lv gap on the right
    // Actually build left-to-right from x=2+ER
    cx = 2 + ER + i * ERST;
    if (i < (int)energy)
      spr.fillCircle(cx, CY, ER, eCols[i]);
    else
      spr.drawCircle(cx, CY, ER, p.textDim); // all at CY = Lv badge center
  }
  eRightEdge = 2 + ER + 4 * ERST + ER; // right edge of last energy circle

  // Mood hearts — centered in the gap between energy and Lv
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? BUDDY_HEART : (mood >= 2) ? BUDDY_YEL
                                                             : p.textDim;
  int gapL = eRightEdge + 2;
  int gapR = lvX - 2;
  int heartSpan = 3 * 12 + 8; // 4 hearts at stride 12, ~8px wide each
  int heartX = (gapL + gapR - heartSpan) / 2;
  for (uint8_t i = 0; i < 4; i++)
    tinyHeart(heartX + 4 + i * 12, CY - LVH / 2, i < mood, moodCol);

  // Bottom: approvals left, tokens right
  uint32_t tok = stats().tokens;
  char tokbuf[12];
  if (tok >= 1000000)
    snprintf(tokbuf, sizeof(tokbuf), "%.1fM", tok / 1000000.0f);
  else if (tok >= 1000)
    snprintf(tokbuf, sizeof(tokbuf), "%.1fK", tok / 1000.0f);
  else
    snprintf(tokbuf, sizeof(tokbuf), "%lu", (unsigned long)tok);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(2, 163);
  spr.printf("appr %u", stats().approvals);
  spr.setCursor(BZ - 2 - (int)strlen(tokbuf) * 6, 163);
  spr.print(tokbuf);
}

// ── Status bar (top of buddy zone) ───────────────────────────────────────────
static void drawStatusBar()
{
  const Palette &p = uiPal();
  UI_FONT_SM();

  // WiFi indicator — top-left
  if (settings().wifiMode != WMODE_OFF)
  {
    uint16_t wfCol = wifiClientAuthed() ? uiGreen() : (wifiUp() ? p.textDim : uiHot());
    spr.setTextColor(wfCol, p.bg);
    spr.setCursor(2, 2);
    if (!wifiUp())
      spr.print("WF?");
    else if (wifiClientAuthed())
      spr.print("WFI");
    else
      spr.print("WF-");
  }

  // Battery — top-right, right-aligned to BZ edge
  static uint32_t lastBatMs = 0;
  static uint8_t batPct = 0;
  if (millis() - lastBatMs > 30000)
  {
    lastBatMs = millis();
    BoardConfig::BatteryStatus b;
    if (BoardConfig::readBatteryStatus(b))
      batPct = b.percent;
  }
  if (batPct > 0)
  {
    char batBuf[6];
    snprintf(batBuf, sizeof(batBuf), "%u%%", batPct);
    int bw = spr.getStrWidth(batBuf);
    spr.setTextColor(batPct < 20 ? uiHot() : p.textDim, p.bg);
    spr.setCursor(BZ - 2 - bw, 2);
    spr.print(batBuf);
  }

  // Fed pips — centered between BLE and battery
  UI_FONT_OFF();
  spr.setTextSize(1);
  uint8_t fed = statsFedProgress();
  static constexpr int FED_N = 10;
  static constexpr int FED_R = 2;  // circle radius
  static constexpr int FED_ST = 5; // stride between centers
  int fedTotalW = (FED_N - 1) * FED_ST;
  int fedStartX = BZ / 2 - fedTotalW / 2;
  for (uint8_t i = 0; i < FED_N; i++)
  {
    int cx = fedStartX + i * FED_ST;
    if (i < fed)
      spr.fillCircle(cx, 5, FED_R, p.body);
    else
      spr.drawCircle(cx, 5, FED_R, p.textDim);
  }
}

// ── Persona derive ────────────────────────────────────────────────────────────
PersonaState derive(const TamaState &s)
{
  if (!s.connected)
    return P_IDLE;
  if (s.sessionsWaiting > 0)
    return P_ATTENTION;
  if (s.recentlyCompleted)
    return P_CELEBRATE;
  if (s.sessionsRunning >= 3)
    return P_BUSY;
  return P_IDLE;
}

void triggerOneShot(PersonaState s, uint32_t durMs)
{
  activeState = s;
  oneShotUntil = millis() + durMs;
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup()
{
  Serial.begin(115200);
  BoardConfig::begin();

  spr.begin();
  axs15231bSetBacklight(false);

  statsLoad();
  settingsLoad();
  paletteLoad();
  buddyApplyPalette(uiPal().bg, uiPal().text, uiPal().textDim,
                   uiPal().body, uiHot(), uiGreen());
  petNameLoad();
  buddyInit();

  LittleFS.begin(true);
  characterInit(nullptr);
  gifAvailable = characterLoaded();
  characterSetBg(uiPal().bg);
  // Restore GIF mode if it was the last selection (sentinel 0xFF)
  buddyMode = !(gifAvailable && speciesIdxLoad() == 0xFF);

  applyBrightness();

  // Splash
  {
    const Palette &p = uiPal();
    spr.fillSprite(p.bg);
    spr.setTextSize(2);
    spr.setTextDatum(MC_DATUM);
    if (ownerName()[0])
    {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);
      spr.drawString(line, W / 2, H / 2 - 14);
      spr.setTextColor(p.body, p.bg);
      spr.drawString(petName(), W / 2, H / 2 + 14);
    }
    else
    {
      spr.setTextColor(p.body, p.bg);
      spr.drawString("Hello!", W / 2, H / 2 - 14);
      spr.setTextSize(1);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W / 2, H / 2 + 14);
    }
    spr.setTextDatum(TL_DATUM);
    axs15231bSetBacklight(true);
    applyBrightness();
    spr.flush();
    delay(1800);
  }

  wifiInit((WifiMode)settings().wifiMode, [](const char *msg)
           { termPush(msg); });

  touch.begin();

  termPush("--- buddy ready ---", false);
  lastInteractMs = millis();
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop()
{
  uint32_t now = millis();
  t++;

  btnA.update(now);
  btnB.update(now);

  // ── Touch: media controls when in media app ──────────────────────────────
  {
    TouchEvent te;
    if (touch.poll(te) && !screenOff)
    {
      lastInteractMs = now;
      if (te.phase == TouchPhase::End && now >= touchGuardUntil &&
          displayMode == DISP_APP)
      {
        for (uint8_t i = 0; i < MBTNS_N; i++) {
          int16_t bx = mbX(i);
          if (te.x >= bx && te.x < bx + MB_W &&
              te.y >= MB_Y && te.y < MB_Y + MB_H)
          {
            const char* act = nullptr;
            switch (MB_ACTIONS[i]) {
              case MA_PREV:  act = "prev";       break;
              case MA_VDOWN: act = "vdown";      break;
              case MA_PLAY:  act = "play_pause"; break;
              case MA_VUP:   act = "vup";        break;
              case MA_NEXT:  act = "next";       break;
            }
            if (act) {
              char cmd[64];
              snprintf(cmd, sizeof(cmd), "{\"cmd\":\"media_ctrl\",\"action\":\"%s\"}", act);
              sendCmd(cmd);
            }
            break;
          }
        }
      }
    }
  }

  dataPoll(&tama);

  // Apply theme palette when VS Code sends an update
  {
    static uint16_t lastPalGen = 0;
    if (tama.paletteGen != lastPalGen)
    {
      lastPalGen = tama.paletteGen;
      paletteApply(tama.palBody, tama.palBg, tama.palText, tama.palTextDim,
                   tama.palInk, tama.palGreen, tama.palHot, tama.palPanel);
      buddyApplyPalette(uiPal().bg, uiPal().text, uiPal().textDim,
                        uiPal().body, uiHot(), uiGreen());
      characterSetBg(uiPal().bg);
    }
  }

  termUpdate();

  if (statsPollLevelUp())
    triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);
  if ((int32_t)(now - oneShotUntil) >= 0)
    activeState = baseState;

  // Push buddy state to WiFi client on auth or state change
  {
    static PersonaState lastWifiState = P_SLEEP;
    static bool lastWifiAuthed = false;
    bool authed = wifiClientAuthed();
    if (authed && (!lastWifiAuthed || activeState != lastWifiState))
    {
      lastWifiState = activeState;
      char evt[56];
      snprintf(evt, sizeof(evt), "{\"evt\":\"state\",\"state\":\"%s\"}\n", stateNames[activeState]);
      wifiWrite((const uint8_t *)evt, strlen(evt));
    }
    lastWifiAuthed = authed;
  }

  // Prompt arrival
  if (strcmp(tama.promptId, lastPromptId) != 0)
  {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId) - 1);
    lastPromptId[sizeof(lastPromptId) - 1] = 0;
    responseSent = false;
    if (tama.promptId[0])
    {
      promptArrivedMs = millis();
      wake();
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = wifiOpen = false;
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Wake on button press
  if (btnA.isHeld() || btnB.isHeld())
  {
    if (screenOff)
    {
      if (btnA.isHeld())
        swallowA = true;
      if (btnB.isHeld())
        swallowB = true;
    }
    wake();
  }

  // ── BtnA long-hold: brightness (600ms), power-off (3000ms) ──────────────
  if (btnA.isHeld() && !swallowA)
  {
    uint32_t held = btnA.heldDurationMs(now);
    if (held >= 3000 && !powerOffFired)
    {
      powerOffFired = true;
      BoardConfig::releaseBatteryPowerHold();
      delay(200);
      ESP.restart();
    }
    else if (held >= 600 && !brightnessFired)
    {
      brightnessFired = true;
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
    }
  }

  // ── BtnA release (short press) ───────────────────────────────────────────
  if (btnA.wasReleasedEvent())
  {
    brightnessFired = false;
    powerOffFired = false;
    if (!swallowA)
    {
      if (inPrompt)
      {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        if (tookS < 5)
          triggerOneShot(P_HEART, 2000);
      }
      else if (resetOpen)
      {
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      }
      else if (wifiOpen)
      {
        wifiSel = (wifiSel + 1) % WIFI_N;
      }
      else if (settingsOpen)
      {
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      }
      else if (menuOpen)
      {
        menuSel = (menuSel + 1) % MENU_N;
      }
      else if (displayMode == DISP_INFO)
      {
        infoPage = (infoPage + 1) % INFO_PAGES;
      }
      else
      {
        displayMode = (displayMode + 1) % DISP_COUNT;
      }
    }
    swallowA = false;
  }

  // ── BtnB short press ─────────────────────────────────────────────────────
  if (btnB.wasReleasedEvent())
  {
    if (!swallowB)
    {
      if (inPrompt)
      {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        statsOnDenial();
      }
      else if (resetOpen)
      {
        applyReset(resetSel);
      }
      else if (wifiOpen)
      {
        applyWifi(wifiSel);
      }
      else if (settingsOpen)
      {
        applySetting(settingsSel);
      }
      else if (menuOpen)
      {
        menuConfirm();
      }
      else
      {
        // Cycle apps: normal → media → normal
        if (displayMode == DISP_NORMAL)
        {
          displayMode = DISP_APP;
          touchGuardUntil = now + 400;
        }
        else
        {
          displayMode = DISP_NORMAL;
        }
      }
    }
    swallowB = false;
  }

  // ── BtnB long hold: toggle menu ─────────────────────────────────────────
  static bool menuLongFired = false;
  if (btnB.isHeld() && !swallowB && btnB.heldDurationMs(now) >= 600 && !menuLongFired)
  {
    menuLongFired = true;
    swallowB = true; // swallow the upcoming release so it doesn't also confirm
    if (resetOpen)
      resetOpen = false;
    else if (wifiOpen)
      wifiOpen = false;
    else if (settingsOpen)
      settingsOpen = false;
    else
    {
      menuOpen = !menuOpen;
      menuSel = 0;
    }
  }
  if (!btnB.isHeld())
    menuLongFired = false;

  // ── Screen auto-off ──────────────────────────────────────────────────────
  if (!screenOff && !inPrompt && millis() - lastInteractMs > screenOffMs())
  {
    axs15231bSleep();
    screenOff = true;
  }

  // ── Render ───────────────────────────────────────────────────────────────
  if (!screenOff)
  {
    const Palette &p = uiPal();

    if (buddyMode)
    {
      // ASCII mode: clear full frame each tick
      spr.fillSprite(p.bg);
      UI_FONT_OFF();
      buddyTick(activeState);
      drawBuddyStats();
    }
    else
    {
      // GIF mode: only clear the text zone; GIF engine owns the buddy zone
      // so the last decoded frame persists between GIF frame boundaries
      spr.fillRect(BZ, 0, W - BZ, H, p.bg);
      UI_FONT_OFF();
      characterSetState(activeState);
      characterTick();
      drawBuddyStats();
    }
    // Vertical divider between buddy zone and text zone
    // for (int iy = 0; iy < H; iy++) spr.drawPixel(TXZ - 4, iy, p.textDim);
    drawStatusBar();

    if (wifiPinCode())
    {
      drawWifiPin();
    }
    else if (wifiConnectMode() && wifiUp() && !wifiClientConnected())
    {
      drawApPassword();
    }
    else if (displayMode == DISP_INFO)
    {
      drawInfo();
    }
    else if (displayMode == DISP_APP)
    {
      drawMedia();
    }
    else if (displayMode == DISP_PET)
    {
      drawPet();
    }
    else if (settings().hud)
    {
      drawHUD();
    }

    if (resetOpen)
      drawReset();
    else if (wifiOpen)
      drawWifi();
    else if (settingsOpen)
      drawSettings();
    else if (menuOpen)
      drawMenu();

    spr.flush();
  }

  delay(screenOff ? 100 : 16);
}
