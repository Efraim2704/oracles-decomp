#include "ui/home.h"
#include <string.h>

bool home_row_enabled(const Home *h, HomeRow r) {
  const LauncherGame *g = &h->l.games[h->l.game];
  if (!g->installed) return false;
  if (r == HOME_RESUME) return g->has_resume;
  if (r >= HOME_SLOT1 && r <= HOME_SLOT4) return h->slots[h->l.game][r - HOME_SLOT1].used;
  return r < HOME_ROWS;
}

int home_latest(const Home *h) {
  int best = -1;
  for (int i = 0; i < HOME_SLOTS; i++) {
    const UiSlot *s = &h->slots[h->l.game][i];
    if (s->used && (best < 0 || s->time > h->slots[h->l.game][best].time)) best = i;
  }
  return best;
}

// Resume if there is one, else the newest slot, else the save files.
static void select_game(Home *h, UiGame g) {
  h->l.game = g;
  h->files_page = false;
  h->file = 0;
  int latest = home_latest(h);
  h->row = home_row_enabled(h, HOME_RESUME) ? HOME_RESUME : latest >= 0 ? (HomeRow)(HOME_SLOT1 + latest) : HOME_FILES;
}

void home_init(Home *h, UiGame initial) {
  h->l.settings_tab = h->l.settings_list = false;
  UiGame g = h->l.games[initial].installed || !h->l.games[!initial].installed ? initial : (UiGame)!initial;
  select_game(h, g);
}

static void step(Home *h, int dir) {
  HomeRow start = h->row;
  do h->row = (HomeRow)((h->row + dir + HOME_ROWS) % HOME_ROWS);
  while (!home_row_enabled(h, h->row) && h->row != start);
}

void home_tap_tab(Home *h, int tab) {
  h->l.settings_list = false;
  h->l.settings_tab = tab == UI_GAMES;
  if (!h->l.settings_tab) select_game(h, (UiGame)tab);
}

static void switch_tab(Home *h, int dir) {
  int tab = h->l.settings_tab ? UI_GAMES : (int)h->l.game;
  home_tap_tab(h, (tab + dir + UI_GAMES + 1) % (UI_GAMES + 1));
}

static LaunchResult pick(Home *h) {
  LaunchResult r = {LAUNCH_NONE, h->l.game, LAUNCH_TITLE, UI_ACCEPT, -1};
  if (!h->l.games[h->l.game].installed) { r.action = LAUNCH_ADD_ROM; return r; }
  if (h->files_page) { r.action = LAUNCH_PLAY; r.file = h->file; return r; }
  if (!home_row_enabled(h, h->row)) return r;
  switch (h->row) {
  case HOME_RESUME: r.action = LAUNCH_PLAY; r.file = LAUNCH_RESUME; break;
  case HOME_FILES: h->files_page = true; break;
  case HOME_TITLE: r.action = LAUNCH_PLAY; r.file = LAUNCH_TITLE; break;
  case HOME_ADD_ROM: r.action = LAUNCH_ADD_ROM; break;
  default: r.action = LAUNCH_PLAY; r.file = LAUNCH_SLOTS; r.slot = h->row - HOME_SLOT1; break;
  }
  return r;
}

LaunchResult home_press(Home *h, UiButton b) {
  LaunchResult r = {LAUNCH_NONE, h->l.game, LAUNCH_TITLE, b, -1};
  if (h->l.settings_tab && h->l.settings_list) {
    if (b == UI_BACK) h->l.settings_list = false;
    else r.action = LAUNCH_SETTINGS;
    return r;
  }
  if (h->l.settings_tab) {
    if (b == UI_LEFT || b == UI_RIGHT) switch_tab(h, b == UI_LEFT ? -1 : 1);
    else if (b == UI_DOWN || b == UI_ACCEPT) h->l.settings_list = true;
    else if (b == UI_BACK) r.action = LAUNCH_QUIT;
    return r;
  }
  if (h->files_page) {
    switch (b) {
    case UI_UP: h->file = (h->file + UI_FILES - 1) % UI_FILES; break;
    case UI_DOWN: h->file = (h->file + 1) % UI_FILES; break;
    case UI_BACK: h->files_page = false; break;
    case UI_ACCEPT: return pick(h);
    default: break;
    }
    return r;
  }
  switch (b) {
  case UI_LEFT: case UI_RIGHT: switch_tab(h, b == UI_LEFT ? -1 : 1); break;
  case UI_UP: step(h, -1); break;
  case UI_DOWN: step(h, 1); break;
  case UI_BACK: r.action = LAUNCH_QUIT; break;
  case UI_ACCEPT: return pick(h);
  }
  return r;
}
