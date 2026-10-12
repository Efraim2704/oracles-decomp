#include "ui/syncui.h"
#include <stdio.h>
#include <string.h>

bool syncmenu_row_shown(const SyncMenu *m, SyncRow r) {
  return m->on ? r != SYNCROW_CREATE : r == SYNCROW_CREATE || r == SYNCROW_ENTER;
}

void syncmenu_open(SyncMenu *m, bool on, const char *code, const char *status) {
  memset(m, 0, sizeof *m);
  m->on = on;
  snprintf(m->code, sizeof m->code, "%s", code ? code : "");
  snprintf(m->status, sizeof m->status, "%s", status ? status : "");
  m->sel = on ? SYNCROW_NOW : SYNCROW_CREATE;
}

MenuAction syncmenu_press(SyncMenu *m, UiButton b, SyncRow *picked) {
  switch (b) {
  case UI_UP: case UI_DOWN: {
    int dir = b == UI_UP ? -1 : 1;
    do m->sel = (SyncRow)((m->sel + dir + SYNCROWS) % SYNCROWS);
    while (!syncmenu_row_shown(m, m->sel));
    return MENU_NONE;
  }
  case UI_ACCEPT: *picked = m->sel; return MENU_PICK;
  case UI_BACK: return MENU_BACK;
  default: return MENU_NONE;
  }
}

void codeentry_open(CodeEntry *e, const char *digits) {
  memset(e, 0, sizeof *e);
  for (int i = 0; i < 16; i++) e->digits[i] = digits && strlen(digits) == 16 ? digits[i] : '0';
}

MenuAction codeentry_press(CodeEntry *e, UiButton b) {
  char *d = &e->digits[e->pos];
  switch (b) {
  case UI_UP: *d = *d == '9' ? '0' : (char)(*d + 1); return MENU_NONE;
  case UI_DOWN: *d = *d == '0' ? '9' : (char)(*d - 1); return MENU_NONE;
  case UI_LEFT: e->pos = (e->pos + 15) % 16; return MENU_NONE;
  case UI_RIGHT: e->pos = (e->pos + 1) % 16; return MENU_NONE;
  case UI_ACCEPT: return MENU_PICK;
  case UI_BACK: return MENU_BACK;
  }
  return MENU_NONE;
}

// Digit i sits in group i/4, groups separated by a dash: x = 4 + 8 * (i + i / 4)
MenuAction conflict_press(ConflictView *v, UiButton b) {
  switch (b) {
  case UI_LEFT: v->sel = 0; return MENU_NONE;
  case UI_RIGHT: v->sel = 1; return MENU_NONE;
  case UI_ACCEPT: return MENU_PICK;
  case UI_BACK: return MENU_BACK;
  default: return MENU_NONE;
  }
}
