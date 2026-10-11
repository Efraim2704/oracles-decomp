#pragma once
// In-game menus: the pause menu and the save-state slot picker. State machines only; src/gui draws them.
#include "ui/ui.h"
#include "ui/launcher.h"

typedef enum { MENU_NONE, MENU_PICK, MENU_BACK } MenuAction;

typedef enum { PAUSE_RESUME, PAUSE_SAVE, PAUSE_LOAD, PAUSE_SETTINGS, PAUSE_QUIT, PAUSE_ITEMS } PauseItem;
typedef struct {
  PauseItem sel;
  bool can_save;                // false when no frame with parked threads was reached
  bool can_load;                // some slot holds a state
} PauseMenu;

void pause_open(PauseMenu *m, bool can_save, bool can_load);
MenuAction pause_press(PauseMenu *m, UiButton b, PauseItem *picked);
// Save needs a frame to save from, Load a used slot.
bool pause_enabled(const PauseMenu *m, PauseItem i);

#define UI_SLOTS 4
typedef struct {
  bool used;
  int64_t time;                 // SDL_Time the state was written; 0 when unknown
  uint8_t thumb[UI_H][UI_W][3];
} UiSlot;

typedef struct {
  bool save;                    // saving (any slot) or loading (used slots only)
  int sel;
  UiSlot slots[UI_SLOTS];
} SlotsMenu;

void slots_open(SlotsMenu *m, bool save);
MenuAction slots_press(SlotsMenu *m, UiButton b, int *slot);
