#pragma once
/*
 * CardputerCVBS.h - composite video output (240p, PAL or NTSC) of the terminal:
 * monochrome with 2 GPIOs, or 6 grey levels with 3 GPIOs.
 *
 * The ESP32-S3 has no DAC: the video levels come from GPIOs and resistors (a resistor DAC).
 *
 * Monochrome (2 pins):
 *   GPIO SYNC  (default G6) --[ 560 R ]--+
 *                                        +----> RCA centre (TV video in, 75 R inside the TV)
 *   GPIO VIDEO (default G5) --[ 220 R ]--+      (270 R works too)
 *   GND ----------------------------------> RCA shield
 *   sync = both low (0 V), black = SYNC high (~0.30 V), white = both high (~1.06 V)
 *
 * 6 grey levels (3 pins, binary-weighted 4:2:1; sync is the lowest code):
 *   G5 (bit 2) --[ 270 R ]--+
 *   G6 (bit 1) --[ 560 R ]--+----> RCA centre
 *   G3 (bit 0) --[ 1.1 k ]--+
 *   code 0 = sync 0 V, 1 = 0.15 V (unused), 2 = black 0.30 V, 3..6 = greys 0.45..0.92 V,
 *   7 = white 1.07 V (the TV's 75 R load makes the steps equal, ~0.15 V)
 *   With 270 R on G5 the monochrome mode still works (white ~0.96 V), so one circuit does both.
 *
 * The LCD peripheral (RGB mode, DMA, no frame buffer) streams one byte per pixel at 10 MHz
 * from two one-line "bounce" buffers; each data bit drives one pin (mono: bit 0 VIDEO, bit 1
 * SYNC; grey: bit 0 G3, bit 1 G6, bit 2 G5, so black = 2 in both). Horizontal and
 * vertical sync pulses are part of the data. An interrupt renders every line, just before it
 * is sent, straight from the terminal's character buffer, so no frame buffer is needed.
 * Because the sync pulses are in the data, a line that is not refilled in time (e.g. during a
 * flash write) only shows as a one-line glitch; the picture keeps its sync.
 *
 *   PAL-style 240p : 64.0 us lines, 312 lines, 50 Hz
 *   NTSC-style 240p: 63.556 us lines, 262 lines, 60 Hz
 *   80 columns x 6 px = 480 px (40 columns are drawn with double-width pixels).
 *   The pixel clock (default 10 MHz) sets the picture width: 480 px last 480 / f us. Only clocks
 *   with an integer divider are offered (fractional dividers make the pixels uneven, visible on
 *   the TV): 8.89, 9.23, 10.00, 10.91 and 11.43 MHz. All the other timings (line length, sync,
 *   porches) are computed in microseconds for the chosen clock.
 *   Text rows are 8 lines (25 rows = 200 lines, like the display), centred in the 240-line
 *   active area; CARDPUTER_CVBS_ROW_H 9 gives 9-line rows (the 9th line continues box /
 *   block / PETSCII graphics and inverse backgrounds).
 *   Calibration: a test frame marking the edges of the 80x25 text area can replace the text.
 *
 * Memory: about 6.5 KB, allocated only while the output is on.
 */
#include <stdint.h>
#include <string.h>
#ifdef ARDUINO_ARCH_ESP32
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#endif

#ifndef CARDPUTER_CVBS_PIN_VIDEO
#define CARDPUTER_CVBS_PIN_VIDEO 5      // to the 220 R
#endif
#ifndef CARDPUTER_CVBS_PIN_SYNC
#define CARDPUTER_CVBS_PIN_SYNC  6      // to the 560 R
#endif

#ifndef CARDPUTER_CVBS_PIN_LSB
#define CARDPUTER_CVBS_PIN_LSB   3      // grey mode only: to the 1.1 k (G7 is not on the header)
#endif

#ifndef CARDPUTER_CVBS_ROW_H
#define CARDPUTER_CVBS_ROW_H 8          // lines per text row: 8 (200 lines) or 9 (225 lines)
#endif

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

namespace cvbs {

// Pixel clocks with an integer divider: pixel = source / div (the driver divides by n, then by 2).
struct Clock { bool pll240; uint8_t div; };
static const Clock CLOCKS[] = {
  { false, 18 },   //  8.89 MHz (160 / 18)  widest
  { true,  26 },   //  9.23 MHz (240 / 26)
  { false, 16 },   // 10.00 MHz (160 / 16)  default
  { true,  22 },   // 10.91 MHz (240 / 22)
  { false, 14 },   // 11.43 MHz (160 / 14)  narrowest
};
static const int NCLOCKS = 5, CLOCK_DEF = 2;
static uint32_t clockHz(int i) {
  if (i < 0 || i >= NCLOCKS) i = CLOCK_DEF;
  return (CLOCKS[i].pll240 ? 240000000u : 160000000u) / CLOCKS[i].div;
}
static const int XOFF_MAX = 60, YOFF_MAX = 30;                    // calibration limits
static const int MAX_HTOTAL = 768;                                // 11.5 MHz x 64 us = 736
// The peripheral adds 3 blank clocks per line and 3 blank lines per frame; our data starts
// after the first 2 blank clocks (data index = line clock - 2).
static const int H_BLANK = 3, H_LEAD = 2, V_BLANK = 3, VSYNC_LINES = 3;
static const int MAX_COLS = 80, MAX_ROWS = 25, ROW_H = (CARDPUTER_CVBS_ROW_H == 8) ? 8 : 9, TEXT_W = 480;
enum : uint8_t { LV_SYNC = 0x00, LV_BLACK = 0x02, LV_WHITE = 0x03, LV_WHITE_GRAY = 0x07 };

// Standards in microseconds (converted to pixel clocks in start()).
struct StdUs { float line, sync, broadLow, activeStart, activeLen; int lines, activeTop; };
static const StdUs STDS[2] = {
  { 64.0f,   4.7f, 27.3f, 10.5f, 52.0f, 312, 44 },   // PAL 50 Hz
  { 63.556f, 4.7f, 27.1f,  9.4f, 52.6f, 262, 20 },   // NTSC 60 Hz
};
struct Std { int hTotal, lines, syncLen, broadLow, textX, activeTop, minX, maxX; };

// What the terminal shares with the renderer. Written by the terminal tasks, read by the ISR;
// a torn read only affects one line of one frame.
struct Source {
  const uint8_t *cells = nullptr;   // _scr: {glyph, attribute} pairs
  int stride = 0;                   // bytes per terminal row
  volatile int rows = 16, cols = 40;
  volatile int curRow = 0, curCol = 0;
  volatile bool curOn = false, pet = false;
  volatile bool test = false;       // calibration frame instead of the text
};


struct State {                      // allocated while the output is on
  Std std;
  int hRes, vRes;
  volatile int xOff, yOff;
  uint32_t pclk;                    // requested clock, Hz
  float pclkExact;                  // source / divider
  uint8_t lineBlank[MAX_HTOTAL];
  uint8_t lineBroad[MAX_HTOTAL];
  uint8_t font[2][256][8];          // [0] CP437, [1] PETSCII; bit 7 = leftmost of 6 pixels
  bool gray;                        // 6 grey levels (3 pins) instead of black/white
  uint8_t white;                    // level of white: 3 (mono) or 7 (grey)
  uint8_t fgLv[2][256], bgLv[2][256];   // level of foreground/background for each attribute;
                                        // [0] ANSI palette, [1] PETSCII palette
  uint8_t graphic[2][256];          // 1 = glyph continues into the 9th line (boxes, blocks, PETSCII)
};

static Source g_src;
static State *g_st = nullptr;
#ifdef ARDUINO_ARCH_ESP32
static esp_lcd_panel_handle_t g_panel = nullptr;
#endif

// Renders one data line (lenBytes = hRes) for data line index `line`. Plain function so that
// it can be tested on a PC; on the device it runs inside the DMA interrupt (IRAM).
static void IRAM_ATTR renderLine(uint8_t *buf, int line, int lenBytes) {
  State *s = g_st;
  if (line < VSYNC_LINES) { memcpy(buf, s->lineBroad, lenBytes); return; }
  memcpy(buf, s->lineBlank, lenBytes);
  int rows = g_src.rows, cols = g_src.cols;
  bool test = g_src.test;
  if (test) { rows = MAX_ROWS; cols = MAX_COLS; }
  if (rows > MAX_ROWS) rows = MAX_ROWS;
  if (cols > MAX_COLS) cols = MAX_COLS;
  int textH = rows * ROW_H;
  int ty = line - (s->std.activeTop + (240 - textH) / 2 + s->yOff);
  if (ty < 0 || ty >= textH) return;
  if (test) {                                            // calibration frame of the 80x25 area
    int x00 = s->std.textX - H_LEAD + s->xOff;
    if (x00 < s->std.minX) x00 = s->std.minX;
    if (x00 > s->std.maxX) x00 = s->std.maxX;
    uint8_t *q = buf + x00;
    int cy = textH / 2;
    bool edge = (ty < 2 || ty >= textH - 2);
    bool midRow = (ty == cy || ty == cy - 1);
    bool nearMid = (ty > cy - 16 && ty < cy + 15);
    bool tick = (ty < 8 || ty >= textH - 8);
    for (int i = 0; i < TEXT_W; i++) {
      bool w = edge || i < 3 || i >= TEXT_W - 3 ||
               (midRow && i > 200 && i < 280) || (nearMid && (i == 239 || i == 240)) ||
               (tick && (i % 60) == 0) ||                // marks every 10 columns
               ((ty % (ROW_H * 5)) == 0 && (i < 12 || i >= TEXT_W - 12));   // marks every 5 rows
      q[i] = w ? s->white : LV_BLACK;
    }
    if (s->gray && ty > cy + 20 && ty < cy + 50)          // grey ramp: black, 4 greys, white
      for (int i = 60; i < 420; i++) q[i] = (uint8_t)(LV_BLACK + (i - 60) / 60);
    return;
  }
  int row = ty / ROW_H, fy = ty % ROW_H;
  bool extra = (fy >= 8);                                // 9th line of the row
  int frow = extra ? 7 : fy;
  int x0 = s->std.textX - H_LEAD + s->xOff;
  if (x0 < s->std.minX) x0 = s->std.minX;
  if (x0 > s->std.maxX) x0 = s->std.maxX;
  bool pet = g_src.pet;
  const uint8_t (*font)[8] = s->font[pet ? 1 : 0];
  const uint8_t *fl = s->fgLv[pet ? 1 : 0], *bl = s->bgLv[pet ? 1 : 0];
  const uint8_t *gr = s->graphic[pet ? 1 : 0];
  const uint8_t *cell = g_src.cells + row * g_src.stride;
  int curCol = (g_src.curOn && g_src.curRow == row) ? g_src.curCol : -1;
  bool dbl = (cols <= 40);                               // 40 columns: double-width pixels
  uint8_t *p = buf + x0;
  for (int c = 0; c < cols; c++, cell += 2) {
    uint8_t g = cell[0];
    uint8_t bits = (extra && !gr[g]) ? 0 : font[g][frow];
    uint8_t f = fl[cell[1]], b = bl[cell[1]];
    if (c == curCol) {                                   // block cursor: swap the levels
      if (f == b) { bits = 0; b = (b >= 5) ? LV_BLACK : s->white; }
      else { uint8_t t = f; f = b; b = t; }
    }
    if (dbl) {
      for (int k = 0; k < 6; k++) { uint8_t v = (bits & (0x80 >> k)) ? f : b; p[0] = v; p[1] = v; p += 2; }
    } else {
      p[0] = (bits & 0x80) ? f : b;
      p[1] = (bits & 0x40) ? f : b;
      p[2] = (bits & 0x20) ? f : b;
      p[3] = (bits & 0x10) ? f : b;
      p[4] = (bits & 0x08) ? f : b;
      p[5] = (bits & 0x04) ? f : b;
      p += 6;
    }
  }
}

#ifdef ARDUINO_ARCH_ESP32
static bool IRAM_ATTR onBounce(esp_lcd_panel_handle_t, void *bounceBuf, int posPx, int lenBytes, void *) {
  if (g_st) renderLine((uint8_t *)bounceBuf, posPx / g_st->hRes, lenBytes);
  return false;
}
#endif

// Converts the standard to pixel clocks for the chosen clock.
static void computeStd(State *s, int standard) {
  const StdUs &u = STDS[standard == 2 ? 1 : 0];
  float f = s->pclkExact / 1e6f;                         // clocks per microsecond
  Std &d = s->std;
  d.hTotal = (int)(u.line * f + 0.5f);
  if (d.hTotal > MAX_HTOTAL) d.hTotal = MAX_HTOTAL;
  d.lines = u.lines;
  d.syncLen = (int)(u.sync * f + 0.5f);
  d.broadLow = (int)(u.broadLow * f + 0.5f);
  int aStart = (int)(u.activeStart * f + 0.5f), aLen = (int)(u.activeLen * f + 0.5f);
  d.textX = aStart + (aLen - TEXT_W) / 2;
  d.activeTop = u.activeTop;
  int hRes = d.hTotal - H_BLANK;
  d.minX = d.syncLen + (int)(3.0f * f);                  // keep 3 us of back porch for the TV's black clamp
  d.maxX = hRes - TEXT_W - (int)(1.0f * f);              // and 1 us of front porch
  if (d.maxX < d.minX) d.maxX = d.minX;
}

static void buildTemplates(State *s) {
  int half = s->std.hTotal / 2;
  for (int d = 0; d < s->hRes; d++) {
    int c = d + H_LEAD;
    s->lineBlank[d] = (c < s->std.syncLen) ? LV_SYNC : LV_BLACK;
    bool low = (c < s->std.broadLow) || (c >= half && c < half + s->std.broadLow);
    s->lineBroad[d] = low ? LV_SYNC : LV_BLACK;
  }
}

// Luma of an RGB565 colour, 0..255
static int luma565(uint16_t c) {
  int r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
  return (r * 299 + g * 587 + b * 114) / 1000;
}

// Levels of every attribute (background << 4 | foreground) for a palette.
// Mono: text on a darker background is white on black, on a brighter one black on white.
// Grey: each colour becomes one of 6 levels by brightness; two different colours are kept
// at least 2 levels apart, so text never disappears (e.g. dark blue on black).
static void buildLevels(uint8_t *fg, uint8_t *bg, const uint16_t *pal, bool gray) {
  for (int a = 0; a < 256; a++) {
    uint16_t cf = pal[a & 15], cb = pal[a >> 4];
    int lf = luma565(cf), lb = luma565(cb);
    if (!gray) {
      if (cf == cb) { fg[a] = bg[a] = (lb > 100) ? LV_WHITE : LV_BLACK; }
      else if (lb > lf) { fg[a] = LV_BLACK; bg[a] = LV_WHITE; }
      else { fg[a] = LV_WHITE; bg[a] = LV_BLACK; }
      continue;
    }
    int f = LV_BLACK + (lf * 5 + 127) / 255, b = LV_BLACK + (lb * 5 + 127) / 255;   // 2..7
    if (cf != cb && (f - b < 2 && b - f < 2)) {
      if (f >= b) { f = b + 2; if (f > LV_WHITE_GRAY) { f = LV_WHITE_GRAY; b = f - 2; } }
      else        { f = b - 2; if (f < LV_BLACK) { f = LV_BLACK; b = f + 2; } }
    }
    if (cf == cb) f = b;
    fg[a] = (uint8_t)f; bg[a] = (uint8_t)b;
  }
}

// Updates the colour rules (call when palettes change). Safe while running.
static void setPalettes(const uint16_t *ansi, const uint16_t *pet) {
  if (!g_st) return;
  buildLevels(g_st->fgLv[0], g_st->bgLv[0], ansi, g_st->gray);
  buildLevels(g_st->fgLv[1], g_st->bgLv[1], pet, g_st->gray);
}

// Fonts: CP437 from the display font, boxes/blocks and PETSCII graphics from the same bitmaps
// the display uses (6x8, bit 7 = leftmost pixel).
static void buildFonts(State *s, const uint8_t (*box)[8], const uint8_t (*petGfx)[8]) {
#ifdef ARDUINO_ARCH_ESP32
  lgfx::LGFX_Sprite g;
  g.setColorDepth(1);
  g.createSprite(6, 8);
  g.setFont(&fonts::Font0);
  g.cp437(true);
  g.setTextColor(1, 0);
  for (int ch = 0; ch < 256; ch++) {
    g.fillSprite(0);
    g.drawChar((uint16_t)ch, 0, 0);
    for (int y = 0; y < 8; y++) {
      uint8_t bits = 0;
      for (int x = 0; x < 6; x++) if (g.readPixel(x, y)) bits |= (uint8_t)(0x80 >> x);
      s->font[0][ch][y] = bits;
    }
  }
  g.deleteSprite();
#else
  for (int ch = 0; ch < 256; ch++)                  // PC tests: a recognisable dummy glyph
    for (int y = 0; y < 8; y++) s->font[0][ch][y] = (ch == ' ' || y == 7) ? 0 : (uint8_t)((ch << (y & 3)) & 0xF8);
#endif
  for (int i = 0; i < 48; i++) memcpy(s->font[0][0xB0 + i], box[i], 8);
  memcpy(s->font[1], s->font[0], sizeof(s->font[0]));
  for (int i = 0; i < 64; i++) memcpy(s->font[1][0x80 + i], petGfx[i], 8);
  for (int ch = 0; ch < 256; ch++) {
    s->graphic[0][ch] = (ch >= 0xB0 && ch <= 0xDF);      // CP437 shades, boxes, blocks
    s->graphic[1][ch] = (ch >= 0x80 && ch <= 0xDF);      // PETSCII graphics (+ the same boxes)
  }
}

static bool running() { return g_st != nullptr; }

// Picture position (calibration): takes effect on the next frame.
static void setOffsets(int x, int y) {
  if (!g_st) return;
  g_st->xOff = x; g_st->yOff = y;
}

static void stop() {
#ifdef ARDUINO_ARCH_ESP32
  if (g_panel) { esp_lcd_panel_del(g_panel); g_panel = nullptr; }
#endif
  State *s = g_st;
  g_st = nullptr;
  free(s);
}

// standard: 1 = PAL, 2 = NTSC; gray: 3-pin grey mode; clock: index into CLOCKS (sets the width);
// xOff / yOff: picture position. Returns false if it could not start (no RAM / driver error).
static bool start(int standard, bool gray, int clock, int xOff, int yOff,
                  const uint8_t (*box)[8], const uint8_t (*petGfx)[8],
                  const uint16_t *ansiPal, const uint16_t *petPal) {
  stop();
#ifdef ARDUINO_ARCH_ESP32
  State *s = (State *)heap_caps_malloc(sizeof(State), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
  State *s = (State *)malloc(sizeof(State));
#endif
  if (!s) return false;
  memset(s, 0, sizeof(*s));
  if (clock < 0 || clock >= NCLOCKS) clock = CLOCK_DEF;
  s->pclk = clockHz(clock);
  s->pclkExact = (CLOCKS[clock].pll240 ? 240e6f : 160e6f) / CLOCKS[clock].div;
  computeStd(s, standard);
  s->hRes = s->std.hTotal - H_BLANK;
  s->vRes = s->std.lines - V_BLANK;
  s->xOff = xOff;
  s->yOff = yOff;
  s->gray = gray;
  s->white = gray ? LV_WHITE_GRAY : LV_WHITE;
  buildTemplates(s);
  buildFonts(s, box, petGfx);
  buildLevels(s->fgLv[0], s->bgLv[0], ansiPal, gray);
  buildLevels(s->fgLv[1], s->bgLv[1], petPal, gray);
  g_st = s;
#ifdef ARDUINO_ARCH_ESP32
  esp_lcd_rgb_panel_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.clk_src = CLOCKS[clock].pll240 ? LCD_CLK_SRC_PLL240M : LCD_CLK_SRC_PLL160M;
  cfg.timings.pclk_hz = s->pclk;          // exact integer division of the source
  cfg.timings.h_res = s->hRes;
  cfg.timings.v_res = s->vRes;
  cfg.timings.hsync_pulse_width = 1;
  cfg.timings.hsync_back_porch = 1;
  cfg.timings.hsync_front_porch = 1;
  cfg.timings.vsync_pulse_width = 1;
  cfg.timings.vsync_back_porch = 1;
  cfg.timings.vsync_front_porch = 1;
  cfg.data_width = 8;
  cfg.bits_per_pixel = 8;
  cfg.bounce_buffer_size_px = s->hRes;      // one line per bounce buffer
  cfg.hsync_gpio_num = -1;
  cfg.vsync_gpio_num = -1;
  cfg.de_gpio_num = -1;
  cfg.pclk_gpio_num = -1;
  cfg.disp_gpio_num = -1;
  for (int i = 0; i < SOC_LCDCAM_RGB_DATA_WIDTH; i++) cfg.data_gpio_nums[i] = -1;
  if (gray) {                               // 3-bit DAC: bit 0 G3, bit 1 G6, bit 2 G5
    cfg.data_gpio_nums[0] = CARDPUTER_CVBS_PIN_LSB;
    cfg.data_gpio_nums[1] = CARDPUTER_CVBS_PIN_SYNC;
    cfg.data_gpio_nums[2] = CARDPUTER_CVBS_PIN_VIDEO;
  } else {
    cfg.data_gpio_nums[0] = CARDPUTER_CVBS_PIN_VIDEO;
    cfg.data_gpio_nums[1] = CARDPUTER_CVBS_PIN_SYNC;
  }
  cfg.flags.no_fb = 1;
  if (esp_lcd_new_rgb_panel(&cfg, &g_panel) != ESP_OK) { g_panel = nullptr; stop(); return false; }
  esp_lcd_rgb_panel_event_callbacks_t cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.on_bounce_empty = onBounce;
  if (esp_lcd_rgb_panel_register_event_callbacks(g_panel, &cbs, nullptr) != ESP_OK) { stop(); return false; }
  esp_lcd_panel_reset(g_panel);
  if (esp_lcd_panel_init(g_panel) != ESP_OK) { stop(); return false; }
#endif
  return true;
}

} // namespace cvbs
