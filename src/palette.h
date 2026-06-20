#pragma once
// Header-only, include from exactly one translation unit (main.cpp).
#include <Arduino.h>
#include <Preferences.h>
#include "character.h"

// ── Gruvbox dark defaults ────────────────────────────────────────────────────
// body=yellow, bg=#282828, text=fg1, textDim=gray, ink=orange
static const Palette _PAL_DEFAULT  = {0xFDE5, 0x2945, 0xEED6, 0xACD0, 0xFC03};
static constexpr uint16_t _GREEN_DEFAULT = 0xBDC4; // #b8bb26
static constexpr uint16_t _HOT_DEFAULT   = 0xFA46; // #fb4934
static constexpr uint16_t _PANEL_DEFAULT = 0x39C6; // #3c3836

static Palette  _livePal   = _PAL_DEFAULT;
static uint16_t _liveGreen = _GREEN_DEFAULT;
static uint16_t _liveHot   = _HOT_DEFAULT;
static uint16_t _livePanel = _PANEL_DEFAULT;

static Preferences _palPrefs;

inline void paletteLoad() {
  _palPrefs.begin("pal", true);
  bool saved = _palPrefs.getBool("saved", false);
  if (saved) {
    _livePal.body    = _palPrefs.getUShort("body",  _PAL_DEFAULT.body);
    _livePal.bg      = _palPrefs.getUShort("bg",    _PAL_DEFAULT.bg);
    _livePal.text    = _palPrefs.getUShort("text",  _PAL_DEFAULT.text);
    _livePal.textDim = _palPrefs.getUShort("tdim",  _PAL_DEFAULT.textDim);
    _livePal.ink     = _palPrefs.getUShort("ink",   _PAL_DEFAULT.ink);
    _liveGreen       = _palPrefs.getUShort("green", _GREEN_DEFAULT);
    _liveHot         = _palPrefs.getUShort("hot",   _HOT_DEFAULT);
    _livePanel       = _palPrefs.getUShort("panel", _PANEL_DEFAULT);
  }
  _palPrefs.end();
}

inline void paletteSave() {
  _palPrefs.begin("pal", false);
  _palPrefs.putBool("saved",   true);
  _palPrefs.putUShort("body",  _livePal.body);
  _palPrefs.putUShort("bg",    _livePal.bg);
  _palPrefs.putUShort("text",  _livePal.text);
  _palPrefs.putUShort("tdim",  _livePal.textDim);
  _palPrefs.putUShort("ink",   _livePal.ink);
  _palPrefs.putUShort("green", _liveGreen);
  _palPrefs.putUShort("hot",   _liveHot);
  _palPrefs.putUShort("panel", _livePanel);
  _palPrefs.end();
}

inline const Palette& uiPal()   { return _livePal; }
inline uint16_t       uiGreen() { return _liveGreen; }
inline uint16_t       uiHot()   { return _liveHot; }
inline uint16_t       uiPanel() { return _livePanel; }
inline uint16_t       uiInk()   { return _livePal.ink; }

// Apply values received from TamaState palette fields (set by data.h on cmd=palette)
inline void paletteApply(uint16_t body, uint16_t bg, uint16_t text, uint16_t textDim,
                         uint16_t ink, uint16_t green, uint16_t hot, uint16_t panel) {
  _livePal.body    = body;
  _livePal.bg      = bg;
  _livePal.text    = text;
  _livePal.textDim = textDim;
  _livePal.ink     = ink;
  _liveGreen       = green;
  _liveHot         = hot;
  _livePanel       = panel;
  paletteSave();
}
