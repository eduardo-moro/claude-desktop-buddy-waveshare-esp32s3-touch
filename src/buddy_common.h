#pragma once
#include <stdint.h>

// Shared geometry/colors (defined in buddy.cpp)
extern const int BUDDY_X_CENTER;
extern const int BUDDY_CANVAS_W;
extern const int BUDDY_Y_BASE;
extern const int BUDDY_Y_OVERLAY;
extern const int BUDDY_CHAR_W;
extern const int BUDDY_CHAR_H;

extern uint16_t BUDDY_BG;
extern uint16_t BUDDY_HEART;
extern uint16_t BUDDY_DIM;
extern uint16_t BUDDY_YEL;
extern uint16_t BUDDY_WHITE;
extern uint16_t BUDDY_CYAN;
extern uint16_t BUDDY_GREEN;
extern uint16_t BUDDY_PURPLE;
extern uint16_t BUDDY_RED;
extern uint16_t BUDDY_BLUE;

void buddyApplyPalette(uint16_t bg, uint16_t text, uint16_t textDim,
                       uint16_t body, uint16_t hot, uint16_t green);

void buddyPrintLine(const char* line, int yPx, uint16_t color, int xOff = 0);
void buddyPrintSprite(const char* const* lines, uint8_t nLines, int yOffset, uint16_t color, int xOff = 0);
void buddySetCursor(int x, int y);
void buddySetColor(uint16_t fg);
void buddyPrint(const char* s);
