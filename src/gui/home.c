// The main menu: a tab per game and Settings. A game's tab leads with Resume and the four slots;
// its save files have a page of their own.
#include "gui/widgets.h"

static const char *const tab_labels[UI_GAMES + 1] = {"Ages", "Seasons", "Settings"};
static const char *const home_labels[HOME_ROWS] = {"Resume", "Slot 1", "Slot 2", "Slot 3", "Slot 4", "Save files", "Title screen"};

static void not_installed(Home *h, const GuiTheme *t, int *button) {
  CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = gui_px(12),
                   .padding = {gui_px(14), gui_px(14), gui_px(40), gui_px(24)}}}) {
    label("Not installed", TEXT, t->soft);
    CLAY_TEXT(gui_str("Pick your own Oracle of %s ROM (.gbc, or a .zip with one inside). The app keeps only what it needs and ships no game data.",
                      h->l.game == UI_GAME_AGES ? "Ages" : "Seasons"),
              gui_text(SMALL, t->muted));
  }
  if (tap_row(t, (Row){.id = CLAY_ID("AddRom"), .label = CLAY_STRING("Add ROM..."), .selected = true, .enabled = true})) *button = UI_ACCEPT;
}

static void files_page(Home *h, const GuiTheme *t, int *button) {
  const LauncherGame *g = &h->l.games[h->l.game];
  if (tap_row(t, (Row){.id = CLAY_ID("FilesBack"), .label = CLAY_STRING("< Save files"), .enabled = true})) *button = UI_BACK;
  for (int i = 0; i < UI_FILES; i++) {
    const UiFileInfo *fi = &g->files[i];
    Clay_ElementId id = CLAY_IDI("File", i);
    if (pick(id, true, &h->file, i)) *button = UI_ACCEPT;
    bool sel = h->file == i;
    ROW(t, id, sel) {
      CLAY_TEXT(gui_str("%d  %s", i + 1, fi->valid ? (fi->name[0] ? fi->name : "?") : "Empty"), text_line(TEXT, row_fg(t, sel, fi->valid)));
      spacer();
      if (fi->valid) {
        CLAY_TEXT(gui_str("%d hearts", fi->max_health / 4), text_line(SMALL, row_dim(t, sel)));
        CLAY_TEXT(gui_str("%d/8 essences", fi->essence_count), text_line(SMALL, row_dim(t, sel)));
      }
    }
  }
}

static const char *section_before(int r) {
  switch (r) {
  case HOME_RESUME: return "Continue";
  case HOME_SLOT1: return "Save states";
  case HOME_FILES: return "More";
  default: return NULL;
  }
}

// What a row shows at the right, and its tag.
static Clay_String value_of(const Home *h, int r, int64_t now, Clay_String *tg) {
  const LauncherGame *g = &h->l.games[h->l.game];
  if (r >= HOME_SLOT1 && r <= HOME_SLOT4) {
    const HomeSlot *sl = &h->slots[h->l.game][r - HOME_SLOT1];
    if (r - HOME_SLOT1 == home_latest(h)) *tg = CLAY_STRING("Latest");
    return sl->used ? when_str(sl->time, now) : CLAY_STRING("Empty");
  }
  if (r == HOME_RESUME && h->resume_time[h->l.game]) return when_str(h->resume_time[h->l.game], now);
  if (r == HOME_FILES) {
    int used = 0;
    for (int k = 0; k < UI_FILES; k++) used += g->files[k].valid;
    return gui_str("%d of %d  >", used, UI_FILES);
  }
  return (Clay_String){0};
}

static void game_rows(Home *h, const GuiTheme *t, int *button) {
  int64_t now = now_time();
  int sel = (int)h->row;
  for (int r = 0; r < HOME_ROWS; r++) {
    if (r == HOME_RESUME && !h->l.games[h->l.game].has_resume) continue;
    const char *sec = section_before(r);
    if (sec) section(t, sec);
    Clay_String tg = {0}, value = value_of(h, r, now, &tg);
    Row row = {.id = CLAY_IDI("Home", r), .label = gui_str("%s", home_labels[r]), .value = value, .tag = tg, .enabled = home_row_enabled(h, (HomeRow)r)};
    if (list_row(t, row, &sel, r)) *button = UI_ACCEPT;
  }
  h->row = (HomeRow)sel;
}

int gui_home(Home *h, const GuiTheme *t, const GuiFrame *f, SettingsMenu *sm, const Settings *s, const char *sync_label, GuiSettingsTap *tap) {
  gui_begin(page_scale(f));
  int button = GUI_NONE;
  tap->row = GUI_NONE;
  tap->volume = -1;
  if (h->l.settings_tab) keep_visible(CLAY_ID("HomeList"), CLAY_IDI("Setting", sm->sel));
  else if (h->files_page) keep_visible(CLAY_ID("HomeList"), CLAY_IDI("File", h->file));
  else keep_visible(CLAY_ID("HomeList"), CLAY_IDI("Home", h->row));
  PAGE(t, f) {
    int tab = tab_bar(t, "Tab", tab_labels, UI_GAMES + 1, h->l.settings_tab ? UI_GAMES : (int)h->l.game, h->l.settings_tab && !h->l.settings_list);
    if (tab != GUI_NONE) home_tap_tab(h, tab);
    LIST("HomeList") {
      if (h->l.settings_tab) {
        settings_rows(sm, s, sync_label, t, h->l.settings_list, tap);
        if (tap->row != GUI_NONE) h->l.settings_list = true;
      } else if (!h->l.games[h->l.game].installed) not_installed(h, t, &button);
      else if (h->files_page) files_page(h, t, &button);
      else game_rows(h, t, &button);
    }
    hint_line(t, f, h->l.settings_tab ? "Enter change   Esc back" : "Enter play   Left/Right switch game   Esc quit");
  }
  return button;
}
