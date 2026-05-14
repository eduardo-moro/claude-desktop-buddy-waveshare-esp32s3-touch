#include "buddy.h"
#include "buddy_common.h"
#include "display/BuddyDisplay.h"
#include <string.h>

// Mirrors PersonaState in main.cpp
enum { B_SLEEP, B_IDLE, B_BUSY, B_ATTENTION, B_CELEBRATE, B_DIZZY, B_HEART };

// ── shared geometry ──────────────────────────────────────────────────────────
// Buddy renders in the left 200px zone of the 640×172 display at 2× scale.
const int BUDDY_X_CENTER  = 86;
const int BUDDY_CANVAS_W  = 172;
const int BUDDY_Y_BASE    = 30;
const int BUDDY_Y_OVERLAY = 6;
const int BUDDY_CHAR_W    = 6;
const int BUDDY_CHAR_H    = 8;

// ── shared colors — Gruvbox dark (RGB565) ────────────────────────────────────
const uint16_t BUDDY_BG     = 0x2945;  // #282828 bg
const uint16_t BUDDY_HEART  = 0xFA46;  // #fb4934 red-dim
const uint16_t BUDDY_DIM    = 0xACD0;  // #a89984 fg4
const uint16_t BUDDY_YEL    = 0xFDE5;  // #fabd2f yellow-dim
const uint16_t BUDDY_WHITE  = 0xEED6;  // #ebdbb2 fg
const uint16_t BUDDY_CYAN   = 0x8E0F;  // #8ec07c aqua-dim
const uint16_t BUDDY_GREEN  = 0xBDC4;  // #b8bb26 green-dim
const uint16_t BUDDY_PURPLE = 0xD433;  // #d3869b purple-dim
const uint16_t BUDDY_RED    = 0xFA46;  // #fb4934 red-dim
const uint16_t BUDDY_BLUE   = 0x8533;  // #83a598 blue-dim

static uint8_t _scale = 2;

void buddyPrintLine(const char* line, int yPx, uint16_t color, int xOff) {
  int len = strlen(line);
  if (_scale > 1) {
    while (len && line[len-1] == ' ') len--;
    while (len && *line == ' ')       { line++; len--; }
  }
  int w = len * BUDDY_CHAR_W * _scale;
  int x = BUDDY_X_CENTER - w / 2 + xOff * _scale;
  spr.setTextColor(color, BUDDY_BG);
  spr.setTextSize(_scale);
  spr.setCursor(x, yPx);
  for (int i = 0; i < len; i++) spr.print(line[i]);
}

void buddyPrintSprite(const char* const* lines, uint8_t nLines, int yOffset, uint16_t color, int xOff) {
  spr.setTextSize(_scale);
  int yBase = BUDDY_Y_BASE * _scale - (_scale - 1) * 14;
  for (uint8_t i = 0; i < nLines; i++)
    buddyPrintLine(lines[i], yBase + (yOffset + i * BUDDY_CHAR_H) * _scale, color, xOff);
}

void buddySetCursor(int x, int y) {
  spr.setCursor(BUDDY_X_CENTER + (x - BUDDY_X_CENTER) * _scale, y * _scale);
}
void buddySetColor(uint16_t fg)   { spr.setTextColor(fg, BUDDY_BG); }
void buddyPrint(const char* s)    { spr.setTextSize(_scale); spr.print(s); }

// ── species registry ─────────────────────────────────────────────────────────
extern const Species CAPYBARA_SPECIES;
extern const Species DUCK_SPECIES;
extern const Species GOOSE_SPECIES;
extern const Species BLOB_SPECIES;
extern const Species CAT_SPECIES;
extern const Species DRAGON_SPECIES;
extern const Species OCTOPUS_SPECIES;
extern const Species OWL_SPECIES;
extern const Species PENGUIN_SPECIES;
extern const Species TURTLE_SPECIES;
extern const Species SNAIL_SPECIES;
extern const Species GHOST_SPECIES;
extern const Species AXOLOTL_SPECIES;
extern const Species CACTUS_SPECIES;
extern const Species ROBOT_SPECIES;
extern const Species RABBIT_SPECIES;
extern const Species MUSHROOM_SPECIES;
extern const Species CHONK_SPECIES;

static const Species* SPECIES_TABLE[] = {
  &CAPYBARA_SPECIES, &DUCK_SPECIES, &GOOSE_SPECIES, &BLOB_SPECIES,
  &CAT_SPECIES, &DRAGON_SPECIES, &OCTOPUS_SPECIES, &OWL_SPECIES,
  &PENGUIN_SPECIES, &TURTLE_SPECIES, &SNAIL_SPECIES, &GHOST_SPECIES,
  &AXOLOTL_SPECIES, &CACTUS_SPECIES, &ROBOT_SPECIES, &RABBIT_SPECIES,
  &MUSHROOM_SPECIES, &CHONK_SPECIES,
};
static const uint8_t N_SPECIES = sizeof(SPECIES_TABLE) / sizeof(SPECIES_TABLE[0]);
static uint8_t currentSpeciesIdx = 0;

// ── tick state ───────────────────────────────────────────────────────────────
static uint32_t tickCount  = 0;
static uint32_t nextTickAt = 0;
static const uint32_t TICK_MS = 200;

#include "stats.h"

void buddyInit() {
  tickCount = 0; nextTickAt = 0;
  uint8_t saved = speciesIdxLoad();
  if (saved < N_SPECIES) currentSpeciesIdx = saved;
}

void buddySetSpeciesIdx(uint8_t idx)  { if (idx < N_SPECIES) currentSpeciesIdx = idx; }
void buddySetSpecies(const char* name) {
  for (uint8_t i = 0; i < N_SPECIES; i++)
    if (strcmp(SPECIES_TABLE[i]->name, name) == 0) { currentSpeciesIdx = i; return; }
}
const char* buddySpeciesName()        { return SPECIES_TABLE[currentSpeciesIdx]->name; }
uint8_t buddySpeciesCount()           { return N_SPECIES; }
uint8_t buddySpeciesIdx()             { return currentSpeciesIdx; }
void buddyNextSpecies() {
  currentSpeciesIdx = (currentSpeciesIdx + 1) % N_SPECIES;
  speciesIdxSave(currentSpeciesIdx);
}

static uint8_t lastDrawnState   = 0xFF;
static uint8_t lastDrawnSpecies = 0xFF;
void buddyInvalidate() { lastDrawnState = 0xFF; }
void buddySetPeek(bool) {} // no-op on this board — always full view

void buddyTick(uint8_t personaState) {
  uint32_t now = millis();
  bool ticked = false;
  if ((int32_t)(now - nextTickAt) >= 0) {
    nextTickAt = now + TICK_MS; tickCount++; ticked = true;
  }
  if (personaState >= 7) personaState = B_IDLE;
  // Always render — caller (main loop) clears the full framebuffer before
  // calling buddyTick, so we must redraw every frame.
  // Advance tick counter only when the interval fires.
  (void)lastDrawnState; (void)lastDrawnSpecies;

  const Species* sp = SPECIES_TABLE[currentSpeciesIdx];
  if (sp->states[personaState]) sp->states[personaState](tickCount);
}
