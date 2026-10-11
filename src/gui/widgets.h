#pragma once
// The pieces the screens are built from, shared by src/gui/*.c only. Sizes are design points (the
// mockups' units); gui_px turns them into render pixels at the frame's scale.
#include "gui/screens.h"

#define ROW_H 44                  // every row is at least a 44pt touch target
#define TALL_ROW_H 104            // a row with a picture: a slot, a side of a conflict
#define PICTURE_ROW_H 72          // a row with a small picture: the main menu's states
#define THUMB_W 80                // a state's picture, 160x144 scaled
#define SMALL_THUMB_W 56
#define TEXT 24
#define SMALL 20
#define TAG 18
#define COLUMN_W 480
#define SIDE_PAD 16
#define TOAST_ROOM 58             // a message's height and gap, kept clear under the pause menu

Clay_Color gui_clear(void);
// One line of text that never wraps.
Clay_TextElementConfig *text_line(float points, UiColor c);
// A label set in small capitals' place: muted and letter-spaced.
Clay_TextElementConfig *text_spaced(float points, UiColor c, float spacing);
void label(const char *s, float points, UiColor c);

// Text colours for a row: on the selection bar, enabled, or greyed out; and its second line.
UiColor row_fg(const GuiTheme *t, bool selected, bool enabled);
UiColor row_dim(const GuiTheme *t, bool selected);

// Takes the free width in a row, pushing what follows to the right.
void spacer(void);
// A small outlined word after a label: Latest.
void tag(Clay_String s, UiColor c);
// The square before On (filled) and Off (outlined), so the state never rests on colour alone.
void mark(bool on, UiColor c);
// A state's picture, w points wide at the game's shape, or an empty frame where there is none.
void thumb(const GuiTheme *t, SDL_Texture *tex, bool selected, float w);
// Inside a picture row: a state's picture, its name (with Latest), and when it was saved under it.
void state_info(const GuiTheme *t, SDL_Texture *tex, float thumb_w, Clay_String name, bool latest, Clay_String when, bool selected, bool enabled);
// "Today 14:02:37" and the rest of gui_when, as frame text.
Clay_String when_str(int64_t time, int64_t now);
int64_t now_time(void);
// "LINK  3 hearts  4/8 essences", or "Empty".
Clay_String file_str(const UiFileInfo *fi);

// A list row's box: the selection bar when selected, the cursor drawn first. Use as
//   ROW(t, id, selected) { label(...); spacer(); ... }
// TALL_ROW and PICTURE_ROW grow from their height to fit a picture and two lines.
Clay_ElementDeclaration row_box(const GuiTheme *t, Clay_ElementId id, bool selected, float min_h);
void row_cursor(const GuiTheme *t, bool selected);
#define ROW_(t, id, sel, min_h)                                  \
  CLAY(row_box((t), (id), (sel), (min_h)))                       \
  for (int row_once_ = (row_cursor((t), (sel)), 0); !row_once_; row_once_ = 1)
#define ROW(t, id, sel) ROW_(t, id, sel, 0)
#define TALL_ROW(t, id, sel) ROW_(t, id, sel, TALL_ROW_H)
#define PICTURE_ROW(t, id, sel) ROW_(t, id, sel, PICTURE_ROW_H)

// The common row: label, optional tag, an On/Off mark or a value at the right.
typedef struct {
  Clay_ElementId id;
  Clay_String label, value, tag;
  bool selected, enabled;
  int toggle;                   // 0 none, 1 On, 2 Off
} Row;
void row(const GuiTheme *t, Row r);
// A row on its own (Connect, Add ROM): true when tapped, never when disabled.
bool tap_row(const GuiTheme *t, Row r);

// A row in a list the keys move through: hovering selects it, and a tap selects it and returns
// true. Disabled rows take neither. pick is for rows drawn by hand; list_row draws a Row, selected
// when *sel is i.
bool pick(Clay_ElementId id, bool enabled, int *sel, int i);
bool list_row(const GuiTheme *t, Row r, int *sel, int i);

// A muted, letter-spaced heading over a group of rows.
void section(const GuiTheme *t, const char *s);
// Back on the left, the title in the middle, a keyboard hint on the right. True when Back was tapped.
bool header(const GuiTheme *t, const char *title, const char *hint);
// Underlined tabs; returns the one tapped, or GUI_NONE. focus tints the selected tab (the tab row,
// not the list under it, has the keys).
int tab_bar(const GuiTheme *t, const char *id, const char *const *labels, int n, int selected, bool focus);
// The keyboard hint under a page, on keyboard screens only.
void hint_line(const GuiTheme *t, const GuiFrame *f, const char *s);
// A paragraph inside a page's padding.
void paragraph(Clay_String s, float points, UiColor c, float top, float bottom);

// The display's scale, grown (up to half again) when a big window leaves the column small.
float page_scale(const GuiFrame *f);
// A full-screen page: the theme's background, one column centred inside the safe area.
//   gui_begin(page_scale(f)); PAGE(t, f) { header(...); LIST("Name") { rows } }
Clay_ElementDeclaration page_box(const GuiTheme *t, const GuiFrame *f);
Clay_ElementDeclaration column_box(void);
#define PAGE(t, f) CLAY(page_box((t), (f))) CLAY(column_box())
// A list that scrolls (drag, wheel) inside the rest of the page.
Clay_ElementDeclaration list_box(Clay_ElementId id);
#define LIST(name) CLAY(list_box(CLAY_ID(name)))
// Scrolls the list so the item drawn last frame is inside it (keys move the selection off screen).
void keep_visible(Clay_ElementId list, Clay_ElementId item);

// The settings rows, shared by the settings page and the main menu's settings tab.
void settings_rows(SettingsMenu *m, const Settings *s, const char *sync_label, const GuiTheme *t, bool focused, GuiSettingsTap *tap);
