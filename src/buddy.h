#pragma once
#include <stdint.h>

// Colors defined in buddy.cpp, updated via buddyApplyPalette()
extern uint16_t BUDDY_HEART;
extern uint16_t BUDDY_YEL;
extern uint16_t BUDDY_GREEN;
extern uint16_t BUDDY_RED;

void buddyApplyPalette(uint16_t bg, uint16_t text, uint16_t textDim,
                       uint16_t body, uint16_t hot, uint16_t green);

void buddyInit();
void buddyTick(uint8_t personaState);
void buddyInvalidate();
void buddySetSpecies(const char* name);
void buddySetSpeciesIdx(uint8_t idx);
void buddyNextSpecies();
void buddySetPeek(bool peek);
uint8_t buddySpeciesIdx();
uint8_t buddySpeciesCount();
const char* buddySpeciesName();

typedef void (*StateFn)(uint32_t t);

struct Species {
  const char* name;
  uint16_t bodyColor;
  StateFn states[7];
};
