#pragma once
// The sync screens' state: the sync page (from the settings' Save sync row), entering a sync code,
// and choosing between two versions of a save that changed on two devices. src/gui draws them.
#include "ui/ui.h"
#include "ui/launcher.h"
#include "ui/menu.h"
#include "ui/savefile.h"

typedef enum { SYNCROW_NOW, SYNCROW_CREATE, SYNCROW_ENTER, SYNCROW_OFF, SYNCROWS } SyncRow;

typedef struct {
  bool on;                      // this device has a sync code
  char code[20];                // "4827-1930-5561-0284"
  char status[24];              // the last sync, e.g. "Synced at 17:32"
  SyncRow sel;
} SyncMenu;

void syncmenu_open(SyncMenu *m, bool on, const char *code, const char *status);
MenuAction syncmenu_press(SyncMenu *m, UiButton b, SyncRow *picked);
// Off: Create and Enter; on: everything but Create.
bool syncmenu_row_shown(const SyncMenu *m, SyncRow r);

typedef struct {
  char digits[17];
  int pos;
} CodeEntry;

void codeentry_open(CodeEntry *e, const char *digits);
// Up/down change the digit, left/right move; A: MENU_PICK (all 16 digits are set), B: MENU_BACK.
MenuAction codeentry_press(CodeEntry *e, UiButton b);

typedef struct {
  char label[24];               // "This device", or the device that wrote the server's copy
  int64_t time;                 // SDL_Time it was written; 0 when unknown
  bool missing;                 // no file on this side
  bool has_files;               // a save: its three files
  UiFileInfo files[UI_FILES];
  bool has_thumb;               // a state slot: its picture
  uint8_t thumb[UI_W * UI_H * 3];
} ConflictSide;

typedef struct {
  char what[24];                // "Seasons save files", "Seasons slot 2"
  ConflictSide side[2];         // 0 this device, 1 the server
  int sel;
} ConflictView;

// Left/right pick a side, A keeps it (MENU_PICK), B leaves the choice for later (MENU_BACK).
MenuAction conflict_press(ConflictView *v, UiButton b);
