#pragma once
// The game picture's size, and each game's title-screen colours read from the player's ROM (the
// frame around a widescreen picture), so the app ships no game art.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UI_W 160
#define UI_H 144

typedef struct { uint8_t r, g, b; } UiColor;

// A game's colours, taken from its title screen's background palettes (PALH_03, paletteData4018:
// bank $16 in Seasons, $17 in Ages). The menus have their own (gui_theme); bg is widescreen's frame.
typedef struct { UiColor bg, panel, border, text, dim, highlight, accent; } UiTheme;
void ui_theme_default(UiTheme *t, bool ages);
bool ui_theme_load(UiTheme *t, const uint8_t *rom, size_t rom_size, bool ages);
