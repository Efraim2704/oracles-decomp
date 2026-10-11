#include "gui/screens.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// Design sizes, in points (the mockups' units): rows are a 44pt touch target everywhere.
#define ROW_H 44
#define SLOT_ROW_H 104
#define TEXT 24
#define SMALL 20
#define TAG 18
#define COLUMN_W 480
#define SIDE_PAD 16
#define TOAST_ROOM 58             // a message's height and gap, kept clear under the pause menu

static Clay_Color clear(void) { return (Clay_Color){0, 0, 0, 0}; }

static Clay_TextElementConfig *text_nowrap(float points, Clay_Color c) {
  return CLAY_TEXT_CONFIG({.textColor = c, .fontSize = gui_px(points), .wrapMode = CLAY_TEXT_WRAP_NONE});
}

static Clay_TextElementConfig *text_spaced(float points, UiColor c, float spacing) {
  return CLAY_TEXT_CONFIG({.textColor = gui_rgb(c), .fontSize = gui_px(points), .letterSpacing = gui_px(spacing), .wrapMode = CLAY_TEXT_WRAP_NONE});
}

static void grow(void) { CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {} }

// A small outlined word after a label: Latest, New.
static void tag(Clay_String s, UiColor c) {
  CLAY({.layout = {.padding = {gui_px(6), gui_px(6), gui_px(1), gui_px(1)}}, .border = {.color = gui_rgb(c), .width = CLAY_BORDER_OUTSIDE(1)}}) {
    CLAY_TEXT(s, text_nowrap(TAG, gui_rgb(c)));
  }
}

// The square before On (filled) and Off (outlined), so the state never rests on colour alone.
static void mark(bool on, UiColor c) {
  uint16_t s = gui_px(9);
  if (on) CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(s), CLAY_SIZING_FIXED(s)}}, .backgroundColor = gui_rgb(c)}) {}
  else CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(s), CLAY_SIZING_FIXED(s)}}, .border = {.color = gui_rgb(c), .width = CLAY_BORDER_OUTSIDE(gui_px(1.5f))}}) {}
}

typedef struct {
  Clay_ElementId id;
  Clay_String label, value;
  bool selected, enabled;
  int toggle;                   // 0 none, 1 On, 2 Off
  Clay_String tag;
  float height;                 // ROW_H when 0
} Row;

// One list row: cursor, label, optional tag, the value at the right. Returns true when tapped.
static bool row(const GuiTheme *t, Row r) {
  UiColor fg = r.selected ? t->on_hi : r.enabled ? t->paper : t->muted;
  UiColor val = r.selected ? t->on_hi : t->muted;
  bool tapped = r.enabled && gui_tapped(r.id);
  CLAY({.id = r.id,
        .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(gui_px(r.height ? r.height : ROW_H))},
                   .padding = {gui_px(14), gui_px(14), 0, 0}, .childGap = gui_px(10), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
        .backgroundColor = r.selected ? gui_rgb(t->hi) : clear()}) {
    CLAY_TEXT(CLAY_STRING(">"), text_nowrap(TEXT, r.selected ? gui_rgb(t->on_hi) : clear()));
    CLAY_TEXT(r.label, text_nowrap(TEXT, gui_rgb(fg)));
    if (r.tag.length) tag(r.tag, r.selected ? t->on_hi : t->accent);
    grow();
    if (r.toggle) {
      UiColor c = r.selected ? t->on_hi : r.toggle == 1 ? t->accent : t->muted;
      mark(r.toggle == 1, c);
      val = c;
    }
    if (r.value.length) CLAY_TEXT(r.value, text_nowrap(TEXT, gui_rgb(val)));
  }
  return tapped;
}

static void section(const GuiTheme *t, Clay_String s) {
  CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0)}, .padding = {gui_px(14), gui_px(14), gui_px(18), gui_px(6)}}}) {
    CLAY_TEXT(s, text_spaced(SMALL, t->muted, 1));
  }
}

// Back on the left, the title in the middle. Returns true when Back was tapped.
static bool header(const GuiTheme *t, Clay_String title, const char *hint) {
  bool tapped = gui_tapped(CLAY_ID("Back"));
  CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(gui_px(56))}, .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(gui_px(150)), CLAY_SIZING_FIXED(gui_px(ROW_H))}, .childGap = gui_px(8), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
      CLAY({.id = CLAY_ID("Back"), .layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_GROW(0)}, .padding = {0, gui_px(12), 0, 0}, .childGap = gui_px(8), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        CLAY_TEXT(CLAY_STRING("<"), text_nowrap(TEXT, gui_rgb(t->muted)));
        CLAY_TEXT(CLAY_STRING("Back"), text_nowrap(TEXT, gui_rgb(t->muted)));
      }
    }
    grow();
    CLAY_TEXT(title, text_spaced(SMALL, t->soft, 2));
    grow();
    CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(gui_px(150))}, .childAlignment = {.x = CLAY_ALIGN_X_RIGHT}}}) {
      if (hint) CLAY_TEXT(gui_str("%s", hint), text_nowrap(TAG, gui_rgb(t->muted)));
    }
  }
  return tapped;
}

// A full-screen page: the theme's background, one column centred inside the safe area.
// The display's scale, grown (up to half again) when a big window leaves the column small.
static float page_scale(const GuiFrame *f) {
  float d = gui_display_scale(), fit = fminf(f->w / (COLUMN_W + 2 * SIDE_PAD), f->h / 640.0f);
  return fmaxf(d, fminf(fit, d * 1.5f));
}

#define PAGE(t, f)                                                                                                        \
  CLAY({.id = CLAY_ID("Page"),                                                                                            \
        .layout = {.sizing = {CLAY_SIZING_FIXED((f)->w), CLAY_SIZING_FIXED((f)->h)},                                     \
                   .padding = {(uint16_t)(f)->safe.x, (uint16_t)((f)->w - (f)->safe.x - (f)->safe.w), (uint16_t)(f)->safe.y, \
                               (uint16_t)((f)->h - (f)->safe.y - (f)->safe.h)},                                          \
                   .childAlignment = {.x = CLAY_ALIGN_X_CENTER}},                                                         \
        .backgroundColor = gui_rgb((t)->ink)})                                                                            \
  CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0, gui_px(COLUMN_W)), CLAY_SIZING_GROW(0)},                                \
                   .padding = {gui_px(SIDE_PAD), gui_px(SIDE_PAD), gui_px(8), gui_px(12)},                                \
                   .layoutDirection = CLAY_TOP_TO_BOTTOM}})

// Scrolls the list so the item drawn last frame is inside it (keys move the selection off screen).
static void keep_visible(Clay_ElementId list, Clay_ElementId item) {
  Clay_ScrollContainerData sc = Clay_GetScrollContainerData(list);
  Clay_ElementData box = Clay_GetElementData(list), it = Clay_GetElementData(item);
  if (!sc.found || !box.found || !it.found) return;
  float top = box.boundingBox.y, bottom = top + box.boundingBox.height;
  if (it.boundingBox.y < top) sc.scrollPosition->y += top - it.boundingBox.y;
  else if (it.boundingBox.y + it.boundingBox.height > bottom) sc.scrollPosition->y -= it.boundingBox.y + it.boundingBox.height - bottom;
  float min = sc.scrollContainerDimensions.height - sc.contentDimensions.height;
  if (sc.scrollPosition->y < min) sc.scrollPosition->y = min;
  if (sc.scrollPosition->y > 0) sc.scrollPosition->y = 0;
}

#define LIST(name)                                                                                     \
  CLAY({.id = CLAY_ID(name), .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM}, \
        .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})

static void hint_line(const GuiTheme *t, const GuiFrame *f, const char *s) {
  if (!f->hints) return;
  CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0)}, .padding = {0, 0, gui_px(8), 0}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}}) {
    CLAY_TEXT(gui_str("%s", s), text_nowrap(SMALL, gui_rgb(t->muted)));
  }
}

// ---- pause ----

static const char *const pause_labels[PAUSE_ITEMS] = {"Resume", "Save state", "Load state", "Settings", "Quit to menu"};

static bool pause_enabled(const PauseMenu *m, int i) { return (i != PAUSE_SAVE || m->can_save) && (i != PAUSE_LOAD || m->can_load); }

int gui_pause(PauseMenu *m, const GuiTheme *t, const GuiFrame *f) {
  // fits the title, five rows, the hint and room for a message under them inside the picture;
  // a big picture grows it up to half again the display's scale
  float need_h = 20 + 14 + PAUSE_ITEMS * ROW_H + (f->hints ? 36 : 0) + TOAST_ROOM + 24, need_w = 240;
  float s = fminf(gui_display_scale() * 1.5f, fminf(f->game.h / need_h, f->game.w / need_w));
  gui_begin(s);
  int tapped = GUI_NONE;
  CLAY({.id = CLAY_ID("Dim"), .layout = {.sizing = {CLAY_SIZING_FIXED(f->w), CLAY_SIZING_FIXED(f->h)}}, .backgroundColor = gui_rgba(t->ink, 200)}) {
    CLAY({.floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {f->game.x, f->game.y}},
          .layout = {.sizing = {CLAY_SIZING_FIXED(f->game.w), CLAY_SIZING_FIXED(f->game.h)}, .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = {0, 0, 0, gui_px(TOAST_ROOM)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .childGap = gui_px(14)}}) {
      CLAY_TEXT(CLAY_STRING("Paused"), text_spaced(SMALL, t->paper, 2));
      CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(gui_px(216))}, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
        for (int i = 0; i < PAUSE_ITEMS; i++) {
          Clay_ElementId id = CLAY_IDI("Pause", i);
          bool on = pause_enabled(m, i);
          if (on && gui_hovered(id)) m->sel = (PauseItem)i;
          if (row(t, (Row){.id = id, .label = gui_str("%s", pause_labels[i]), .selected = (int)m->sel == i, .enabled = on})) tapped = i;
        }
      }
      if (f->hints) CLAY_TEXT(CLAY_STRING("Enter select   Esc resume"), text_nowrap(TAG, gui_rgb(t->muted)));
    }
  }
  if (tapped >= 0) m->sel = (PauseItem)tapped;
  return tapped;
}

// ---- slots ----

void gui_when(int64_t time, int64_t now, char *out, size_t size) {
  static const char *const days[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  SDL_DateTime t, n, y;
  if (!time || !SDL_TimeToDateTime(time, &t, true) || !SDL_TimeToDateTime(now, &n, true)) { snprintf(out, size, "Saved"); return; }
  bool yesterday = SDL_TimeToDateTime(now - SDL_NS_PER_SECOND * 86400LL, &y, true) && y.year == t.year && y.month == t.month && y.day == t.day;
  int64_t age = now - time;
  if (t.year == n.year && t.month == n.month && t.day == n.day) snprintf(out, size, "Today %02d:%02d:%02d", t.hour, t.minute, t.second);
  else if (yesterday) snprintf(out, size, "Yesterday %02d:%02d:%02d", t.hour, t.minute, t.second);
  else if (age >= 0 && age < SDL_NS_PER_SECOND * 86400LL * 6) snprintf(out, size, "%s %02d:%02d:%02d", days[t.day_of_week % 7], t.hour, t.minute, t.second);
  else snprintf(out, size, "%s %d %02d:%02d", months[(t.month - 1) % 12], t.day, t.hour, t.minute);
}

static int newest(const SlotsMenu *m) {
  int best = -1;
  for (int i = 0; i < UI_SLOTS; i++)
    if (m->slots[i].used && (best < 0 || m->slots[i].time > m->slots[best].time)) best = i;
  return best;
}

int gui_slots(SlotsMenu *m, const GuiTheme *t, const GuiFrame *f, uint32_t stamp) {
  gui_begin(page_scale(f));
  int tapped = GUI_NONE, latest = newest(m);
  SDL_Time now = 0;
  SDL_GetCurrentTime(&now);
  keep_visible(CLAY_ID("SlotList"), CLAY_IDI("Slot", m->sel));
  PAGE(t, f) {
    if (header(t, m->save ? CLAY_STRING("Save state") : CLAY_STRING("Load state"), NULL)) tapped = GUI_BACK;
    LIST("SlotList") {
      for (int i = 0; i < UI_SLOTS; i++) {
        const UiSlot *sl = &m->slots[i];
        Clay_ElementId id = CLAY_IDI("Slot", i);
        bool on = m->save || sl->used, sel = m->sel == i;
        if (on && gui_hovered(id)) m->sel = i;
        if (on && gui_tapped(id)) tapped = i;
        UiColor fg = sel ? t->on_hi : on ? t->paper : t->muted, dim = sel ? t->on_hi : t->muted;
        char when[32] = "Empty";
        if (sl->used) gui_when(sl->time, now, when, sizeof when);
        CLAY({.id = id,
              .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(gui_px(SLOT_ROW_H))}, .padding = {gui_px(12), gui_px(12), 0, 0},
                         .childGap = gui_px(14), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
              .backgroundColor = sel ? gui_rgb(t->hi) : clear()}) {
          CLAY_TEXT(CLAY_STRING(">"), text_nowrap(TEXT, sel ? gui_rgb(t->on_hi) : clear()));
          SDL_Texture *thumb = sl->used ? gui_image(i, &sl->thumb[0][0][0], UI_W, UI_H, stamp) : NULL;
          CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(gui_px(80)), CLAY_SIZING_FIXED(gui_px(72))}},
                .image = {.imageData = thumb},
                .backgroundColor = thumb ? clear() : sel ? gui_rgb(t->hi_dim) : gui_rgba(t->paper, 13),
                .border = {.color = thumb ? clear() : gui_rgb(sel ? t->on_hi : t->line_strong), .width = CLAY_BORDER_OUTSIDE(thumb ? 0 : 1)}}) {}
          CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = gui_px(2)}}) {
            CLAY({.layout = {.childGap = gui_px(10), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
              CLAY_TEXT(gui_str("Slot %d", i + 1), text_nowrap(TEXT, gui_rgb(fg)));
              if (i == latest) tag(CLAY_STRING("Latest"), sel ? t->on_hi : t->accent);
            }
            CLAY_TEXT(gui_str("%s", when), text_nowrap(SMALL, gui_rgb(dim)));
          }
        }
      }
    }
    hint_line(t, f, m->save ? "Enter save   Esc back" : "Enter load   Esc back");
  }
  if (tapped >= 0) m->sel = tapped;
  return tapped;
}

// ---- settings ----

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

static void settings_rows(SettingsMenu *m, const Settings *s, const char *sync_label, const GuiTheme *t, bool focused, GuiSettingsTap *tap) {
  tap->row = GUI_NONE;
  tap->volume = -1;
  for (int r = 0; r < SET_ROWS; r++) {
    const char *sec = section_before(r);
    if (sec) section(t, gui_str("%s", sec));
    Clay_ElementId id = CLAY_IDI("Setting", r);
    bool sel = focused && (int)m->sel == r;
    if (gui_hovered(id)) m->sel = (SettingsRow)r;
    int tg = toggle_of(s, r);
    Clay_String value = {0};
    switch (r) {
    case SET_SCREEN: value = gui_str("< %s >", filter_labels[s->filter]); break;
    case SET_SCALE: value = gui_str("< %s >", s->fill ? "Fill" : "Pixel"); break;
    case SET_COLOURS: value = gui_str("< %s >", s->gbc_colours ? "GBC" : "Vivid"); break;
    case SET_CONTROLS: value = CLAY_STRING(">"); break;
    case SET_SYNC: value = gui_str("%s  >", sync_label && sync_label[0] ? sync_label : "Off"); break;
    default: if (tg) value = gui_str("%s", tg == 1 ? "On" : "Off"); break;
    }
    if (r == SET_VOLUME) {
      bool tapped = gui_tapped(id);
      UiColor fg = sel ? t->on_hi : t->paper;
      CLAY({.id = id,
            .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(gui_px(ROW_H))}, .padding = {gui_px(14), gui_px(14), 0, 0},
                       .childGap = gui_px(10), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
            .backgroundColor = sel ? gui_rgb(t->hi) : clear()}) {
        CLAY_TEXT(CLAY_STRING(">"), text_nowrap(TEXT, sel ? gui_rgb(t->on_hi) : clear()));
        CLAY_TEXT(CLAY_STRING("Volume"), text_nowrap(TEXT, gui_rgb(fg)));
        grow();
        CLAY({.layout = {.childGap = gui_px(3), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
          for (int i = 0; i < 10; i++) {
            Clay_ElementId bar = CLAY_IDI("Volume", i);
            if (gui_tapped(bar)) { tap->volume = i + 1; tapped = false; }
            // each bar is its own target: tall enough to hit, drawn as a short block
            CLAY({.id = bar, .layout = {.sizing = {CLAY_SIZING_FIXED(gui_px(10)), CLAY_SIZING_FIXED(gui_px(ROW_H))}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}}}) {
              UiColor c = i < s->volume ? (sel ? t->on_hi : t->accent) : (sel ? t->hi_dim : t->line);
              CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(gui_px(7)), CLAY_SIZING_FIXED(gui_px(16))}}, .backgroundColor = gui_rgb(c)}) {}
            }
          }
        }
      }
      if (tap->volume >= 0) { m->sel = SET_VOLUME; tap->row = r; }
      else if (tapped) { m->sel = SET_VOLUME; tap->row = r; tap->button = UI_RIGHT; }
      continue;
    }
    if (row(t, (Row){.id = id, .label = gui_str("%s", setting_labels[r]), .value = value, .selected = sel, .enabled = true, .toggle = tg})) {
      m->sel = (SettingsRow)r;
      tap->row = r;
      tap->button = UI_ACCEPT;
    }
  }
}

bool gui_settings(SettingsMenu *m, const Settings *s, const char *sync_label, const GuiTheme *t, const GuiFrame *f, GuiSettingsTap *tap) {
  gui_begin(page_scale(f));
  bool back = false;
  keep_visible(CLAY_ID("SettingsList"), CLAY_IDI("Setting", m->sel));
  PAGE(t, f) {
    back = header(t, CLAY_STRING("Settings"), f->hints ? "Enter change  Esc back" : NULL);
    LIST("SettingsList") { settings_rows(m, s, sync_label, t, true, tap); }
  }
  return back;
}

// ---- main menu ----

static const char *const tab_labels[UI_GAMES + 1] = {"Ages", "Seasons", "Settings"};
static const char *const home_labels[HOME_ROWS] = {"Resume", "Slot 1", "Slot 2", "Slot 3", "Slot 4", "Save files", "Title screen"};

static void tabs(Home *h, const GuiTheme *t) {
  int on = h->l.settings_tab ? UI_GAMES : (int)h->l.game;
  CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(gui_px(56))}, .childGap = gui_px(4), .childAlignment = {.y = CLAY_ALIGN_Y_BOTTOM},
                   .padding = {0, 0, 0, gui_px(4)}},
        .border = {.color = gui_rgb(t->line), .width = {.bottom = 1}}}) {
    for (int i = 0; i <= UI_GAMES; i++) {
      Clay_ElementId id = CLAY_IDI("Tab", i);
      if (gui_tapped(id)) home_tap_tab(h, i);
      bool sel = i == on, focus = sel && (!h->l.settings_tab || !h->l.settings_list);
      CLAY({.id = id, .layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(gui_px(ROW_H))}, .padding = {gui_px(14), gui_px(14), 0, 0},
                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
            .border = {.color = gui_rgb(t->hi), .width = {.bottom = sel ? gui_px(3) : 0}},
            .backgroundColor = focus && h->l.settings_tab ? gui_rgba(t->hi, 60) : clear()}) {
        CLAY_TEXT(gui_str("%s", tab_labels[i]), text_nowrap(TEXT, gui_rgb(sel ? t->accent : t->muted)));
      }
    }
  }
}

static void file_row(Home *h, const GuiTheme *t, int i, int *button) {
  const UiFileInfo *fi = &h->l.games[h->l.game].files[i];
  Clay_ElementId id = CLAY_IDI("File", i);
  bool sel = h->file == i;
  if (gui_hovered(id)) h->file = i;
  if (gui_tapped(id)) { h->file = i; *button = UI_ACCEPT; }
  UiColor fg = sel ? t->on_hi : fi->valid ? t->paper : t->muted;
  CLAY({.id = id,
        .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(gui_px(ROW_H))}, .padding = {gui_px(14), gui_px(14), 0, 0},
                   .childGap = gui_px(10), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
        .backgroundColor = sel ? gui_rgb(t->hi) : clear()}) {
    CLAY_TEXT(CLAY_STRING(">"), text_nowrap(TEXT, sel ? gui_rgb(t->on_hi) : clear()));
    CLAY_TEXT(gui_str("%d  %s", i + 1, fi->valid ? (fi->name[0] ? fi->name : "?") : "Empty"), text_nowrap(TEXT, gui_rgb(fg)));
    grow();
    if (fi->valid) {
      CLAY_TEXT(gui_str("%d hearts", fi->max_health / 4), text_nowrap(SMALL, gui_rgb(sel ? t->on_hi : t->muted)));
      CLAY_TEXT(gui_str("%d/8 essences", fi->essence_count), text_nowrap(SMALL, gui_rgb(sel ? t->on_hi : t->muted)));
    }
  }
}

int gui_home(Home *h, const GuiTheme *t, const GuiFrame *f, SettingsMenu *sm, const Settings *s, const char *sync_label, GuiSettingsTap *tap) {
  gui_begin(page_scale(f));
  int button = GUI_NONE;
  tap->row = GUI_NONE;
  tap->volume = -1;
  const LauncherGame *g = &h->l.games[h->l.game];
  SDL_Time now = 0;
  SDL_GetCurrentTime(&now);
  if (h->l.settings_tab) keep_visible(CLAY_ID("HomeList"), CLAY_IDI("Setting", sm->sel));
  else if (h->files_page) keep_visible(CLAY_ID("HomeList"), CLAY_IDI("File", h->file));
  else keep_visible(CLAY_ID("HomeList"), CLAY_IDI("Home", h->row));
  PAGE(t, f) {
    tabs(h, t);
    LIST("HomeList") {
      if (h->l.settings_tab) {
        settings_rows(sm, s, sync_label, t, h->l.settings_list, tap);
        if (tap->row != GUI_NONE) h->l.settings_list = true;
      } else if (!g->installed) {
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = gui_px(12),
                         .padding = {gui_px(14), gui_px(14), gui_px(40), gui_px(24)}}}) {
          CLAY_TEXT(CLAY_STRING("Not installed"), text_nowrap(TEXT, gui_rgb(t->soft)));
          CLAY_TEXT(gui_str("Pick your own Oracle of %s ROM (.gbc, or a .zip with one inside). The app keeps only what it needs and ships no game data.",
                            h->l.game == UI_GAME_AGES ? "Ages" : "Seasons"),
                    gui_text(SMALL, t->muted));
        }
        if (row(t, (Row){.id = CLAY_ID("AddRom"), .label = CLAY_STRING("Add ROM..."), .selected = true, .enabled = true})) button = UI_ACCEPT;
      } else if (h->files_page) {
        if (row(t, (Row){.id = CLAY_ID("FilesBack"), .label = CLAY_STRING("< Save files"), .enabled = true})) button = UI_BACK;
        for (int i = 0; i < UI_FILES; i++) file_row(h, t, i, &button);
      } else {
        int latest = home_latest(h);
        for (int i = 0; i < HOME_ROWS; i++) {
          if (i == HOME_RESUME && !g->has_resume) continue;
          if (i == HOME_RESUME) section(t, CLAY_STRING("Continue"));
          if (i == HOME_SLOT1) section(t, CLAY_STRING("Save states"));
          if (i == HOME_FILES) section(t, CLAY_STRING("More"));
          Clay_ElementId id = CLAY_IDI("Home", i);
          bool on = home_row_enabled(h, (HomeRow)i);
          if (on && gui_hovered(id)) h->row = (HomeRow)i;
          Clay_String value = {0}, tg = {0};
          if (i >= HOME_SLOT1 && i <= HOME_SLOT4) {
            const HomeSlot *sl = &h->slots[h->l.game][i - HOME_SLOT1];
            char when[32] = "Empty";
            if (sl->used) gui_when(sl->time, now, when, sizeof when);
            value = gui_str("%s", when);
            if (i - HOME_SLOT1 == latest) tg = CLAY_STRING("Latest");
          }
          if (i == HOME_RESUME && h->resume_time[h->l.game]) {
            char when[32];
            gui_when(h->resume_time[h->l.game], now, when, sizeof when);
            value = gui_str("%s", when);
          }
          if (i == HOME_FILES) {
            int used = 0;
            for (int k = 0; k < UI_FILES; k++) used += g->files[k].valid;
            value = gui_str("%d of %d  >", used, UI_FILES);
          }
          if (row(t, (Row){.id = id, .label = gui_str("%s", home_labels[i]), .value = value, .tag = tg, .selected = (int)h->row == i, .enabled = on}))
            { h->row = (HomeRow)i; button = UI_ACCEPT; }
        }
      }
    }
    if (f->hints) hint_line(t, f, h->l.settings_tab ? "Enter change   Esc back" : "Enter play   Left/Right switch game   Esc quit");
  }
  return button;
}

// ---- controls ----

static const char *const control_labels[CTRL_ROWS] = {"Up", "Down", "Left", "Right", "A", "B", "Start", "Select", "Pause",
                                                      "Fast forward", "Swap A/B", "Item X", "Item Y", "Reset to defaults"};

static void key_chip(const GuiTheme *t, const char *name, bool sel) {
  CLAY({.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(gui_px(28))}, .padding = {gui_px(8), gui_px(8), 0, 0}, .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
        .border = {.color = gui_rgb(sel ? t->on_hi : t->line_strong), .width = CLAY_BORDER_OUTSIDE(1)}}) {
    CLAY_TEXT(gui_str("%s", name), text_nowrap(SMALL, gui_rgb(sel ? t->on_hi : t->soft)));
  }
}

int gui_controls(ControlsMenu *m, const Bindings *b, const char *(*key_name)(int), const char *(*pad_name)(int), const GuiTheme *t,
                 const GuiFrame *f, int *page) {
  gui_begin(page_scale(f));
  int tapped = GUI_NONE;
  *page = -1;
  keep_visible(CLAY_ID("ControlList"), CLAY_IDI("Control", m->sel));
  PAGE(t, f) {
    if (header(t, CLAY_STRING("Controls"), f->hints ? (m->waiting ? "Press the key" : "Enter set  Esc back") : NULL)) tapped = GUI_BACK;
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(gui_px(52))}, .childGap = gui_px(4), .childAlignment = {.y = CLAY_ALIGN_Y_BOTTOM},
                     .padding = {0, 0, 0, gui_px(4)}},
          .border = {.color = gui_rgb(t->line), .width = {.bottom = 1}}}) {
      for (int i = 0; i < 2; i++) {
        Clay_ElementId id = CLAY_IDI("ControlTab", i);
        if (gui_tapped(id)) *page = i;
        bool on = m->pad_page == (i == 1);
        CLAY({.id = id, .layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(gui_px(ROW_H))}, .padding = {gui_px(14), gui_px(14), 0, 0},
                                   .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
              .border = {.color = gui_rgb(t->hi), .width = {.bottom = on ? gui_px(3) : 0}}}) {
          CLAY_TEXT(i ? CLAY_STRING("Gamepad") : CLAY_STRING("Keyboard"), text_nowrap(TEXT, gui_rgb(on ? t->accent : t->muted)));
        }
      }
    }
    LIST("ControlList") {
      for (int r = 0; r < CTRL_ROWS; r++) {
        if (r == ACT_UP) section(t, CLAY_STRING("Movement"));
        if (r == ACT_A) section(t, CLAY_STRING("Buttons"));
        if (r == ACT_PAUSE) section(t, CLAY_STRING("More"));
        if (r == CTRL_RESET) CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(gui_px(14))}}}) {}
        Clay_ElementId id = CLAY_IDI("Control", r);
        bool sel = m->sel == r;
        if (!m->waiting && gui_hovered(id)) m->sel = r;
        if (!m->waiting && gui_tapped(id)) tapped = r;
        CLAY({.id = id,
              .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(gui_px(ROW_H))}, .padding = {gui_px(14), gui_px(14), 0, 0},
                         .childGap = gui_px(10), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
              .backgroundColor = sel ? gui_rgb(t->hi) : clear()}) {
          CLAY_TEXT(CLAY_STRING(">"), text_nowrap(TEXT, sel ? gui_rgb(t->on_hi) : clear()));
          CLAY_TEXT(gui_str("%s", control_labels[r]), text_nowrap(TEXT, gui_rgb(sel ? t->on_hi : t->paper)));
          grow();
          if (r == CTRL_RESET) continue;
          if (sel && m->waiting) { CLAY_TEXT(m->pad_page ? CLAY_STRING("Press a button...") : CLAY_STRING("Press a key..."), text_nowrap(TEXT, gui_rgb(t->on_hi))); continue; }
          CLAY({.layout = {.childGap = gui_px(8), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
            if (m->pad_page) key_chip(t, b->pad[r] == NO_BINDING ? "-" : pad_name(b->pad[r]), sel);
            else for (int k = 0; k < KEYS_PER_ACTION; k++)
              if (b->key[r][k] != NO_BINDING || k == 0) key_chip(t, b->key[r][k] == NO_BINDING ? "-" : key_name(b->key[r][k]), sel);
          }
        }
      }
    }
  }
  if (tapped >= 0) m->sel = tapped;
  return tapped;
}

// ---- save sync ----

int gui_sync(SyncMenu *m, const GuiTheme *t, const GuiFrame *f, bool *reveal) {
  static const char *const labels_off[SYNCROWS] = {"Sync now", "Create a code", "Enter a code", "Turn off"};
  static const char *const labels_on[SYNCROWS] = {"Sync now", "Create a code", "Use another code", "Turn off"};
  gui_begin(page_scale(f));
  int tapped = GUI_NONE;
  PAGE(t, f) {
    if (header(t, CLAY_STRING("Save sync"), f->hints ? "Enter select  Esc back" : NULL)) tapped = GUI_BACK;
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = gui_px(6), .padding = {gui_px(14), gui_px(14), gui_px(20), gui_px(18)}}}) {
      if (m->on) {
        CLAY({.layout = {.childGap = gui_px(10), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
          mark(true, t->accent);
          CLAY_TEXT(gui_str("%s", m->status[0] ? m->status : "Sync is on"), text_nowrap(TEXT, gui_rgb(t->paper)));
        }
        Clay_ElementId show = CLAY_ID("ShowCode");
        if (gui_tapped(show)) *reveal = !*reveal;
        char masked[24];
        snprintf(masked, sizeof masked, "%s", m->code);
        if (!*reveal) for (int i = 0; masked[i] && i < 14; i++) if (masked[i] != '-') masked[i] = '*';
        CLAY({.layout = {.childGap = gui_px(14), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
          CLAY_TEXT(gui_str("Code %s", masked), text_nowrap(SMALL, gui_rgb(t->muted)));
          CLAY({.id = show, .layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(gui_px(ROW_H))}, .padding = {gui_px(8), gui_px(8), 0, 0}, .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
            CLAY_TEXT(*reveal ? CLAY_STRING("Hide") : CLAY_STRING("Show"), text_nowrap(SMALL, gui_rgb(t->accent)));
          }
        }
      } else {
        CLAY_TEXT(CLAY_STRING("Keep your saves and save states in step across your devices."), gui_text(SMALL, t->paper));
      }
    }
    for (int r = 0; r < SYNCROWS; r++) {
      bool shown = m->on ? r != SYNCROW_CREATE : r == SYNCROW_CREATE || r == SYNCROW_ENTER;
      if (!shown) continue;
      Clay_ElementId id = CLAY_IDI("Sync", r);
      if (gui_hovered(id)) m->sel = (SyncRow)r;
      bool sel = (int)m->sel == r;
      if (row(t, (Row){.id = id, .label = gui_str("%s", m->on ? labels_on[r] : labels_off[r]), .selected = sel, .enabled = true})) tapped = r;
    }
    if (m->on) CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0)}, .padding = {gui_px(14), gui_px(14), gui_px(18), 0}}}) {
      CLAY_TEXT(CLAY_STRING("Enter the same code on another device to share saves and save states."), gui_text(SMALL, t->muted));
    }
  }
  if (tapped >= 0) m->sel = (SyncRow)tapped;
  return tapped;
}

int gui_code(CodeEntry *e, const GuiTheme *t, const GuiFrame *f) {
  gui_begin(page_scale(f));
  int tapped = GUI_NONE;
  PAGE(t, f) {
    if (header(t, CLAY_STRING("Enter code"), f->hints ? "Type it  Esc back" : NULL)) tapped = GUI_BACK;
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0)}, .padding = {gui_px(14), gui_px(14), gui_px(8), gui_px(20)}}}) {
      CLAY_TEXT(CLAY_STRING("Type the 16-digit code from your other device's Save sync page."), gui_text(SMALL, t->paper));
    }
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = gui_px(12), .childAlignment = {.x = CLAY_ALIGN_X_CENTER},
                     .padding = {0, 0, gui_px(8), gui_px(24)}}}) {
      for (int g = 0; g < 4; g++)
        CLAY({.layout = {.childGap = gui_px(8)}}) {
          for (int k = 0; k < 4; k++) {
            int i = g * 4 + k;
            Clay_ElementId id = CLAY_IDI("Digit", i);
            if (gui_tapped(id)) tapped = i;
            bool on = e->pos == i;
            CLAY({.id = id, .layout = {.sizing = {CLAY_SIZING_FIXED(gui_px(52)), CLAY_SIZING_FIXED(gui_px(60))}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                  .backgroundColor = on ? gui_rgb(t->hi) : gui_rgba(t->paper, 10),
                  .border = {.color = gui_rgb(t->line_strong), .width = {.bottom = on ? 0 : gui_px(2)}}}) {
              CLAY_TEXT(gui_str("%c", e->digits[i]), text_nowrap(36, gui_rgb(on ? t->on_hi : t->paper)));
            }
          }
        }
    }
    if (row(t, (Row){.id = CLAY_ID("Connect"), .label = CLAY_STRING("Connect"), .enabled = true})) tapped = GUI_CONNECT;
  }
  if (tapped >= 0 && tapped < 16) e->pos = tapped;
  return tapped;
}

int gui_conflict(ConflictView *v, const GuiTheme *t, const GuiFrame *f) {
  gui_begin(page_scale(f));
  int tapped = GUI_NONE;
  PAGE(t, f) {
    if (header(t, CLAY_STRING("Choose a save"), f->hints ? "Enter keep  Esc later" : NULL)) tapped = GUI_BACK;
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0)}, .padding = {gui_px(14), gui_px(14), gui_px(8), gui_px(16)}}}) {
      CLAY_TEXT(gui_str("%s was changed on two devices. Which one do you want to keep?", v->what), gui_text(SMALL + 2, t->paper));
    }
    for (int sd = 0; sd < 2; sd++) {
      const ConflictSide *side = &v->side[sd];
      Clay_ElementId id = CLAY_IDI("Side", sd);
      if (gui_hovered(id)) v->sel = sd;
      if (gui_tapped(id)) tapped = sd;
      bool sel = v->sel == sd;
      UiColor fg = sel ? t->on_hi : t->paper, dim = sel ? t->on_hi : t->muted;
      SDL_Texture *thumb = side->has_thumb ? gui_image(4 + sd, side->thumb, UI_W, UI_H, 1) : NULL;
      CLAY({.id = id, .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(gui_px(SLOT_ROW_H))}, .padding = {gui_px(12), gui_px(12), gui_px(10), gui_px(10)},
                                 .childGap = gui_px(14), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
            .backgroundColor = sel ? gui_rgb(t->hi) : clear()}) {
        CLAY_TEXT(CLAY_STRING(">"), text_nowrap(TEXT, sel ? gui_rgb(t->on_hi) : clear()));
        if (thumb) CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(gui_px(80)), CLAY_SIZING_FIXED(gui_px(72))}}, .image = {.imageData = thumb}}) {}
        CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = gui_px(2)}}) {
          CLAY_TEXT(gui_str("%s", side->label), text_nowrap(TEXT, gui_rgb(fg)));
          CLAY_TEXT(gui_str("%s", side->missing ? "Not there" : side->when), text_nowrap(SMALL, gui_rgb(dim)));
          if (side->has_files && !side->missing)
            for (int i = 0; i < UI_FILES; i++) {
              const UiFileInfo *fi = &side->files[i];
              if (fi->valid) CLAY_TEXT(gui_str("%d  %s  %d hearts  %d/8 essences", i + 1, fi->name[0] ? fi->name : "?", fi->max_health / 4, fi->essence_count), text_nowrap(SMALL, gui_rgb(dim)));
              else CLAY_TEXT(gui_str("%d  Empty", i + 1), text_nowrap(SMALL, gui_rgb(dim)));
            }
        }
      }
    }
    if (row(t, (Row){.id = CLAY_ID("Later"), .label = CLAY_STRING("Decide later"), .enabled = true})) tapped = GUI_BACK;
  }
  if (tapped >= 0) v->sel = tapped;
  return tapped;
}

// ---- toast ----

bool gui_toast(const char *msg, bool undo, const GuiTheme *t, const GuiFrame *f) {
  Clay_ElementId u = CLAY_ID("Undo");
  bool tapped = undo && gui_tapped(u);
  CLAY({.id = CLAY_ID("Toast"),
        .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .zIndex = 100,
                     .offset = {f->game.x + f->game.w / 2, f->game.y + f->game.h - gui_px(14)},
                     .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_BOTTOM, .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
        .layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(gui_px(ROW_H))}, .padding = {gui_px(16), gui_px(undo ? 4 : 16), 0, 0},
                   .childGap = gui_px(14), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
        .backgroundColor = gui_rgb(t->ink),
        .border = {.color = gui_rgb(t->line_strong), .width = CLAY_BORDER_OUTSIDE(1)}}) {
    mark(true, t->accent);
    CLAY_TEXT(gui_str("%s", msg), text_nowrap(22, gui_rgb(t->paper)));
    if (undo)
      CLAY({.id = u, .layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_GROW(0)}, .padding = {gui_px(12), gui_px(12), 0, 0}, .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        CLAY_TEXT(CLAY_STRING("Undo"), text_nowrap(22, gui_rgb(t->accent)));
      }
  }
  return tapped;
}
