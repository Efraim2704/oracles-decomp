#include "ui/menu.h"

bool pause_enabled(const PauseMenu *m, PauseItem i) {
  return (i != PAUSE_SAVE || m->can_save) && (i != PAUSE_LOAD || m->can_load);
}

void pause_open(PauseMenu *m, bool can_save, bool can_load) {
  m->sel = PAUSE_RESUME;
  m->can_save = can_save;
  m->can_load = can_load;
}

MenuAction pause_press(PauseMenu *m, UiButton b, PauseItem *picked) {
  switch (b) {
  case UI_UP: case UI_DOWN: {
    int dir = b == UI_UP ? -1 : 1;
    do m->sel = (PauseItem)((m->sel + dir + PAUSE_ITEMS) % PAUSE_ITEMS);
    while (!pause_enabled(m, m->sel));
    return MENU_NONE;
  }
  case UI_ACCEPT: *picked = m->sel; return MENU_PICK;
  case UI_BACK: return MENU_BACK;
  default: return MENU_NONE;
  }
}

void slots_open(SlotsMenu *m, bool save) {
  m->save = save;
  m->sel = 0;
  if (!save)
    for (int i = 0; i < UI_SLOTS; i++)
      if (m->slots[i].used) { m->sel = i; break; }
}

MenuAction slots_press(SlotsMenu *m, UiButton b, int *slot) {
  switch (b) {
  case UI_UP: case UI_DOWN: {
    int dir = b == UI_UP ? -1 : 1, start = m->sel;
    do m->sel = (m->sel + dir + UI_SLOTS) % UI_SLOTS;
    while (!m->save && !m->slots[m->sel].used && m->sel != start);
    return MENU_NONE;
  }
  case UI_ACCEPT:
    if (!m->save && !m->slots[m->sel].used) return MENU_NONE;
    *slot = m->sel;
    return MENU_PICK;
  case UI_BACK: return MENU_BACK;
  default: return MENU_NONE;
  }
}
