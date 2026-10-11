// Save sync: the sync page, entering a code from another device, and a save changed on two devices.
#include "gui/widgets.h"
#include <stdio.h>

// "Sync is on", the code (masked until Show), and Show/Hide.
static void sync_status(SyncMenu *m, const GuiTheme *t, bool *reveal) {
  CLAY({.layout = {.childGap = gui_px(10), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
    mark(true, t->accent);
    label(m->status[0] ? m->status : "Sync is on", TEXT, t->paper);
  }
  Clay_ElementId show = CLAY_ID("ShowCode");
  if (gui_tapped(show)) *reveal = !*reveal;
  char masked[24];
  snprintf(masked, sizeof masked, "%s", m->code);
  if (!*reveal)
    for (int i = 0; masked[i] && i < 14; i++)
      if (masked[i] != '-') masked[i] = '*';
  CLAY({.layout = {.childGap = gui_px(14), .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
    CLAY_TEXT(gui_str("Code %s", masked), text_line(SMALL, t->muted));
    CLAY({.id = show, .layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(gui_px(ROW_H))}, .padding = {gui_px(8), gui_px(8), 0, 0}, .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
      label(*reveal ? "Hide" : "Show", SMALL, t->accent);
    }
  }
}

int gui_sync(SyncMenu *m, const GuiTheme *t, const GuiFrame *f, bool *reveal) {
  static const char *const labels_off[SYNCROWS] = {"Sync now", "Create a code", "Enter a code", "Turn off"};
  static const char *const labels_on[SYNCROWS] = {"Sync now", "Create a code", "Use another code", "Turn off"};
  gui_begin(page_scale(f));
  int tapped = GUI_NONE, sel = (int)m->sel;
  PAGE(t, f) {
    if (header(t, "Save sync", f->hints ? "Enter select  Esc back" : NULL)) tapped = GUI_BACK;
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = gui_px(6), .padding = {gui_px(14), gui_px(14), gui_px(20), gui_px(18)}}}) {
      if (m->on) sync_status(m, t, reveal);
      else CLAY_TEXT(CLAY_STRING("Keep your saves and save states in step across your devices."), gui_text(SMALL, t->paper));
    }
    for (int r = 0; r < SYNCROWS; r++) {
      if (!syncmenu_row_shown(m, (SyncRow)r)) continue;
      if (list_row(t, (Row){.id = CLAY_IDI("Sync", r), .label = gui_str("%s", m->on ? labels_on[r] : labels_off[r]), .enabled = true}, &sel, r)) tapped = r;
    }
    if (m->on) paragraph(CLAY_STRING("Enter the same code on another device to share saves and save states."), SMALL, t->muted, 18, 0);
  }
  m->sel = (SyncRow)sel;
  return tapped;
}

int gui_code(CodeEntry *e, const GuiTheme *t, const GuiFrame *f) {
  gui_begin(page_scale(f));
  int tapped = GUI_NONE;
  PAGE(t, f) {
    if (header(t, "Enter code", f->hints ? "Type it  Esc back" : NULL)) tapped = GUI_BACK;
    paragraph(CLAY_STRING("Type the 16-digit code from your other device's Save sync page."), SMALL, t->paper, 8, 20);
    // four groups of four boxes, the one being typed into lit
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
              CLAY_TEXT(gui_str("%c", e->digits[i]), text_line(36, on ? t->on_hi : t->paper));
            }
          }
        }
    }
    if (tap_row(t, (Row){.id = CLAY_ID("Connect"), .label = CLAY_STRING("Connect"), .enabled = true})) tapped = GUI_CONNECT;
  }
  if (tapped >= 0 && tapped < 16) e->pos = tapped;
  return tapped;
}

static void conflict_side(const ConflictSide *side, int sd, const GuiTheme *t, bool sel, int64_t now) {
  UiColor dim = row_dim(t, sel);
  if (side->has_thumb) thumb(t, gui_image(4 + sd, side->thumb, UI_W, UI_H, 1), sel, THUMB_W);
  CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = gui_px(2)}}) {
    label(side->label, TEXT, row_fg(t, sel, true));
    CLAY_TEXT(side->missing ? CLAY_STRING("Not there") : when_str(side->time, now), text_line(SMALL, dim));
    if (side->has_files && !side->missing)
      for (int i = 0; i < UI_FILES; i++) {
        Clay_String fs = file_str(&side->files[i]);
        CLAY_TEXT(gui_str("%d  %.*s", i + 1, (int)fs.length, fs.chars), text_line(SMALL, dim));
      }
  }
}

int gui_conflict(ConflictView *v, const GuiTheme *t, const GuiFrame *f) {
  gui_begin(page_scale(f));
  int tapped = GUI_NONE;
  int64_t now = now_time();
  PAGE(t, f) {
    if (header(t, "Choose a save", f->hints ? "Enter keep  Esc later" : NULL)) tapped = GUI_BACK;
    paragraph(gui_str("%s was changed on two devices. Which one do you want to keep?", v->what), SMALL + 2, t->paper, 8, 16);
    for (int sd = 0; sd < 2; sd++) {
      Clay_ElementId id = CLAY_IDI("Side", sd);
      if (pick(id, true, &v->sel, sd)) tapped = sd;
      bool sel = v->sel == sd;
      TALL_ROW(t, id, sel) { conflict_side(&v->side[sd], sd, t, sel, now); }
    }
    if (tap_row(t, (Row){.id = CLAY_ID("Later"), .label = CLAY_STRING("Decide later"), .enabled = true})) tapped = GUI_BACK;
  }
  return tapped;
}
