#pragma once
// What the game-pixel side of the frontend reads from the player's ROM, so it ships no game art:
// the dialogue font (the touch overlay's button labels) and each game's title-screen colours.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UI_W 160
#define UI_H 144
#define UI_GLYPH_W 8
#define UI_GLYPH_H 16

typedef struct { uint8_t r, g, b; } UiColor;
typedef struct { uint8_t glyph[128][UI_GLYPH_H]; bool loaded; } UiFont;

// A game's menu colours, taken from its title screen's background palettes (PALH_03, paletteData4018:
// bank $16 in Seasons, $17 in Ages), so each game's menus carry its own look.
typedef struct { UiColor bg, panel, border, text, dim, highlight, accent; } UiTheme;
void ui_theme_default(UiTheme *t, bool ages);
bool ui_theme_load(UiTheme *t, const uint8_t *rom, size_t rom_size, bool ages);

// The dialogue font (1bpp, 8x16 per character, ASCII from $20, a clear bit is ink) at 1c:4720 in both games.
bool ui_font_load(UiFont *font, const uint8_t *rom, size_t rom_size);

// The first and last glyph rows with ink in the text (the font's letters sit low in their cells);
// false when nothing in it has ink.
bool ui_text_ink(const UiFont *font, const char *s, int *top, int *bottom);
