#include "character.h"
#include "display/BuddyDisplay.h"
#include <LittleFS.h>
#include <AnimatedGIF.h>
#include <ArduinoJson.h>

extern BuddyDisplay spr;

// Buddy zone on the 640×172 display: leftmost 200×172 pixels.
static constexpr int BZ = 172;
static constexpr int BH = BuddyDisplay::H;

static const char *STATE_NAMES[] = {
    "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart"};
static const uint8_t N_STATES = 7;

// ── text mode ────────────────────────────────────────────────────────────────
struct TextState
{
  char frames[8][20];
  uint8_t nFrames;
  uint16_t delayMs;
};
static TextState textStates[N_STATES];
static bool textMode = false;
static uint8_t textFrame = 0;
static uint32_t textNext = 0;

// ── state ─────────────────────────────────────────────────────────────────────
static bool loaded = false;
static Palette pal = {0xC2A6, 0x0000, 0xFFFF, 0x8410, 0x0000};
static char basePath[48];

static const uint8_t MAX_GIFS = 32;
static char gifPaths[MAX_GIFS][32];
static uint8_t stateStart[N_STATES];
static uint8_t stateCount[N_STATES];
static uint8_t stateRot[N_STATES];
static uint8_t gifTotal = 0;
static uint8_t curState = 0xFF;

static AnimatedGIF gif;
static File gifFile;
static int gifX = 0, gifY = 0, gifW = 0, gifH = 0;

static uint32_t nextFrameAt = 0;
static uint32_t animPauseUntil = 0;
static uint32_t variantStartedMs = 0;
static const uint32_t VARIANT_DWELL_MS = 5000;
static const uint32_t ANIM_PAUSE_MS = 800;
static bool gifOpen = false;

// ── helpers ───────────────────────────────────────────────────────────────────

static uint16_t parseHexColor(const char *s, uint16_t fallback)
{
  if (!s)
    return fallback;
  if (*s == '#')
    s++;
  uint32_t v = strtoul(s, nullptr, 16);
  return (uint16_t)(((v >> 19) & 0x1F) << 11 | ((v >> 10) & 0x3F) << 5 | ((v >> 3) & 0x1F));
}

static void gifPlace()
{
  gifX = (BZ - gifW) / 2;
  gifY = (BH - gifH) / 2 - 40; // shift up to leave room for stats at bottom
  if (gifX < 0)
    gifX = 0;
  if (gifY < 0)
    gifY = 0;
}

// ── AnimatedGIF file callbacks ─────────────────────────────────────────────────

static void *gifOpenCb(const char *fname, int32_t *pSize)
{
  gifFile = LittleFS.open(fname, "r");
  if (!gifFile)
    return nullptr;
  *pSize = gifFile.size();
  return (void *)&gifFile;
}

static void gifCloseCb(void *handle)
{
  File *f = (File *)handle;
  if (f)
    f->close();
}

static int32_t gifReadCb(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
  File *f = (File *)pFile->fHandle;
  int32_t n = f->read(pBuf, iLen);
  pFile->iPos = f->position();
  return n;
}

static int32_t gifSeekCb(GIFFILE *pFile, int32_t iPosition)
{
  File *f = (File *)pFile->fHandle;
  f->seek(iPosition);
  pFile->iPos = (int32_t)f->position();
  return pFile->iPos;
}

// Draw one GIF scanline into the buddy zone of the framebuffer.
// Transparent pixels render as pal.bg so every frame fully paints its region.
// GIFs are pre-processed full-frame (gifsicle --unoptimize) — no disposal needed.
static void gifDrawCb(GIFDRAW *d)
{
  int y = gifY + d->iY + d->y;
  if (y < 0 || y >= BH)
    return;
  uint16_t *pal16 = d->pPalette;
  uint8_t *src = d->pPixels;
  uint8_t t = d->ucTransparent;
  bool hasT = d->ucHasTransparency;
  int x0 = gifX + d->iX;
  int w = d->iWidth;
  if (x0 < 0)
  {
    src -= x0;
    w += x0;
    x0 = 0;
  }
  if (x0 + w > BZ)
    w = BZ - x0;
  if (w <= 0)
    return;
  for (int i = 0; i < w; i++)
    spr.drawPixel(x0 + i, y, (hasT && src[i] == t) ? pal.bg : pal16[src[i]]);
}

// ── Public API ─────────────────────────────────────────────────────────────────

bool characterInit(const char *name)
{
  if (!LittleFS.begin(false))
  {
    if (!LittleFS.open("/"))
    {
      Serial.println("[char] LittleFS mount failed");
      return false;
    }
  }

  // No name → scan /characters/ for the first subdirectory present
  static char scanned[24];
  if (!name)
  {
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory())
    {
      File e = d.openNextFile();
      while (e)
      {
        if (e.isDirectory())
        {
          const char *n = strrchr(e.name(), '/');
          strncpy(scanned, n ? n + 1 : e.name(), sizeof(scanned) - 1);
          scanned[sizeof(scanned) - 1] = 0;
          name = scanned;
          break;
        }
        e = d.openNextFile();
      }
      d.close();
    }
    if (!name)
    {
      Serial.println("[char] no characters installed");
      return false;
    }
  }

  snprintf(basePath, sizeof(basePath), "/characters/%s", name);
  char mpath[64];
  snprintf(mpath, sizeof(mpath), "%s/manifest.json", basePath);

  File mf = LittleFS.open(mpath, "r");
  if (!mf)
  {
    Serial.printf("[char] manifest not found: %s\n", mpath);
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, mf);
  mf.close();
  if (err)
  {
    Serial.printf("[char] manifest parse: %s\n", err.c_str());
    return false;
  }

  JsonObject colors = doc["colors"];
  pal.body = parseHexColor(colors["body"], pal.body);
  pal.bg = parseHexColor(colors["bg"], pal.bg);
  pal.text = parseHexColor(colors["text"], pal.text);
  pal.textDim = parseHexColor(colors["textDim"], pal.textDim);
  pal.ink = parseHexColor(colors["ink"], pal.ink);

  const char *mode = doc["mode"];
  textMode = (mode && strcmp(mode, "text") == 0);

  JsonObject states = doc["states"];

  if (textMode)
  {
    for (uint8_t i = 0; i < N_STATES; i++)
    {
      TextState &ts = textStates[i];
      ts.nFrames = 0;
      ts.delayMs = 200;
      JsonObject st = states[STATE_NAMES[i]];
      if (st.isNull())
        continue;
      ts.delayMs = st["delay"] | 200;
      JsonArray fr = st["frames"];
      for (JsonVariant v : fr)
      {
        if (ts.nFrames >= 8)
          break;
        const char *s = v.as<const char *>();
        strncpy(ts.frames[ts.nFrames], s ? s : "", 19);
        ts.frames[ts.nFrames++][19] = 0;
      }
    }
    loaded = true;
    Serial.printf("[char] loaded '%s' (text mode)\n", name);
    return true;
  }

  gifTotal = 0;
  for (uint8_t i = 0; i < N_STATES; i++)
  {
    stateStart[i] = gifTotal;
    stateCount[i] = 0;
    stateRot[i] = 0;
    JsonVariant v = states[STATE_NAMES[i]];
    if (v.is<JsonArray>())
    {
      for (JsonVariant e : v.as<JsonArray>())
      {
        if (gifTotal >= MAX_GIFS)
          break;
        const char *fn = e.as<const char *>();
        if (fn)
        {
          snprintf(gifPaths[gifTotal], 32, "%s", fn);
          gifTotal++;
          stateCount[i]++;
        }
      }
    }
    else
    {
      const char *fn = v.as<const char *>();
      if (fn)
      {
        snprintf(gifPaths[gifTotal], 32, "%s", fn);
        gifTotal++;
        stateCount[i] = 1;
      }
    }
  }

  gif.begin(LITTLE_ENDIAN_PIXELS);
  loaded = true;
  Serial.printf("[char] loaded '%s' (%u gifs)\n", (const char *)doc["name"], gifTotal);
  return true;
}

bool characterLoaded() { return loaded; }
const Palette &characterPalette() { return pal; }
void characterSetBg(uint16_t bg)
{
  pal.bg = bg;
  spr.fillRect(0, 0, BZ, BH, pal.bg);
}

void characterRenderTo(void *, int, int) {} // not used on this board
void characterSetPeek(bool) {}              // no peek mode on wide landscape display

void characterClose()
{
  if (gifOpen)
  {
    gif.close();
    gifOpen = false;
  }
  loaded = false;
  textMode = false;
  curState = 0xFF;
}

void characterInvalidate()
{
  if (!loaded)
    return;
  if (textMode)
  {
    uint8_t s = curState;
    curState = 0xFF;
    characterSetState(s);
    return;
  }
  if (gifOpen)
  {
    gif.close();
    gifOpen = false;
  }
  animPauseUntil = 0;
  uint8_t s = curState;
  curState = 0xFF;
  characterSetState(s);
}

void characterSetState(uint8_t s)
{
  if (!loaded || s >= N_STATES || s == curState)
    return;

  if (textMode)
  {
    curState = s;
    textFrame = 0;
    textNext = 0;
    return;
  }

  if (gifOpen)
  {
    gif.close();
    gifOpen = false;
  }
  animPauseUntil = 0;
  curState = s;

  if (stateCount[s] == 0)
    return;

  uint8_t idx = stateStart[s] + stateRot[s];
  char full[80];
  snprintf(full, sizeof(full), "%s/%s", basePath, gifPaths[idx]);
  if (gif.open(full, gifOpenCb, gifCloseCb, gifReadCb, gifSeekCb, gifDrawCb))
  {
    gifOpen = true;
    gifW = gif.getCanvasWidth();
    gifH = gif.getCanvasHeight();
    gifPlace();
    spr.fillRect(0, 0, BZ, BH, pal.bg); // clear buddy zone for incoming GIF
    nextFrameAt = 0;
    variantStartedMs = millis();
    Serial.printf("[char] %s: %dx%d @ (%d,%d)\n", gifPaths[idx], gifW, gifH, gifX, gifY);
  }
  else
  {
    Serial.printf("[char] open failed: %s (err %d)\n", full, gif.getLastError());
  }
}

void characterTick()
{
  if (!loaded)
    return;

  if (textMode)
  {
    TextState &ts = textStates[curState];
    if (ts.nFrames == 0)
      return;
    uint32_t now = millis();
    if (now < textNext)
      return;
    textNext = now + ts.delayMs;
    const char *line = ts.frames[textFrame];
    int len = strlen(line);
    spr.setTextColor(pal.body, pal.bg);
    spr.setTextSize(2);
    spr.setCursor((BZ - len * 12) / 2, BH / 2 - 7);
    spr.print(line);
    textFrame = (textFrame + 1) % ts.nFrames;
    return;
  }

  uint32_t now = millis();

  if (!gifOpen)
  {
    if (animPauseUntil && now >= animPauseUntil)
    {
      animPauseUntil = 0;
      uint8_t s = curState;
      curState = 0xFF;
      characterSetState(s);
    }
    return;
  }
  if (now < nextFrameAt)
    return;

  int delayMs = 0;
  if (!gif.playFrame(false, &delayMs))
  {
    // Single-gif states freeze on last frame (avoid expensive re-open loop)
    if (stateCount[curState] == 1)
    {
      gif.close();
      gifOpen = false;
      return;
    }
    // Multi-variant: dwell then rotate to next idle variant
    if (now - variantStartedMs < VARIANT_DWELL_MS)
    {
      gif.reset();
      nextFrameAt = now;
      return;
    }
    gif.close();
    gifOpen = false;
    stateRot[curState] = (stateRot[curState] + 1) % stateCount[curState];
    animPauseUntil = now + ANIM_PAUSE_MS;
    return;
  }
  nextFrameAt = now + (delayMs > 0 ? delayMs : 100);
}
