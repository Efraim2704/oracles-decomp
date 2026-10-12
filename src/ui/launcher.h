#pragma once
// What the main menu knows about each game, and the buttons and results every menu shares. The
// main menu itself is ui/home.h.
#include "ui/ui.h"
#include "ui/savefile.h"

typedef enum { UI_UP, UI_DOWN, UI_LEFT, UI_RIGHT, UI_ACCEPT, UI_BACK } UiButton;
typedef enum { UI_GAME_AGES, UI_GAME_SEASONS, UI_GAMES } UiGame;
// LAUNCH_SETTINGS: the button is for the settings list (the settings tab has the focus in it)
typedef enum { LAUNCH_NONE, LAUNCH_PLAY, LAUNCH_ADD_ROM, LAUNCH_SETTINGS, LAUNCH_QUIT } LaunchAction;

#define LAUNCH_TITLE (-1)
#define LAUNCH_RESUME (-2)      // the state written when the game was last quit
#define LAUNCH_SLOTS (-3)       // pick one of the save-state slots

typedef struct {
  bool installed;               // the game's assets are in the cache
  bool has_resume;              // an auto state from the last quit exists
  UiFileInfo files[UI_FILES];
  UiTheme theme;
} LauncherGame;

typedef struct {
  LauncherGame games[UI_GAMES];
  UiGame game;                  // highlighted game (on the settings tab: the one whose colours it uses)
  bool settings_tab;
  bool settings_list;           // on the settings tab, the list has the focus (else the tab row)
} Launcher;

typedef struct {
  LaunchAction action;
  UiGame game;
  int file;                     // 0..2, LAUNCH_TITLE, LAUNCH_RESUME or LAUNCH_SLOTS
  UiButton button;              // with LAUNCH_SETTINGS
  int slot;                     // with LAUNCH_SLOTS from the main menu: the slot chosen (0..3)
} LaunchResult;
