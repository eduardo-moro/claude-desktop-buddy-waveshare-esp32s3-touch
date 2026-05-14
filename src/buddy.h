#pragma once
#include <stdint.h>

// Colors defined in buddy.cpp, used by stat bar rendering in main.cpp
extern const uint16_t BUDDY_HEART;
extern const uint16_t BUDDY_YEL;
extern const uint16_t BUDDY_GREEN;
extern const uint16_t BUDDY_RED;

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
