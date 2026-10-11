// The settings list: its own page from the pause menu, and the main menu's settings tab.
#include "gui/widgets.h"

static const char *const filter_labels[FILTERS] = {"Sharp", "Lines", "LCD", "CRT", "xBRZ", "HQX"};
static const char *const setting_labels[SET_ROWS] = {"Volume", "Screen", "Scale", "Widescreen", "Dim sides", "Colours", "Fullscreen",
                                                     "Fast text", "Fast menus", "Quick swap", "Item X/Y buttons", "Controls", "Save sync"};

static const char *section_before(int r) {
  switch (r) {
  case SET_VOLUME: return "Sound";
  case SET_SCREEN: return "Picture";
  case SET_FAST_TEXT: return "Play";
  case SET_CONTROLS: return "System";
  default: return NULL;
  }
}

// 1 On, 2 Off, 0 not an On/Off row
static int toggle_of(const Settings *s, int r) {
  bool v;
  switch (r) {
  case SET_WIDESCREEN: v = s->widescreen; break;
  case SET_DIM_SIDES: v = s->dim_sides; break;
  case SET_FULLSCREEN: v = s->fullscreen; break;
  case SET_FAST_TEXT: v = s->fast_text; break;
  case SET_FAST_MENUS: v = s->fast_menus; break;
  case SET_QUICK_SWAP: v = s->quick_swap; break;
  case SET_FOUR_SLOTS: v = s->four_slots; break;
  default: return 0;
  }
  return v ? 1 : 2;
}

static Clay_String value_of(const Settings *s, const char *sync_label, int r, int toggle) {
  switch (r) {
  case SET_SCREEN: return gui_str("< %s >", filter_labels[s->filter]);
  case SET_SCALE: return gui_str("< %s >", s->fill ? "Fill" : "Pixel");
  case SET_COLOURS: return gui_str("< %s >", s->gbc_colours ? "GBC" : "Vivid");
  case SET_CONTROLS: return CLAY_STRING(">");
  case SET_SYNC: return gui_str("%s  >", sync_label && sync_label[0] ? sync_label : "Off");
  default: return toggle ? gui_str("%s", toggle == 1 ? "On" : "Off") : (Clay_String){0};
  }
}

// Volume: ten bars, each its own target (tall enough to hit, drawn as a short block). Returns the
// bar tapped (1..10), or 0.
static int volume_row(const GuiTheme *t, Clay_ElementId id, int volume, bool sel) {
  int set = 0;
  ROW(t, id, sel) {
    label("Volume", TEXT, row_fg(t, sel, true));
    spacer();
    CLAY({.layout = {.childGap = gui_px(3), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
      for (int i = 0; i < 10; i++) {
        Clay_ElementId bar = CLAY_IDI("Volume", i);
        if (gui_tapped(bar)) set = i + 1;
        UiColor c = i < volume ? (sel ? t->on_hi : t->accent) : (sel ? t->hi_dim : t->line);
        CLAY({.id = bar, .layout = {.sizing = {CLAY_SIZING_FIXED(gui_px(10)), CLAY_SIZING_FIXED(gui_px(ROW_H))}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}}}) {
          CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(gui_px(7)), CLAY_SIZING_FIXED(gui_px(16))}}, .backgroundColor = gui_rgb(c)}) {}
        }
      }
    }
  }
  return set;
}

void settings_rows(SettingsMenu *m, const Settings *s, const char *sync_label, const GuiTheme *t, bool focused, GuiSettingsTap *tap) {
  tap->row = GUI_NONE;
  tap->volume = -1;
  int sel = (int)m->sel;
  for (int r = 0; r < SET_ROWS; r++) {
    const char *sec = section_before(r);
    if (sec) section(t, sec);
    Clay_ElementId id = CLAY_IDI("Setting", r);
    if (r == SET_VOLUME) {
      // a bar sets the volume; the rest of the row steps it up, as Right does
      bool row_tapped = pick(id, true, &sel, r);
      int bar = volume_row(t, id, s->volume, focused && sel == r);
      if (bar) { sel = r; tap->row = r; tap->volume = bar; }
      else if (row_tapped) { tap->row = r; tap->button = UI_RIGHT; }
      continue;
    }
    int tg = toggle_of(s, r);
    if (pick(id, true, &sel, r)) { tap->row = r; tap->button = UI_ACCEPT; }
    // unfocused (the main menu's tab row has the keys), nothing in the list shows as selected
    row(t, (Row){.id = id, .label = gui_str("%s", setting_labels[r]), .value = value_of(s, sync_label, r, tg), .enabled = true, .toggle = tg,
                 .selected = focused && sel == r});
  }
  m->sel = (SettingsRow)sel;
}

bool gui_settings(SettingsMenu *m, const Settings *s, const char *sync_label, const GuiTheme *t, const GuiFrame *f, GuiSettingsTap *tap) {
  gui_begin(page_scale(f));
  bool back = false;
  keep_visible(CLAY_ID("SettingsList"), CLAY_IDI("Setting", m->sel));
  PAGE(t, f) {
    back = header(t, "Settings", f->hints ? "Enter change  Esc back" : NULL);
    LIST("SettingsList") { settings_rows(m, s, sync_label, t, true, tap); }
  }
  return back;
}
