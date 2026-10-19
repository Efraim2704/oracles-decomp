#pragma once
// The main menu: a tab per game plus Settings. A game's tab leads with what a save-state player
// reaches for: Resume, then the four state slots, then the game's own save files (a page of their
// own), the title screen and Add ROM (another release, or a newer dump of this one). Pure state machine; src/gui draws it and turns taps into rows.
#include <stdint.h>
#include "ui/launcher.h"
#include "ui/menu.h"

#define HOME_SLOTS UI_SLOTS

typedef enum { HOME_RESUME, HOME_SLOT1, HOME_SLOT4 = HOME_SLOT1 + HOME_SLOTS - 1, HOME_FILES, HOME_TITLE, HOME_ADD_ROM, HOME_ROWS } HomeRow;

typedef struct {
  Launcher l;                   // the games, their files and themes; l.game is the tab shown
  UiSlot slots[UI_GAMES][HOME_SLOTS];
  UiSlot resume[UI_GAMES];    // the state written when the game was last quit
  uint32_t stamp;               // changes whenever the slots are read again (their pictures may have)
  HomeRow row;
  bool files_page;              // the save files page is open
  int file;                     // its row
} Home;

void home_init(Home *h, UiGame initial);
// The row exists and can be chosen (an empty slot cannot be loaded; Resume needs a resume state).
bool home_row_enabled(const Home *h, HomeRow r);
// The newest used slot of the shown game, or -1.
int home_latest(const Home *h);
// Buttons, as the launcher took them: left/right change tab, B quits (or leaves the files page),
// A picks. LAUNCH_SETTINGS hands the button to the settings list while it has the focus.
LaunchResult home_press(Home *h, UiButton b);
// A tap on a tab: UI_GAMES is the settings tab.
void home_tap_tab(Home *h, int tab);
