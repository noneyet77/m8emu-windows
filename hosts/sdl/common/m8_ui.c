#include "m8_ui.h"
#include "SDL2_inprint.h"
#include "fonts/fonts.h"
#include <string.h>

static struct {
    SDL_Renderer *renderer;
    SDL_Texture *target, *texture;
    const struct inline_font *previous, *small;
    int text_y;
} ui;

bool m8_ui_begin(SDL_Renderer *renderer, SDL_Texture **texture, int width, int height) {
    float tw = 0, th = 0;
    if (*texture && (!SDL_GetTextureSize(*texture, &tw, &th) || (int)tw != width || (int)th != height)) {
        SDL_DestroyTexture(*texture); *texture = NULL;
    }
    if (!*texture) {
        *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, width, height);
        if (!*texture) return false;
        SDL_SetTextureBlendMode(*texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(*texture, SDL_SCALEMODE_NEAREST);
    }
    ui.previous = inline_font_get_current(); ui.small = fonts_get(0);
    if (!ui.previous) return false;
    ui.renderer = renderer; ui.texture = *texture;
    ui.target = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, *texture);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    if (ui.previous != ui.small) { inline_font_close(); inline_font_initialize(ui.small); }
    ui.text_y = ui.small->text_offset_y;
    return true;
}

void m8_ui_end(void) {
    if (ui.previous != ui.small) { inline_font_close(); inline_font_initialize(ui.previous); }
    /* inprint caches the last color; resync it with the font texture. */
    inprint(ui.renderer, " ", 0, 0, 0x010101, 0x010101);
    SDL_SetRenderTarget(ui.renderer, ui.target);
    SDL_RenderTexture(ui.renderer, ui.texture, NULL, NULL);
}

void m8_ui_text(int col, int row, const char *s, Uint32 color) {
    char glyph[2] = {0};
    incolor(color);
    for (; *s && col < M8_UI_COLUMNS; ++s, ++col) {
        /* The font covers printable ASCII; m8c prints letters in capitals. */
        glyph[0] = *s >= 'a' && *s <= 'z' ? (char)(*s - 32) : *s > ' ' && *s < 127 ? *s : ' ';
        if (glyph[0] != ' ')
            inprint(ui.renderer, glyph, (Uint32)(col * M8_UI_CELL_W), (Uint32)(row * M8_UI_CELL_H + ui.text_y), color, color);
    }
}

void m8_ui_fill(int x, int y, int w, int h, Uint32 color) {
    SDL_FRect r = {(float)x, (float)y, (float)w, (float)h};
    SDL_SetRenderDrawColor(ui.renderer, (Uint8)(color >> 16), (Uint8)(color >> 8), (Uint8)color, 0xff);
    SDL_RenderFillRect(ui.renderer, &r);
}

void m8_ui_cursor(int col, int row, int width) {
    int x0 = col * M8_UI_CELL_W - 4, x1 = (col + (width < 1 ? 1 : width)) * M8_UI_CELL_W;
    int y0 = row * M8_UI_CELL_H, y1 = y0 + 12;
    for (int i = 0; i < 3; ++i) {
        m8_ui_fill(x0, y0 + i, 3 - i, 1, M8_UI_CURSOR); m8_ui_fill(x1 - 2 + i, y0 + i, 3 - i, 1, M8_UI_CURSOR);
        m8_ui_fill(x0, y1 - i, 3 - i, 1, M8_UI_CURSOR); m8_ui_fill(x1 - 2 + i, y1 - i, 3 - i, 1, M8_UI_CURSOR);
    }
}

void m8_ui_field(int col, int row, const char *value, bool selected, Uint32 color) {
    m8_ui_text(col, row, value, selected ? M8_UI_SELECTED : color);
    if (selected) m8_ui_cursor(col, row, (int)strlen(value));
}

int m8_ui_text_y(void) { return ui.text_y; }
