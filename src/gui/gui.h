#pragma once
// Menus drawn at the screen's own resolution, over the game picture: Clay lays them out, SDL draws
// them, in VT323. Everything is in render pixels; the screens size things in design points times
// the frame's scale (gui_px). The game-pixel canvas in src/ui stays for the touch overlay.
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include "clay.h"
#include "ui/ui.h"

// The calm theme. The shared colours come from the owner's palette, the highlight and accent from
// each game's box cover (Ages blue, Seasons orange). Every text colour clears 4.5:1 on what it sits on.
typedef struct {
  UiColor ink;          // page background
  UiColor paper;        // text
  UiColor muted;        // section labels, secondary text, hints, Off
  UiColor soft;         // titles
  UiColor line;         // dividers, scrollbars
  UiColor line_strong;  // outlines that must read as controls (3:1)
  UiColor hi;           // the selection bar
  UiColor on_hi;        // text on the selection bar
  UiColor hi_dim;       // unlit parts drawn on the selection bar
  UiColor accent;       // On, Latest, links
  UiColor error;        // destructive actions and errors
} GuiTheme;

void gui_theme(GuiTheme *t, bool ages);

// Draws with this renderer from now on.
bool gui_init(SDL_Renderer *ren);
// Mouse and touch go to the menus; mouse_is_finger: the mouse already arrives as touch events.
// True when a finger came down on something the menu offers for tapping: that finger is the menu's.
bool gui_event(const SDL_Event *ev, bool mouse_is_finger);
// The last pointer input was a mouse (hover may move the selection), not a finger.
bool gui_mouse_hovering(void);

// One frame: begin, declare elements with CLAY(), end draws them. scale: render pixels per point.
void gui_begin(float scale);
void gui_end(void);
float gui_scale(void);
uint16_t gui_px(float points);
// render pixels per point the display asks for (1 on a plain desktop, about 3 on a phone)
float gui_display_scale(void);

// A tap or click that pressed and released on this element (and did not drag).
bool gui_tapped(Clay_ElementId id);
// Some element was tapped since the last call (for the tap's haptic tick).
bool gui_take_tap(void);
// A frame with no menu on screen: nothing is offered for tapping. Without it, the last menu drawn
// (the main menu's rows fill the screen) would go on claiming fingers meant for the controls.
void gui_none(void);
// The last frame drawn offered this element for tapping (tests tap only what is on screen).
bool gui_tappable(Clay_ElementId id);
// The mouse moved onto this element this frame.
bool gui_hovered(Clay_ElementId id);

// Text that lives until the frame is drawn.
Clay_String gui_str(const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);
Clay_Color gui_rgb(UiColor c);
Clay_Color gui_rgba(UiColor c, uint8_t alpha);
Clay_TextElementConfig *gui_text(float points, UiColor c);
// Drawn at once, outside any frame: text centred on (cx, cy) by its capitals, size in pixels.
void gui_label(const char *s, float cx, float cy, float size, UiColor c, uint8_t alpha);
SDL_Renderer *gui_renderer(void);
// key: a small slot number for the cache; stamp changes when the pixels do
SDL_Texture *gui_image(int key, const uint8_t *rgb, int w, int h, uint32_t stamp);
