// The save-state slots, and how their times read.
#include "gui/widgets.h"
#include <stdio.h>

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
  int64_t now = now_time();
  keep_visible(CLAY_ID("SlotList"), CLAY_IDI("Slot", m->sel));
  PAGE(t, f) {
    if (header(t, m->save ? "Save state" : "Load state", NULL)) tapped = GUI_BACK;
    LIST("SlotList") {
      for (int i = 0; i < UI_SLOTS; i++) {
        const UiSlot *sl = &m->slots[i];
        Clay_ElementId id = CLAY_IDI("Slot", i);
        bool on = m->save || sl->used;
        if (pick(id, on, &m->sel, i)) tapped = i;
        bool sel = m->sel == i;
        TALL_ROW(t, id, sel) {
          state_info(t, sl->used ? gui_image(i, &sl->thumb[0][0][0], UI_W, UI_H, stamp) : NULL, THUMB_W, gui_str("Slot %d", i + 1), i == latest,
                     sl->used ? when_str(sl->time, now) : CLAY_STRING("Empty"), sel, on);
        }
      }
    }
    hint_line(t, f, m->save ? "Enter save   Esc back" : "Enter load   Esc back");
  }
  return tapped;
}
