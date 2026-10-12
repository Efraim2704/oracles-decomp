#include "gui/gui.h"
#include "stb_truetype.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char gui_vt323[];
extern const unsigned int gui_vt323_size;

static const UiColor PAPER = {0xf2, 0xf0, 0xe5}, MUTED = {0x93, 0x8e, 0x96}, SOFT = {0xb8, 0xb5, 0xb9},
                     LINE = {0x45, 0x44, 0x4f}, LINE_STRONG = {0x77, 0x73, 0x7f}, ERROR = {0xe0, 0x7a, 0x6e};

void gui_theme(GuiTheme *t, bool ages) {
  *t = (GuiTheme){.paper = PAPER, .muted = MUTED, .soft = SOFT, .line = LINE, .line_strong = LINE_STRONG, .error = ERROR};
  if (ages) {
    t->ink = (UiColor){0x16, 0x19, 0x2a};
    t->hi = (UiColor){0x3a, 0x68, 0xa0};
    t->on_hi = PAPER;
    t->hi_dim = (UiColor){0x28, 0x4c, 0x78};
    t->accent = (UiColor){0x68, 0xc2, 0xd3};
  } else {
    t->ink = (UiColor){0x26, 0x1a, 0x15};
    t->hi = (UiColor){0xe5, 0x56, 0x0b};
    t->on_hi = (UiColor){0x14, 0x0c, 0x08};
    t->hi_dim = (UiColor){0xa6, 0x3e, 0x07};
    t->accent = (UiColor){0xe2, 0x9b, 0x0c};
  }
}

// ---- the face: one atlas of printable ASCII plus the middle dot per pixel size ----

#define GLYPHS 96                       // ' '..'~', then U+00B7
#define ATLAS_SIZES 8
#define ATLAS_W 1024

typedef struct {
  int size;                             // em size in pixels; 0 when unused
  int atlas_h;
  float ascent, line;
  stbtt_packedchar chars[GLYPHS];
  uint8_t *alpha;
  SDL_Texture *tex;
  uint64_t used;
} Face;

static stbtt_fontinfo font;
static Face faces[ATLAS_SIZES];
static uint64_t face_clock;
static SDL_Renderer *ren;

static Face *face(int size) {
  if (size < 4) size = 4;
  Face *f = NULL, *oldest = &faces[0];
  for (int i = 0; i < ATLAS_SIZES && !f; i++) {
    if (faces[i].size == size) f = &faces[i];
    else if (faces[i].used < oldest->used) oldest = &faces[i];
  }
  if (!f) {
    f = oldest;
    if (f->tex) SDL_DestroyTexture(f->tex);
    free(f->alpha);
    memset(f, 0, sizeof *f);
    f->size = size;
    for (f->atlas_h = 256;; f->atlas_h *= 2) {
      f->alpha = calloc((size_t)ATLAS_W * f->atlas_h, 1);
      stbtt_pack_context pc;
      stbtt_PackBegin(&pc, f->alpha, ATLAS_W, f->atlas_h, 0, 1, NULL);
      stbtt_pack_range ranges[2] = {
        {STBTT_POINT_SIZE((float)size), 32, NULL, GLYPHS - 1, f->chars, 0, 0},
        {STBTT_POINT_SIZE((float)size), 0xb7, NULL, 1, f->chars + GLYPHS - 1, 0, 0},
      };
      bool ok = stbtt_PackFontRanges(&pc, gui_vt323, 0, ranges, 2) != 0;
      stbtt_PackEnd(&pc);
      if (ok || f->atlas_h >= 8192) break;
      free(f->alpha);
    }
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
    float s = stbtt_ScaleForMappingEmToPixels(&font, (float)size);
    f->ascent = ascent * s;
    f->line = (ascent - descent) * s;
  }
  f->used = ++face_clock;
  return f;
}

static SDL_Texture *face_texture(Face *f) {
  if (f->tex || !ren) return f->tex;
  SDL_Surface *s = SDL_CreateSurface(ATLAS_W, f->atlas_h, SDL_PIXELFORMAT_RGBA32);
  if (!s) return NULL;
  for (int y = 0; y < f->atlas_h; y++) {
    uint8_t *row = (uint8_t *)s->pixels + (size_t)y * s->pitch;
    for (int x = 0; x < ATLAS_W; x++) {
      row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 255;
      row[x * 4 + 3] = f->alpha[(size_t)y * ATLAS_W + x];
    }
  }
  f->tex = SDL_CreateTextureFromSurface(ren, s);
  SDL_DestroySurface(s);
  if (f->tex) SDL_SetTextureBlendMode(f->tex, SDL_BLENDMODE_BLEND);
  return f->tex;
}

// The glyph for the next character of s (UTF-8), and how many bytes it used.
static int glyph(const char *s, int32_t left, int *bytes) {
  unsigned char c = (unsigned char)s[0];
  *bytes = 1;
  if (c >= 32 && c < 127) return c - 32;
  if (c == 0xc2 && left > 1 && (unsigned char)s[1] == 0xb7) { *bytes = 2; return GLYPHS - 1; }
  if (c >= 0xc0) while (*bytes < left && ((unsigned char)s[*bytes] & 0xc0) == 0x80) (*bytes)++;
  return '?' - 32;
}

static Clay_Dimensions measure(Clay_StringSlice text, Clay_TextElementConfig *config, void *user) {
  (void)user;
  Face *f = face(config->fontSize);
  float w = 0;
  for (int32_t i = 0; i < text.length;) {
    int n, g = glyph(text.chars + i, text.length - i, &n);
    w += f->chars[g].xadvance + config->letterSpacing;
    i += n;
  }
  return (Clay_Dimensions){w, config->lineHeight ? config->lineHeight : f->line};
}

static void draw_text(const Clay_RenderCommand *cmd) {
  const Clay_TextRenderData *t = &cmd->renderData.text;
  Face *f = face(t->fontSize);
  SDL_Texture *tex = face_texture(f);
  if (!tex) return;
  SDL_SetTextureColorMod(tex, (Uint8)t->textColor.r, (Uint8)t->textColor.g, (Uint8)t->textColor.b);
  SDL_SetTextureAlphaMod(tex, (Uint8)t->textColor.a);
  float x = roundf(cmd->boundingBox.x), y = roundf(cmd->boundingBox.y + f->ascent);
  if (t->lineHeight) y += roundf((t->lineHeight - f->line) / 2);
  for (int32_t i = 0; i < t->stringContents.length;) {
    int n, g = glyph(t->stringContents.chars + i, t->stringContents.length - i, &n);
    stbtt_aligned_quad q;
    stbtt_GetPackedQuad(f->chars, ATLAS_W, f->atlas_h, g, &x, &y, &q, 1);
    SDL_FRect src = {q.s0 * ATLAS_W, q.t0 * f->atlas_h, (q.s1 - q.s0) * ATLAS_W, (q.t1 - q.t0) * f->atlas_h};
    SDL_FRect dst = {q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0};
    if (dst.w > 0) SDL_RenderTexture(ren, tex, &src, &dst);
    x += t->letterSpacing;
    i += n;
  }
}

void gui_label(const char *s, float cx, float cy, float size, UiColor c, uint8_t alpha) {
  Face *f = face((int)roundf(size));
  SDL_Texture *tex = face_texture(f);
  if (!tex) return;
  int32_t len = (int32_t)strlen(s);
  Clay_Dimensions d = measure((Clay_StringSlice){.length = len, .chars = s}, &(Clay_TextElementConfig){.fontSize = (uint16_t)f->size}, NULL);
  SDL_SetTextureColorMod(tex, c.r, c.g, c.b);
  SDL_SetTextureAlphaMod(tex, alpha);
  // the capitals, not the line, are centred: half the height of H below cy is the baseline
  int x0, y0, x1, y1;
  stbtt_GetCodepointBox(&font, 'H', &x0, &y0, &x1, &y1);
  float cap = (y1 - y0) * stbtt_ScaleForMappingEmToPixels(&font, (float)f->size);
  float x = roundf(cx - d.width / 2), y = roundf(cy + cap / 2);
  for (int32_t i = 0; i < len;) {
    int n, g = glyph(s + i, len - i, &n);
    stbtt_aligned_quad q;
    stbtt_GetPackedQuad(f->chars, ATLAS_W, f->atlas_h, g, &x, &y, &q, 1);
    SDL_FRect src = {q.s0 * ATLAS_W, q.t0 * f->atlas_h, (q.s1 - q.s0) * ATLAS_W, (q.t1 - q.t0) * f->atlas_h};
    SDL_FRect dst = {q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0};
    if (dst.w > 0) SDL_RenderTexture(ren, tex, &src, &dst);
    i += n;
  }
}

SDL_Renderer *gui_renderer(void) { return ren; }

// ---- Clay ----

static void on_error(Clay_ErrorData e) { fprintf(stderr, "gui: %.*s\n", (int)e.errorText.length, e.errorText.chars); }

bool gui_init(SDL_Renderer *r) {
  static bool done;
  if (r) ren = r;
  if (done) return true;
  if (!stbtt_InitFont(&font, gui_vt323, stbtt_GetFontOffsetForIndex(gui_vt323, 0))) return false;
  uint32_t size = Clay_MinMemorySize();
  void *mem = malloc(size);
  if (!mem) return false;
  Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(size, mem), (Clay_Dimensions){640, 480}, (Clay_ErrorHandler){on_error, NULL});
  Clay_SetMeasureTextFunction(measure, NULL);
  done = true;
  return true;
}

// ---- pointer input: the mouse, and the first finger of a direct touch screen ----

// what the screens offered for tapping, this frame and the last drawn one
#define TAPPABLE 128
static uint32_t tappable[2][TAPPABLE];
static int n_tappable[2];

void gui_none(void) { n_tappable[0] = n_tappable[1] = 0; }

bool gui_tappable(Clay_ElementId id) {
  for (int i = 0; i < n_tappable[1]; i++) if (tappable[1][i] == id.id) return true;
  return false;
}


#define TAP_SLOP 12.0f                  // points a press may move and still be a tap
#define PRESS_IDS 16

static struct {
  float x, y, down_x, down_y;
  bool down, pressed, released, moved, dragged, mouse;
  SDL_FingerID finger;
  bool finger_down;
  uint32_t press_ids[PRESS_IDS];
  int n_press;
  float wheel;
} ptr;

static float scale = 1;

static void pointer(float x, float y, int phase) {     // phase: -1 up, 0 move, 1 down
  ptr.x = x;
  ptr.y = y;
  if (phase == 1) {
    ptr.down = ptr.pressed = true;
    ptr.dragged = false;
    ptr.down_x = x;
    ptr.down_y = y;
    Clay_SetPointerState((Clay_Vector2){x, y}, true);
    Clay_ElementIdArray ids = Clay_GetPointerOverIds();
    ptr.n_press = 0;
    for (int i = 0; i < ids.length && ptr.n_press < PRESS_IDS; i++) ptr.press_ids[ptr.n_press++] = ids.internalArray[i].id;
    return;
  }
  if (ptr.down && fabsf(x - ptr.down_x) + fabsf(y - ptr.down_y) > TAP_SLOP * scale) ptr.dragged = true;
  if (phase == -1) { ptr.down = false; ptr.released = true; }
  Clay_SetPointerState((Clay_Vector2){x, y}, ptr.down);
}

// The press just made landed on something the last frame offered for tapping.
static bool press_on_tappable(void) {
  for (int i = 0; i < ptr.n_press; i++)
    for (int k = 0; k < n_tappable[1]; k++) if (ptr.press_ids[i] == tappable[1][k]) return true;
  return false;
}

bool gui_event(const SDL_Event *ev, bool mouse_is_finger) {
  SDL_Renderer *r = ren;
  if (!r || !gui_init(NULL)) return false;
  switch (ev->type) {
  case SDL_EVENT_MOUSE_MOTION:
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP: {
    if (ev->motion.which == SDL_TOUCH_MOUSEID) return false;
    if (ev->type != SDL_EVENT_MOUSE_MOTION && (mouse_is_finger || ev->button.button != SDL_BUTTON_LEFT)) return false;
    float x = ev->motion.x, y = ev->motion.y;
    if (ev->type != SDL_EVENT_MOUSE_MOTION) { x = ev->button.x; y = ev->button.y; }
    SDL_RenderCoordinatesFromWindow(r, x, y, &x, &y);
    ptr.mouse = true;
    if (ev->type == SDL_EVENT_MOUSE_MOTION) { if (!mouse_is_finger) { ptr.moved = true; pointer(x, y, 0); } }
    else pointer(x, y, ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 1 : -1);
    break;
  }
  case SDL_EVENT_MOUSE_WHEEL:
    ptr.wheel += ev->wheel.y;
    break;
  case SDL_EVENT_FINGER_DOWN:
  case SDL_EVENT_FINGER_MOTION:
  case SDL_EVENT_FINGER_UP:
  case SDL_EVENT_FINGER_CANCELED: {
    bool down = ev->type == SDL_EVENT_FINGER_DOWN;
    if (down && ptr.finger_down) return false;          // one finger drives the menus
    if (!down && (!ptr.finger_down || ev->tfinger.fingerID != ptr.finger)) return false;
    int w = 0, h = 0;
    SDL_GetCurrentRenderOutputSize(r, &w, &h);
    ptr.mouse = false;
    ptr.finger = ev->tfinger.fingerID;
    ptr.finger_down = down || ev->type == SDL_EVENT_FINGER_MOTION;
    if (ev->type == SDL_EVENT_FINGER_CANCELED) { ptr.down = false; ptr.n_press = 0; break; }
    pointer(ev->tfinger.x * w, ev->tfinger.y * h, down ? 1 : ev->type == SDL_EVENT_FINGER_UP ? -1 : 0);
    return down && press_on_tappable();
  }
  default: break;
  }
  return false;
}

bool gui_mouse_hovering(void) { return ptr.mouse; }

static bool tap_landed;

bool gui_tapped(Clay_ElementId id) {
  if (n_tappable[0] < TAPPABLE) tappable[0][n_tappable[0]++] = id.id;
  if (!ptr.released || ptr.dragged || !Clay_PointerOver(id)) return false;
  for (int i = 0; i < ptr.n_press; i++)
    if (ptr.press_ids[i] == id.id) return tap_landed = true;
  return false;
}

bool gui_take_tap(void) {
  bool t = tap_landed;
  tap_landed = false;
  return t;
}

bool gui_hovered(Clay_ElementId id) { return ptr.mouse && ptr.moved && Clay_PointerOver(id); }

// Drag and wheel scrolling for Clay's clip containers, once a frame before the layout.
static void gui_update_scroll(float dt) {
  Clay_UpdateScrollContainers(true, (Clay_Vector2){0, ptr.wheel * 3}, dt);
  ptr.wheel = 0;
}

// ---- frames ----

static char strings[32768];
static size_t strings_used;

Clay_String gui_str(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *p = strings + strings_used;
  size_t room = sizeof strings - strings_used;
  int n = vsnprintf(p, room, fmt, ap);
  va_end(ap);
  if (n < 0) n = 0;
  if ((size_t)n >= room) n = room ? (int)room - 1 : 0;
  strings_used += (size_t)n + 1;
  return (Clay_String){.length = n, .chars = p};
}

Clay_Color gui_rgb(UiColor c) { return (Clay_Color){c.r, c.g, c.b, 255}; }
Clay_Color gui_rgba(UiColor c, uint8_t alpha) { return (Clay_Color){c.r, c.g, c.b, alpha}; }

Clay_TextElementConfig *gui_text(float points, UiColor c) {
  return CLAY_TEXT_CONFIG({.textColor = gui_rgb(c), .fontSize = gui_px(points), .wrapMode = CLAY_TEXT_WRAP_WORDS});
}

float gui_scale(void) { return scale; }
uint16_t gui_px(float points) { float v = roundf(points * scale); return (uint16_t)(v < 1 ? 1 : v); }

float gui_display_scale(void) {
  SDL_Window *w = ren ? SDL_GetRenderWindow(ren) : NULL;
  float s = w ? SDL_GetWindowDisplayScale(w) : 1;
  return s > 0.5f ? s : 1;
}

void gui_begin(float s) {
  SDL_Renderer *r = ren;
  gui_init(NULL);
  scale = s > 0.25f ? s : 0.25f;
  strings_used = 0;
  int w = 0, h = 0;
  SDL_GetCurrentRenderOutputSize(r, &w, &h);
  Clay_SetLayoutDimensions((Clay_Dimensions){(float)w, (float)h});
  Clay_SetPointerState((Clay_Vector2){ptr.x, ptr.y}, ptr.down);
  static uint64_t last;
  uint64_t now = SDL_GetTicks();
  gui_update_scroll(last && now > last ? (now - last) / 1000.0f : 0.016f);
  last = now;
  Clay_BeginLayout();
}

static SDL_FRect frect(Clay_BoundingBox b) { return (SDL_FRect){b.x, b.y, b.width, b.height}; }

static void fill(Clay_BoundingBox b, Clay_Color c) {
  if (c.a <= 0) return;
  SDL_SetRenderDrawColor(ren, (Uint8)c.r, (Uint8)c.g, (Uint8)c.b, (Uint8)c.a);
  SDL_FRect f = frect(b);
  SDL_RenderFillRect(ren, &f);
}

void gui_end(void) {
  Clay_RenderCommandArray cmds = Clay_EndLayout();
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
  for (int32_t i = 0; i < cmds.length; i++) {
    Clay_RenderCommand *c = Clay_RenderCommandArray_Get(&cmds, i);
    Clay_BoundingBox b = c->boundingBox;
    switch (c->commandType) {
    case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: fill(b, c->renderData.rectangle.backgroundColor); break;
    case CLAY_RENDER_COMMAND_TYPE_BORDER: {
      Clay_BorderRenderData *d = &c->renderData.border;
      fill((Clay_BoundingBox){b.x, b.y, b.width, d->width.top}, d->color);
      fill((Clay_BoundingBox){b.x, b.y + b.height - d->width.bottom, b.width, d->width.bottom}, d->color);
      fill((Clay_BoundingBox){b.x, b.y, d->width.left, b.height}, d->color);
      fill((Clay_BoundingBox){b.x + b.width - d->width.right, b.y, d->width.right, b.height}, d->color);
      break;
    }
    case CLAY_RENDER_COMMAND_TYPE_TEXT: draw_text(c); break;
    case CLAY_RENDER_COMMAND_TYPE_IMAGE: {
      fill(b, c->renderData.image.backgroundColor);
      SDL_FRect f = frect(b);
      if (c->renderData.image.imageData) SDL_RenderTexture(ren, c->renderData.image.imageData, NULL, &f);
      break;
    }
    case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: {
      SDL_Rect r = {(int)b.x, (int)b.y, (int)ceilf(b.width), (int)ceilf(b.height)};
      SDL_SetRenderClipRect(ren, &r);
      break;
    }
    case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END: SDL_SetRenderClipRect(ren, NULL); break;
    default: break;
    }
  }
  memcpy(tappable[1], tappable[0], sizeof tappable[0]);
  n_tappable[1] = n_tappable[0];
  n_tappable[0] = 0;
  ptr.pressed = ptr.released = ptr.moved = false;
  if (!ptr.down) ptr.n_press = 0;
}

// ---- images ----

#define IMAGES 16
static struct { SDL_Texture *tex; uint32_t stamp; int w, h; SDL_Renderer *ren; } images[IMAGES];

SDL_Texture *gui_image(int key, const uint8_t *rgb, int w, int h, uint32_t stamp) {
  if (key < 0 || key >= IMAGES || !ren) return NULL;
  if (images[key].tex && (images[key].w != w || images[key].h != h || images[key].ren != ren)) { SDL_DestroyTexture(images[key].tex); images[key].tex = NULL; }
  if (!images[key].tex) {
    images[key].tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!images[key].tex) return NULL;
    SDL_SetTextureScaleMode(images[key].tex, SDL_SCALEMODE_NEAREST);
    images[key].w = w;
    images[key].h = h;
    images[key].ren = ren;
    images[key].stamp = stamp + 1;
  }
  if (images[key].stamp != stamp) { SDL_UpdateTexture(images[key].tex, NULL, rgb, w * 3); images[key].stamp = stamp; }
  return images[key].tex;
}
