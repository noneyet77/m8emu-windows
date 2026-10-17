#ifndef M8_UI_H
#define M8_UI_H
#include <SDL3/SDL.h>

/* Host screens in the firmware's look: M8 default theme colors, m8c's small
 * font on the 8x10 cell grid of the 320x240 canvas, and the bracket cursor. */
enum { M8_UI_BACKGROUND = 0x000000, M8_UI_EMPTY = 0x202830, M8_UI_INFO = 0x485060, M8_UI_TEXT = 0x90b8b8,
       M8_UI_VALUE = 0xf8fcf8, M8_UI_TITLE = 0xf82040, M8_UI_SELECTED = 0x00fcf8, M8_UI_CURSOR = 0x00dcd8,
       M8_UI_PLAY = 0x00fc60 };
enum { M8_UI_COLUMNS = 40, M8_UI_CELL_W = 8, M8_UI_CELL_H = 10 };

/* Render into a cached canvas-sized texture, with the small font selected.
 * false means nothing may be drawn; end() restores m8c's font and target and
 * composites the texture. Call both from m8c's overlay hook. */
bool m8_ui_begin(SDL_Renderer *renderer, SDL_Texture **texture, int width, int height);
void m8_ui_end(void);
void m8_ui_text(int col, int row, const char *text, Uint32 color);
void m8_ui_fill(int x, int y, int w, int h, Uint32 color);
/* The firmware cursor: four stepped corners around a field of width cells. */
void m8_ui_cursor(int col, int row, int width);
/* Text in SELECTED with the cursor when selected, else in color. */
void m8_ui_field(int col, int row, const char *value, bool selected, Uint32 color);
/* Pixel offset of glyphs inside a cell, for bars drawn beside text. */
int m8_ui_text_y(void);
#endif
