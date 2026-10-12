#include "gui/widgets.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

Clay_Color gui_clear(void) { return (Clay_Color){0, 0, 0, 0}; }

Clay_TextElementConfig *text_line(float points, UiColor c) {
  return CLAY_TEXT_CONFIG({.textColor = gui_rgb(c), .fontSize = gui_px(points), .wrapMode = CLAY_TEXT_WRAP_NONE});
}

Clay_TextElementConfig *text_spaced(float points, UiColor c, float spacing) {
  return CLAY_TEXT_CONFIG({.textColor = gui_rgb(c), .fontSize = gui_px(points), .letterSpacing = gui_px(spacing), .wrapMode = CLAY_TEXT_WRAP_NONE});
}

void label(const char *s, float points, UiColor c) { CLAY_TEXT(gui_str("%s", s), text_line(points, c)); }

UiColor row_fg(const GuiTheme *t, bool selected, bool enabled) { return selected ? t->on_hi : enabled ? t->paper : t->muted; }
UiColor row_dim(const GuiTheme *t, bool selected) { return selected ? t->on_hi : t->muted; }

void spacer(void) { CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {} }

void tag(Clay_String s, UiColor c) {
  CLAY({.layout = {.padding = {gui_px(6), gui_px(6), gui_px(1), gui_px(1)}}, .border = {.color = gui_rgb(c), .width = CLAY_BORDER_OUTSIDE(1)}}) {
    CLAY_TEXT(s, text_line(TAG, c));
  }
}

void mark(bool on, UiColor c) {
  uint16_t s = gui_px(9);
  CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(s), CLAY_SIZING_FIXED(s)}},
        .backgroundColor = on ? gui_rgb(c) : gui_clear(),
        .border = {.color = gui_rgb(c), .width = CLAY_BORDER_OUTSIDE(on ? 0 : gui_px(1.5f))}}) {}
}

void thumb(const GuiTheme *t, SDL_Texture *tex, bool selected, float w) {
  CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(gui_px(w)), CLAY_SIZING_FIXED(gui_px(w * UI_H / UI_W))}},
        .image = {.imageData = tex},
        .backgroundColor = tex ? gui_clear() : selected ? gui_rgb(t->hi_dim) : gui_rgba(t->paper, 13),
        .border = {.color = gui_rgb(selected ? t->on_hi : t->line_strong), .width = CLAY_BORDER_OUTSIDE(tex ? 0 : 1)}}) {}
}

void state_info(const GuiTheme *t, SDL_Texture *tex, float thumb_w, Clay_String name, bool latest, Clay_String when, bool selected, bool enabled) {
  thumb(t, tex, selected, thumb_w);
  CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = gui_px(2)}}) {
    CLAY({.layout = {.childGap = gui_px(10), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
      CLAY_TEXT(name, text_line(TEXT, row_fg(t, selected, enabled)));
      if (latest) tag(CLAY_STRING("Latest"), selected ? t->on_hi : t->accent);
    }
    CLAY_TEXT(when, text_line(SMALL, row_dim(t, selected)));
  }
}

Clay_String when_str(int64_t time, int64_t now) {
  char when[32];
  gui_when(time, now, when, sizeof when);
  return gui_str("%s", when);
}

int64_t now_time(void) {
  SDL_Time now = 0;
  SDL_GetCurrentTime(&now);
  return now;
}

Clay_String file_str(const UiFileInfo *fi) {
  if (!fi->valid) return CLAY_STRING("Empty");
  return gui_str("%s  %d hearts  %d/8 essences", fi->name[0] ? fi->name : "?", fi->max_health / 4, fi->essence_count);
}

Clay_ElementDeclaration row_box(const GuiTheme *t, Clay_ElementId id, bool selected, float min_h) {
  bool tall = min_h > 0;
  return (Clay_ElementDeclaration){
      .id = id,
      .layout = {.sizing = {CLAY_SIZING_GROW(0), tall ? CLAY_SIZING_FIT(gui_px(min_h)) : CLAY_SIZING_FIXED(gui_px(ROW_H))},
                 .padding = {gui_px(tall ? 12 : 14), gui_px(tall ? 12 : 14), gui_px(tall ? 8 : 0), gui_px(tall ? 8 : 0)},
                 .childGap = gui_px(tall ? 14 : 10), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
      .backgroundColor = selected ? gui_rgb(t->hi) : gui_clear()};
}

void row_cursor(const GuiTheme *t, bool selected) {
  CLAY_TEXT(CLAY_STRING(">"), CLAY_TEXT_CONFIG({.textColor = selected ? gui_rgb(t->on_hi) : gui_clear(), .fontSize = gui_px(TEXT), .wrapMode = CLAY_TEXT_WRAP_NONE}));
}

void row(const GuiTheme *t, Row r) {
  UiColor value = row_dim(t, r.selected);
  ROW(t, r.id, r.selected) {
    CLAY_TEXT(r.label, text_line(TEXT, row_fg(t, r.selected, r.enabled)));
    if (r.tag.length) tag(r.tag, r.selected ? t->on_hi : t->accent);
    spacer();
    if (r.toggle) {
      value = r.selected ? t->on_hi : r.toggle == 1 ? t->accent : t->muted;
      mark(r.toggle == 1, value);
    }
    if (r.value.length) CLAY_TEXT(r.value, text_line(TEXT, value));
  }
}

bool tap_row(const GuiTheme *t, Row r) {
  bool tapped = r.enabled && gui_tapped(r.id);
  row(t, r);
  return tapped;
}

bool pick(Clay_ElementId id, bool enabled, int *sel, int i) {
  if (!enabled) return false;
  if (gui_hovered(id)) *sel = i;
  if (!gui_tapped(id)) return false;
  *sel = i;
  return true;
}

bool list_row(const GuiTheme *t, Row r, int *sel, int i) {
  bool tapped = pick(r.id, r.enabled, sel, i);
  r.selected = *sel == i;
  row(t, r);
  return tapped;
}

void section(const GuiTheme *t, const char *s) {
  CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0)}, .padding = {gui_px(14), gui_px(14), gui_px(18), gui_px(6)}}}) {
    CLAY_TEXT(gui_str("%s", s), text_spaced(SMALL, t->muted, 1));
  }
}

bool header(const GuiTheme *t, const char *title, const char *hint) {
  Clay_ElementId back = CLAY_ID("Back");
  bool tapped = gui_tapped(back);
  CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(gui_px(56))}, .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
    // the sides are the same width, so the title stays centred
    CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(gui_px(150)), CLAY_SIZING_FIXED(gui_px(ROW_H))}}}) {
      CLAY({.id = back, .layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_GROW(0)}, .padding = {0, gui_px(12), 0, 0}, .childGap = gui_px(8),
                                   .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        label("<", TEXT, t->muted);
        label("Back", TEXT, t->muted);
      }
    }
    spacer();
    CLAY_TEXT(gui_str("%s", title), text_spaced(SMALL, t->soft, 2));
    spacer();
    CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(gui_px(150))}, .childAlignment = {.x = CLAY_ALIGN_X_RIGHT}}}) {
      if (hint) label(hint, TAG, t->muted);
    }
  }
  return tapped;
}

int tab_bar(const GuiTheme *t, const char *id, const char *const *labels, int n, int selected, bool focus) {
  int tapped = GUI_NONE;
  Clay_String name = {.length = (int32_t)strlen(id), .chars = id};
  CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(gui_px(52))}, .childGap = gui_px(4), .childAlignment = {.y = CLAY_ALIGN_Y_BOTTOM},
                   .padding = {0, 0, 0, gui_px(4)}},
        .border = {.color = gui_rgb(t->line), .width = {.bottom = 1}}}) {
    for (int i = 0; i < n; i++) {
      Clay_ElementId tab = Clay_GetElementIdWithIndex(name, (uint32_t)i);
      if (gui_tapped(tab)) tapped = i;
      bool on = i == selected;
      CLAY({.id = tab,
            .layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(gui_px(ROW_H))}, .padding = {gui_px(14), gui_px(14), 0, 0},
                       .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
            .border = {.color = gui_rgb(t->hi), .width = {.bottom = on ? gui_px(3) : 0}},
            .backgroundColor = on && focus ? gui_rgba(t->hi, 60) : gui_clear()}) {
        label(labels[i], TEXT, on ? t->accent : t->muted);
      }
    }
  }
  return tapped;
}

void hint_line(const GuiTheme *t, const GuiFrame *f, const char *s) {
  if (!f->hints) return;
  CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0)}, .padding = {0, 0, gui_px(8), 0}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}}) {
    label(s, SMALL, t->muted);
  }
}

void paragraph(Clay_String s, float points, UiColor c, float top, float bottom) {
  CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0)}, .padding = {gui_px(14), gui_px(14), gui_px(top), gui_px(bottom)}}}) {
    CLAY_TEXT(s, gui_text(points, c));
  }
}

float page_scale(const GuiFrame *f) {
  float d = gui_display_scale(), fit = fminf(f->w / (COLUMN_W + 2 * SIDE_PAD), f->h / 640.0f);
  return fmaxf(d, fminf(fit, d * 1.5f));
}

Clay_ElementDeclaration page_box(const GuiTheme *t, const GuiFrame *f) {
  return (Clay_ElementDeclaration){
      .id = CLAY_ID("Page"),
      .layout = {.sizing = {CLAY_SIZING_FIXED(f->w), CLAY_SIZING_FIXED(f->h)},
                 .padding = {(uint16_t)f->safe.x, (uint16_t)(f->w - f->safe.x - f->safe.w), (uint16_t)f->safe.y, (uint16_t)(f->h - f->safe.y - f->safe.h)},
                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER}},
      .backgroundColor = gui_rgb(t->ink)};
}

Clay_ElementDeclaration column_box(void) {
  return (Clay_ElementDeclaration){
      .layout = {.sizing = {CLAY_SIZING_GROW(0, gui_px(COLUMN_W)), CLAY_SIZING_GROW(0)},
                 .padding = {gui_px(SIDE_PAD), gui_px(SIDE_PAD), gui_px(8), gui_px(12)},
                 .layoutDirection = CLAY_TOP_TO_BOTTOM}};
}

Clay_ElementDeclaration list_box(Clay_ElementId id) {
  return (Clay_ElementDeclaration){
      .id = id,
      .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
      .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}};
}

void keep_visible(Clay_ElementId list, Clay_ElementId item) {
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
