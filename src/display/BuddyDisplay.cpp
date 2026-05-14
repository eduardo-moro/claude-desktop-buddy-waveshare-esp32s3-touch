#include "display/BuddyDisplay.h"
#include "display/axs15231b.h"
#include "display/Font5x7.h"
#include <esp_heap_caps.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdarg>

BuddyDisplay spr;

// Physical panel: 172 wide × 640 tall (portrait MADCTL=0x00).
// Logical layout: 640 wide × 172 tall (landscape, 180° software flip).
// LandscapeFlipped: logicalX = nativeY, logicalY = (H-1) - nativeX
static constexpr int kNativeW   = 172;
static constexpr int kNativeH   = 640;
static constexpr int kChunkRows = 16;
static constexpr int kTxBufPx   = kNativeW * kChunkRows;

bool BuddyDisplay::begin() {
  buf_   = (uint16_t*)heap_caps_malloc(W * H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  txBuf_ = (uint16_t*)heap_caps_malloc(kTxBufPx * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  if (!buf_ || !txBuf_) return false;
  memset(buf_, 0, W * H * sizeof(uint16_t));
  axs15231bInit();
  axs15231bSetBrightnessPercent(80);
  axs15231bSetBacklight(true);
  return true;
}

void BuddyDisplay::flush() {
  for (int nativeYStart = 0; nativeYStart < kNativeH; nativeYStart += kChunkRows) {
    int nativeRows = std::min(kChunkRows, kNativeH - nativeYStart);
    for (int localNY = 0; localNY < nativeRows; localNY++) {
      int nativeY = nativeYStart + localNY;
      uint16_t* dst = txBuf_ + localNY * kNativeW;
      for (int nativeX = 0; nativeX < kNativeW; nativeX++) {
        dst[nativeX] = __builtin_bswap16(buf_[((H - 1) - nativeX) * W + nativeY]);
      }
    }
    axs15231bPushColors(0, nativeYStart, kNativeW, nativeRows, txBuf_);
  }
}

void BuddyDisplay::pixel(int x, int y, uint16_t color) {
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  buf_[y * W + x] = color;
}

void BuddyDisplay::drawPixel(int x, int y, uint16_t color) { pixel(x, y, color); }

void BuddyDisplay::fillRect(int x, int y, int w, int h, uint16_t color) {
  int x1 = std::max(0, x), y1 = std::max(0, y);
  int x2 = std::min(W, x + w), y2 = std::min(H, y + h);
  for (int iy = y1; iy < y2; iy++)
    for (int ix = x1; ix < x2; ix++)
      buf_[iy * W + ix] = color;
}

void BuddyDisplay::drawRect(int x, int y, int w, int h, uint16_t color) {
  drawFastHLine(x, y, w, color);
  drawFastHLine(x, y+h-1, w, color);
  for (int iy = y; iy < y+h; iy++) { pixel(x, iy, color); pixel(x+w-1, iy, color); }
}

void BuddyDisplay::drawFastHLine(int x, int y, int len, uint16_t color) {
  for (int i = 0; i < len; i++) pixel(x+i, y, color);
}

void BuddyDisplay::drawLine(int x0, int y0, int x1, int y1, uint16_t color) {
  int dx = abs(x1-x0), dy = abs(y1-y0);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
  while (true) {
    pixel(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 > -dy) { err -= dy; x0 += sx; }
    if (e2 <  dx) { err += dx; y0 += sy; }
  }
}

void BuddyDisplay::fillCircle(int cx, int cy, int r, uint16_t color) {
  for (int y = -r; y <= r; y++)
    for (int x = -r; x <= r; x++)
      if (x*x + y*y <= r*r) pixel(cx+x, cy+y, color);
}

void BuddyDisplay::drawCircle(int cx, int cy, int r, uint16_t color) {
  int x = r, y = 0, d = 1 - r;
  while (x >= y) {
    pixel(cx+x,cy+y,color); pixel(cx-x,cy+y,color);
    pixel(cx+x,cy-y,color); pixel(cx-x,cy-y,color);
    pixel(cx+y,cy+x,color); pixel(cx-y,cy+x,color);
    pixel(cx+y,cy-x,color); pixel(cx-y,cy-x,color);
    y++; if (d < 0) d += 2*y+1; else { x--; d += 2*(y-x)+1; }
  }
}

void BuddyDisplay::fillRoundRect(int x, int y, int w, int h, int r, uint16_t color) {
  fillRect(x+r, y, w-2*r, h, color);
  fillRect(x, y+r, r, h-2*r, color);
  fillRect(x+w-r, y+r, r, h-2*r, color);
  fillCircle(x+r,   y+r,   r, color);
  fillCircle(x+w-r-1, y+r,   r, color);
  fillCircle(x+r,   y+h-r-1, r, color);
  fillCircle(x+w-r-1, y+h-r-1, r, color);
}

void BuddyDisplay::drawRoundRect(int x, int y, int w, int h, int r, uint16_t color) {
  drawFastHLine(x+r, y, w-2*r, color);
  drawFastHLine(x+r, y+h-1, w-2*r, color);
  for (int iy = y+r; iy < y+h-r; iy++) { pixel(x, iy, color); pixel(x+w-1, iy, color); }
  drawCircle(x+r,     y+r,     r, color);
  drawCircle(x+w-r-1, y+r,     r, color);
  drawCircle(x+r,     y+h-r-1, r, color);
  drawCircle(x+w-r-1, y+h-r-1, r, color);
}

void BuddyDisplay::fillTriangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t color) {
  if (y0 > y1) { std::swap(x0,x1); std::swap(y0,y1); }
  if (y1 > y2) { std::swap(x1,x2); std::swap(y1,y2); }
  if (y0 > y1) { std::swap(x0,x1); std::swap(y0,y1); }
  for (int y = y0; y <= y2; y++) {
    int xa, xb;
    if (y < y1) {
      xa = (y0==y1) ? x0 : x0 + (x1-x0)*(y-y0)/(y1-y0);
      xb = (y0==y2) ? x0 : x0 + (x2-x0)*(y-y0)/(y2-y0);
    } else {
      xa = (y1==y2) ? x1 : x1 + (x2-x1)*(y-y1)/(y2-y1);
      xb = (y0==y2) ? x0 : x0 + (x2-x0)*(y-y0)/(y2-y0);
    }
    if (xa > xb) std::swap(xa, xb);
    for (int x = xa; x <= xb; x++) pixel(x, y, color);
  }
}

// ── Bitmap font (5×7) ────────────────────────────────────────────────────────

void BuddyDisplay::drawChar(int x, int y, char c) {
  if (c < 32 || c > 126) c = '?';
  const uint8_t* glyph = kFont5x7[c - 32];
  int s = textSize_;
  for (int col = 0; col < 5; col++) {
    uint8_t colData = glyph[col];
    for (int row = 0; row < 7; row++) {
      uint16_t color = (colData >> row) & 1 ? textFg_ : textBg_;
      for (int sy = 0; sy < s; sy++)
        for (int sx = 0; sx < s; sx++)
          pixel(x + col*s + sx, y + row*s + sy, color);
    }
  }
  // Gap column
  for (int row = 0; row < 7 * s; row++)
    pixel(x + 5*s, y + row, textBg_);
}

// ── U8g2 decoder ─────────────────────────────────────────────────────────────
// Self-contained port of the U8g2 RLE glyph decoder.
// Reads font data directly (ESP32 has no PROGMEM distinction) and writes to
// the PSRAM framebuffer via pixel(). No U8g2 display driver needed.

// Read 'cnt' unsigned bits from the bit stream
uint8_t BuddyDisplay::u8Bits(U8Dec& d, uint8_t cnt) {
  uint8_t val = *d.ptr >> d.bits;
  uint8_t bpc = d.bits + cnt;
  if (bpc >= 8) { d.ptr++; val |= *d.ptr << (8 - d.bits); bpc -= 8; }
  val &= (1u << cnt) - 1;
  d.bits = bpc;
  return val;
}

// Read 'cnt' signed bits
int8_t BuddyDisplay::u8SBits(U8Dec& d, uint8_t cnt) {
  return (int8_t)u8Bits(d, cnt) - (int8_t)(1 << (cnt - 1));
}

// UTF-8 → codepoint, advances *s past the consumed bytes. Returns 0 at end.
uint16_t BuddyDisplay::utf8Next(const char** s) {
  uint8_t c = (uint8_t)**s;
  if (!c) return 0;
  (*s)++;
  if (c < 0x80) return c;
  if ((c & 0xE0) == 0xC0) {
    uint16_t cp = (c & 0x1F) << 6;
    if (**s) { cp |= (uint8_t)**s & 0x3F; (*s)++; }
    return cp;
  }
  if ((c & 0xF0) == 0xE0) {
    uint16_t cp = (uint16_t)(c & 0x0F) << 12;
    if (**s) { cp |= ((uint8_t)**s & 0x3F) << 6; (*s)++; }
    if (**s) { cp |= (uint8_t)**s & 0x3F;         (*s)++; }
    return cp;
  }
  return '?';
}

// Font header helpers
static inline uint8_t  fB(const uint8_t* f, int i) { return f[i]; }
static inline uint16_t fW(const uint8_t* f, int i)  { return ((uint16_t)f[i]<<8)|f[i+1]; }

// Find glyph data pointer for 'enc' in 'font'. Returns null if not present.
// U8g2 font structure (23-byte header, then ASCII section, then Unicode section):
//   ASCII glyphs: [codepoint(1), size(1), bitmap_data...]
//   Unicode glyphs: [codepoint(2), size(1), bitmap_data...]
const uint8_t* BuddyDisplay::u8FindGlyph(const uint8_t* font, uint16_t enc) const {
  const uint8_t* f = font + 23;  // skip header

  if (enc <= 255) {
    // Jump to the right ASCII sub-section via the two start offsets
    if (enc >= 'a') f += fW(font, 19);
    else if (enc >= 'A') f += fW(font, 17);

    for (;;) {
      if (f[1] == 0) return nullptr;         // end-of-section sentinel
      if (f[0] == (uint8_t)enc) return f + 2; // skip codepoint + size
      f += f[1];
    }
  } else {
    // Unicode section: lookup table of 4-byte entries [offset(2), maxEnc(2)]
    f += fW(font, 21);
    const uint8_t* lut = f;
    do {
      f += fW(lut, 0);
      uint16_t maxEnc = fW(lut, 2);
      lut += 4;
      if (maxEnc >= enc) break;
    } while (true);

    // Linear scan within the section
    for (;;) {
      uint16_t e = ((uint16_t)f[0] << 8) | f[1];
      if (e == 0) return nullptr;
      if (e == enc) return f + 3;  // skip codepoint(2) + size(1)
      f += f[2];
    }
  }
}

// Draw 'len' pixels into the glyph scanline, wrapping at glyph width.
// (lx, ly) tracks position within the glyph bounding box.
void BuddyDisplay::u8DrawLen(int gx, int gy, uint8_t& lx, uint8_t& ly,
                              uint8_t gw, uint8_t len, uint16_t color) {
  while (len > 0) {
    uint8_t rem = gw - lx;
    uint8_t cur = len < rem ? len : rem;
    for (int i = 0; i < cur; i++) pixel(gx + lx + i, gy + ly, color);
    len -= cur;
    lx  += cur;
    if (lx >= gw) { lx = 0; ly++; }
  }
}

// Decode and draw one glyph. x, y = baseline position.
// Returns the x-advance (number of pixels to move cursor right).
int8_t BuddyDisplay::u8DrawGlyph(const uint8_t* font, uint16_t enc, int x, int y) {
  const uint8_t* gd = u8FindGlyph(font, enc);
  if (!gd) return 0;

  // Font header fields
  uint8_t bp0 = fB(font, 2), bp1 = fB(font, 3);
  uint8_t bpw = fB(font, 4), bph = fB(font, 5);
  uint8_t bpx = fB(font, 6), bpy = fB(font, 7), bpd = fB(font, 8);

  U8Dec d = {gd, 0};
  uint8_t w  = u8Bits(d, bpw);
  uint8_t h  = u8Bits(d, bph);
  int8_t  ox = u8SBits(d, bpx);
  int8_t  oy = u8SBits(d, bpy);
  int8_t  adv = u8SBits(d, bpd);

  if (w == 0) return adv;

  // Glyph top-left in screen space.
  // x, y is the baseline; u8g2 places top at baseline - h - oy.
  int gx = x + ox;
  int gy = y - h - oy;

  uint8_t lx = 0, ly = 0;
  for (;;) {
    uint8_t a = u8Bits(d, bp0);  // background run length
    uint8_t b = u8Bits(d, bp1);  // foreground run length
    do {
      u8DrawLen(gx, gy, lx, ly, w, a, textBg_);
      u8DrawLen(gx, gy, lx, ly, w, b, textFg_);
    } while (u8Bits(d, 1) != 0);
    if (ly >= h) break;
  }
  return adv;
}

// Return x-advance for a glyph without drawing it (for string width measurement).
int8_t BuddyDisplay::u8GlyphAdvance(const uint8_t* font, uint16_t enc) const {
  const uint8_t* gd = u8FindGlyph(font, enc);
  if (!gd) return 0;
  U8Dec d = {gd, 0};
  u8Bits(d,  fB(font, 4));   // skip w
  u8Bits(d,  fB(font, 5));   // skip h
  u8SBits(d, fB(font, 6));   // skip ox
  u8SBits(d, fB(font, 7));   // skip oy
  return u8SBits(d, fB(font, 8));
}

// ── Text API ─────────────────────────────────────────────────────────────────

int BuddyDisplay::getFontAscent() const {
  if (!u8Font_) return 7 * textSize_;
  return (int8_t)fB(u8Font_, 13);  // ascent_A
}

int BuddyDisplay::getFontHeight() const {
  if (!u8Font_) return 8 * textSize_;
  return fB(u8Font_, 10);  // max_char_height
}

int BuddyDisplay::fontHeight() const { return getFontHeight(); }

int BuddyDisplay::getStrWidth(const char* s) const {
  if (!s) return 0;
  if (!u8Font_) return strlen(s) * 6 * textSize_;

  int w = 0;
  const char* p = s;
  uint16_t cp;
  while ((cp = utf8Next(&p)) != 0) {
    int8_t adv = u8GlyphAdvance(u8Font_, cp);
    if (adv == 0 && u8FontFB_) adv = u8GlyphAdvance(u8FontFB_, cp);
    w += adv;
  }
  return w;
}

int BuddyDisplay::textWidth(const char* s) const { return getStrWidth(s); }

void BuddyDisplay::print(char c) {
  if (u8Font_) {
    // Single byte — treat as Latin-1 (valid for ASCII + Latin-1 Supplement)
    const char tmp[2] = {c, 0};
    const char* p = tmp;
    uint16_t cp = utf8Next(&p);
    int8_t adv = u8DrawGlyph(u8Font_, cp, curX_, curY_ + getFontAscent());
    if (adv == 0 && u8FontFB_)
      adv = u8DrawGlyph(u8FontFB_, cp, curX_, curY_ + getFontAscent());
    curX_ += adv;
    return;
  }
  if (c == '\n') { curX_ = 0; curY_ += 8 * textSize_; return; }
  drawChar(curX_, curY_, c);
  curX_ += 6 * textSize_;
}

void BuddyDisplay::print(const char* s) {
  if (!s) return;
  if (u8Font_) {
    // UTF-8 aware path
    const char* p = s;
    uint16_t cp;
    int ascent = getFontAscent();
    while ((cp = utf8Next(&p)) != 0) {
      if (cp == '\n') { curX_ = 0; curY_ += getFontHeight(); continue; }
      int8_t adv = u8DrawGlyph(u8Font_, cp, curX_, curY_ + ascent);
      if (adv == 0 && u8FontFB_)
        adv = u8DrawGlyph(u8FontFB_, cp, curX_, curY_ + ascent);
      curX_ += adv;
    }
    return;
  }
  while (*s) print(*s++);
}

void BuddyDisplay::print(int n) {
  char buf[16]; snprintf(buf, sizeof(buf), "%d", n); print(buf);
}

void BuddyDisplay::print(unsigned long n) {
  char buf[16]; snprintf(buf, sizeof(buf), "%lu", n); print(buf);
}

void BuddyDisplay::printf(const char* fmt, ...) {
  char buf[128];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  print(buf);
}

void BuddyDisplay::drawString(const char* s, int x, int y) {
  int tw = getStrWidth(s);
  int th = getFontHeight();
  int ox = x, oy = y;
  switch (textDatum_) {
    case MC_DATUM: ox = x - tw/2; oy = y - th/2; break;
    case TC_DATUM: ox = x - tw/2; oy = y;         break;
    default: break;
  }
  curX_ = ox; curY_ = oy;
  print(s);
}
