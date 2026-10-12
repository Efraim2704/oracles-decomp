#pragma once
// Player settings (kept in settings.ini in the app folder) and the settings list's state machine.
#include "ui/ui.h"
#include "ui/launcher.h"
#include "ui/menu.h"
#include "ui/controls.h"

// XBRZ and HQX are the two pixel-art scalers: they round off the jagged diagonals instead of
// enlarging each pixel into a square. See src/ui/upscale.h.
typedef enum { FILTER_SHARP, FILTER_SCANLINES, FILTER_LCD, FILTER_CRT,
               FILTER_XBRZ, FILTER_HQX, FILTERS } ScreenFilter;

typedef struct {
  int volume;                   // 0..10
  bool fullscreen;
  ScreenFilter filter;
  bool fill;                    // the game fills the width (or height) at a fractional scale
  bool widescreen;              // 256 wide: the neighbouring rooms beside the 160 of the game
  bool dim_sides;               // widescreen's side strips drawn darker than the live room
  bool gbc_colours;             // the Game Boy Color screen's paler, warmer colours
  bool fast_text;               // each line of a text box appears at once
  bool quick_swap;              // the Swap button exchanges the A and B items
  bool fast_menus;              // quick fades into and out of the inventory and map
  bool four_slots;              // Item X / Item Y buttons, assigned in the inventory
  bool haptics;                 // a tick when a finger presses a control or taps a menu (Android)
  Bindings bindings;
  char sync_label[12];          // the SYNC row's value, kept by the app (sync.ini, not settings.ini)
} Settings;

typedef enum { SET_VOLUME, SET_SCREEN, SET_SCALE, SET_WIDESCREEN, SET_DIM_SIDES, SET_COLOURS, SET_FULLSCREEN, SET_FAST_TEXT, SET_FAST_MENUS, SET_QUICK_SWAP, SET_FOUR_SLOTS, SET_HAPTICS, SET_CONTROLS, SET_SYNC, SET_ROWS } SettingsRow;

typedef struct {
  SettingsRow sel;
  bool phone;                   // Android: Vibration is shown (nothing else vibrates) and Fullscreen
                                // is not (the app always fills the screen there)
} SettingsMenu;

void settings_default(Settings *s);
// Unknown keys and bad values are ignored, so an old or hand-edited file still loads.
void settings_parse(Settings *s, const char *text);
// Writes "key=value" lines; returns the number of bytes (as snprintf).
int settings_format(const Settings *s, char *out, size_t size);

void settings_menu_open(SettingsMenu *m, bool phone);
// The row is in the list (SET_HAPTICS only on a phone, SET_FULLSCREEN never); keys skip the others.
bool settings_row_shown(const SettingsMenu *m, SettingsRow r);
// Left/right change the highlighted value in s; accept on CONTROLS or SYNC returns MENU_PICK with that row.
MenuAction settings_press(SettingsMenu *m, Settings *s, UiButton b, SettingsRow *picked);
