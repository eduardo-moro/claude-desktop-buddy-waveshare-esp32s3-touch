#pragma once
#include <Arduino.h>
#include <stdarg.h>

// TFT_eSPI datum constants
constexpr uint8_t TL_DATUM = 0;
constexpr uint8_t TC_DATUM = 1;
constexpr uint8_t MC_DATUM = 4;

// Common colors (gruvbox dark)
constexpr uint16_t GREEN  = 0xBDC4;  // #b8bb26 green-dim
constexpr uint16_t RED    = 0xFA46;  // #fb4934 red-dim

// Framebuffer-backed display for Waveshare ESP32-S3 3.49" (AXS15231B, 640x172).
// Provides the subset of TFT_eSprite API used by buddy code, plus a standalone
// U8g2 font decoder that draws directly into the PSRAM framebuffer.
class BuddyDisplay {
 public:
  static constexpr int W = 640;
  static constexpr int H = 172;

  bool begin();
  void flush();
  void pushSprite(int, int) { flush(); }
  void createSprite(int, int) {}
  void fillSprite(uint16_t c) { fillRect(0, 0, W, H, c); }

  void fillRect(int x, int y, int w, int h, uint16_t color);
  void drawRect(int x, int y, int w, int h, uint16_t color);
  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t color);
  void drawRoundRect(int x, int y, int w, int h, int r, uint16_t color);
  void fillCircle(int x, int y, int r, uint16_t color);
  void drawCircle(int x, int y, int r, uint16_t color);
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);
  void drawLine(int x0, int y0, int x1, int y1, uint16_t color);
  void drawFastHLine(int x, int y, int len, uint16_t color);
  void drawPixel(int x, int y, uint16_t color);

  // ── Text API ────────────────────────────────────────────────────────────────
  void setTextSize(uint8_t s)                 { textSize_ = s ? s : 1; }
  void setTextColor(uint16_t fg, uint16_t bg) { textFg_ = fg; textBg_ = bg; }
  void setTextColor(uint16_t fg)              { textFg_ = fg; }
  void setTextDatum(uint8_t d)                { textDatum_ = d; }
  void setCursor(int x, int y)               { curX_ = x; curY_ = y; }

  void print(char c);
  void print(const char* s);
  void print(int n);
  void print(unsigned long n);
  void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  void drawString(const char* s, int x, int y);

  int textWidth(const char* s) const;
  int fontHeight() const;

  // ── U8g2 font API ──────────────────────────────────────────────────────────
  // setFont(nullptr) reverts to the built-in 5×7 bitmap font (used by buddy art).
  // setFont(u8g2_font_*) enables the U8g2 decoder for all subsequent print calls.
  // setFontFallback() is tried when a glyph is missing from the primary font
  // (use it for a symbols font to cover dingbats/special chars).
  void setFont(const uint8_t* font)         { u8Font_ = font; }
  void setFontFallback(const uint8_t* font) { u8FontFB_ = font; }
  void clearFont()                          { u8Font_ = nullptr; u8FontFB_ = nullptr; }

  int getFontAscent() const;   // pixels above baseline for capital A
  int getFontHeight() const;   // max glyph height
  int getStrWidth(const char* s) const;  // pixel width with current font

 private:
  uint16_t* buf_       = nullptr;
  uint16_t* txBuf_     = nullptr;
  uint8_t   textSize_  = 1;
  uint16_t  textFg_    = 0xFFFF;
  uint16_t  textBg_    = 0x0000;
  uint8_t   textDatum_ = TL_DATUM;
  int       curX_      = 0;
  int       curY_      = 0;

  // U8g2 font pointers (null = bitmap mode)
  const uint8_t* u8Font_   = nullptr;
  const uint8_t* u8FontFB_ = nullptr;

  void drawChar(int x, int y, char c);
  void pixel(int x, int y, uint16_t color);

  // U8g2 decoder internals
  struct U8Dec { const uint8_t* ptr; uint8_t bits; };
  static uint8_t  u8Bits(U8Dec& d, uint8_t cnt);
  static int8_t   u8SBits(U8Dec& d, uint8_t cnt);

  const uint8_t* u8FindGlyph(const uint8_t* font, uint16_t enc) const;
  int8_t u8GlyphAdvance(const uint8_t* font, uint16_t enc) const;
  int8_t u8DrawGlyph(const uint8_t* font, uint16_t enc, int x, int y);
  void   u8DrawLen(int gx, int gy, uint8_t& lx, uint8_t& ly,
                   uint8_t gw, uint8_t len, uint16_t color);

  // UTF-8 codepoint iterator
  static uint16_t utf8Next(const char** s);
};

extern BuddyDisplay spr;
