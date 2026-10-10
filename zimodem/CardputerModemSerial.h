#pragma once
/*
 * CardputerModemSerial.h
 *
 * Stream wrapper to run Zimodem on the M5Stack Cardputer / Cardputer ADV.
 * It replaces Zimodem's HWSerial object:
 *  - INPUT:  merges the UART (retro computer) and the Cardputer keyboard
 *  - OUTPUT: everything Zimodem writes goes to the UART AND to a character
 *            terminal on the display (40x16 window onto a virtual screen
 *            of up to 80x25)
 *
 * FEATURES
 *  - real-time terminal: keyboard and display run in a dedicated FreeRTOS
 *    task (core 0), so the screen keeps scrolling even while Zimodem is
 *    busy in long/blocking loops
 *  - scrollback (CARDPUTER_SCROLLBACK_LINES rows), status bar (WiFi, baud,
 *    call state), ANSI colors (16 colors, SGR) and basic cursor/erase codes
 *  - encodings: CP437 (DOS), UTF-8 and PETSCII (C64 colors, cursor, reverse,
 *    graphic symbols); outside a call PETSCII is shown as plain ASCII so
 *    that AT commands stay readable
 *  - optional 80-column and 25-row virtual screens with manual horizontal pan
 *  - settings menu: virtual screen, text color, UART RX/TX pins, baud rate,
 *    UART input on/off, encoding (stored in NVS)
 *  - phonebook with 10 BBS entries (name + host[:port]), stored in NVS;
 *    dialing types "ATDT host[:port]" into Zimodem for you
 *  - when the baud rate is changed in the menu, "AT&W" is sent to Zimodem
 *    (if no call is open) so that it is saved in its configuration
 *  - pull-up on the RX pin (a floating RX produced random characters)
 *
 * Must be included BEFORE any Zimodem #define (in particular
 * "#define null 0"), because the M5 libraries don't like it.
 *
 * KEYS
 *   Ctrl+letter        control code (Ctrl+C = 0x03, etc.)
 *   Fn + `             ESC
 *   Fn + 1             settings menu (Up/Down item, Left/Right value, Enter/Esc)
 *   Fn + 2             phonebook (Up/Down slot, Enter dial, Tab edit, Esc exit)
 *   Fn + 3             display encoding: CP437 -> UTF-8 -> PETSCII
 *   Fn + Left/Right    (with 80 columns active) pans the window by 1 column
 *                      (repeats when held down)
 *   Fn + Backspace     Del (ESC [ 3 ~)
 *   Fn + Up / Down     scrollback by 1 row (repeats when held down)
 *   Fn + Left/Right    (80 columns off) scrollback one page back / forward
 *   Shift+Fn+arrows    sends the arrow keys to the host (ESC [ A/B/C/D)
 *   any other key typed brings the screen back to the bottom (live)
 * The Cardputer arrows are ; . , / on the Fn layer: Up=; Down=. Left=, Right=/
 */
#include <Arduino.h>
#include <WiFi.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#ifdef ARDUINO_ARCH_ESP32
#include "driver/gpio.h"
#endif
#include <vector>
#include <algorithm>

#ifndef CARDPUTER_MODEM_RX
#define CARDPUTER_MODEM_RX 1   // Grove G1 (check against your board's pinout)
#endif
#ifndef CARDPUTER_MODEM_TX
#define CARDPUTER_MODEM_TX 2   // Grove G2
#endif
#ifndef CARDPUTER_SCROLLBACK_LINES
#define CARDPUTER_SCROLLBACK_LINES 300   // 2 bytes per cell: 300 rows = 24 KB
#endif
// "Normal" text color (ANSI 37 / default). 0x07E0 = phosphor green as
// before; use 0xAD55 for the "authentic" ANSI gray.
#ifndef CARDPUTER_TERM_DEFAULT_FG
#define CARDPUTER_TERM_DEFAULT_FG 0x07E0
#endif

// Initial encoding of received bytes >= 0x80: 0 = CP437 (DOS), 1 = UTF-8.
// Can be changed on the fly with Fn+3.
#ifndef CARDPUTER_TERM_UTF8_DEFAULT
#define CARDPUTER_TERM_UTF8_DEFAULT 0
#endif

// ---- generated data (see gen.py): CP437 box-drawing glyphs in a 6x8 cell ----
// Generated from a geometric description (up/down/left/right arms, single or double):
// the lines reach the cell edges, so the boxes come out continuous.
// 48 glyphs (CP437 0xB0..0xDF) x 8 rows; bit 7 = leftmost pixel, 6 pixels per row.
static const uint8_t CPMS_BOX[48][8] = {
  {0xA8,0x00,0xA8,0x00,0xA8,0x00,0xA8,0x00},  // B0
  {0xA8,0x54,0xA8,0x54,0xA8,0x54,0xA8,0x54},  // B1
  {0xFC,0xA8,0xFC,0xA8,0xFC,0xA8,0xFC,0xA8},  // B2
  {0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10},  // B3
  {0x10,0x10,0x10,0x10,0xF0,0x10,0x10,0x10},  // B4
  {0x10,0x10,0x10,0xF0,0x10,0xF0,0x10,0x10},  // B5
  {0x28,0x28,0x28,0x28,0xE8,0x28,0x28,0x28},  // B6
  {0x00,0x00,0x00,0x00,0xF8,0x28,0x28,0x28},  // B7
  {0x00,0x00,0x00,0xF0,0x10,0xF0,0x10,0x10},  // B8
  {0x28,0x28,0x28,0xE8,0x08,0xE8,0x28,0x28},  // B9
  {0x28,0x28,0x28,0x28,0x28,0x28,0x28,0x28},  // BA
  {0x00,0x00,0x00,0xF8,0x08,0xE8,0x28,0x28},  // BB
  {0x28,0x28,0x28,0xE8,0x08,0xF8,0x00,0x00},  // BC
  {0x28,0x28,0x28,0x28,0xF8,0x00,0x00,0x00},  // BD
  {0x10,0x10,0x10,0xF0,0x10,0xF0,0x00,0x00},  // BE
  {0x00,0x00,0x00,0x00,0xF0,0x10,0x10,0x10},  // BF
  {0x10,0x10,0x10,0x10,0x1C,0x00,0x00,0x00},  // C0
  {0x10,0x10,0x10,0x10,0xFC,0x00,0x00,0x00},  // C1
  {0x00,0x00,0x00,0x00,0xFC,0x10,0x10,0x10},  // C2
  {0x10,0x10,0x10,0x10,0x1C,0x10,0x10,0x10},  // C3
  {0x00,0x00,0x00,0x00,0xFC,0x00,0x00,0x00},  // C4
  {0x10,0x10,0x10,0x10,0xFC,0x10,0x10,0x10},  // C5
  {0x10,0x10,0x10,0x1C,0x10,0x1C,0x10,0x10},  // C6
  {0x28,0x28,0x28,0x28,0x2C,0x28,0x28,0x28},  // C7
  {0x28,0x28,0x28,0x2C,0x20,0x3C,0x00,0x00},  // C8
  {0x00,0x00,0x00,0x3C,0x20,0x2C,0x28,0x28},  // C9
  {0x28,0x28,0x28,0xEC,0x00,0xFC,0x00,0x00},  // CA
  {0x00,0x00,0x00,0xFC,0x00,0xEC,0x28,0x28},  // CB
  {0x28,0x28,0x28,0x2C,0x20,0x2C,0x28,0x28},  // CC
  {0x00,0x00,0x00,0xFC,0x00,0xFC,0x00,0x00},  // CD
  {0x28,0x28,0x28,0xEC,0x00,0xEC,0x28,0x28},  // CE
  {0x10,0x10,0x10,0xFC,0x00,0xFC,0x00,0x00},  // CF
  {0x28,0x28,0x28,0x28,0xFC,0x00,0x00,0x00},  // D0
  {0x00,0x00,0x00,0xFC,0x00,0xFC,0x10,0x10},  // D1
  {0x00,0x00,0x00,0x00,0xFC,0x28,0x28,0x28},  // D2
  {0x28,0x28,0x28,0x28,0x3C,0x00,0x00,0x00},  // D3
  {0x10,0x10,0x10,0x1C,0x10,0x1C,0x00,0x00},  // D4
  {0x00,0x00,0x00,0x1C,0x10,0x1C,0x10,0x10},  // D5
  {0x00,0x00,0x00,0x00,0x3C,0x28,0x28,0x28},  // D6
  {0x28,0x28,0x28,0x28,0xFC,0x28,0x28,0x28},  // D7
  {0x10,0x10,0x10,0xFC,0x10,0xFC,0x10,0x10},  // D8
  {0x10,0x10,0x10,0x10,0xF0,0x00,0x00,0x00},  // D9
  {0x00,0x00,0x00,0x00,0x1C,0x10,0x10,0x10},  // DA
  {0xFC,0xFC,0xFC,0xFC,0xFC,0xFC,0xFC,0xFC},  // DB
  {0x00,0x00,0x00,0x00,0xFC,0xFC,0xFC,0xFC},  // DC
  {0xE0,0xE0,0xE0,0xE0,0xE0,0xE0,0xE0,0xE0},  // DD
  {0x1C,0x1C,0x1C,0x1C,0x1C,0x1C,0x1C,0x1C},  // DE
  {0xFC,0xFC,0xFC,0xFC,0x00,0x00,0x00,0x00},  // DF
};
// Unicode U+00A0..U+00FF -> CP437 code (or fallback ASCII character)
static const uint8_t CPMS_LATIN1[96] = {
  0x20,0xAD,0x9B,0x9C,0x6F,0x9D,0x7C,0x15,0x22,0x63,0xA6,0xAE,0xAA,0x2D,0x52,0x2D,
  0xF8,0xF1,0xFD,0x33,0x27,0xE6,0x14,0xFA,0x2C,0x31,0xA7,0xAF,0xAC,0xAB,0x7E,0xA8,
  0x41,0x41,0x41,0x41,0x8E,0x8F,0x92,0x80,0x45,0x90,0x45,0x45,0x49,0x49,0x49,0x49,
  0x44,0xA5,0x4F,0x4F,0x4F,0x4F,0x99,0x78,0x4F,0x55,0x55,0x55,0x9A,0x59,0x50,0xE1,
  0x85,0xA0,0x83,0x61,0x84,0x86,0x91,0x87,0x8A,0x82,0x88,0x89,0x8D,0xA1,0x8C,0x8B,
  0x64,0xA4,0x95,0xA2,0x93,0x6F,0x94,0xF6,0x6F,0x97,0xA3,0x96,0x81,0x79,0x70,0x98,
};
// Unicode U+2500..U+259F (boxes and blocks) -> CP437 code (0x3F = '?')
static const uint8_t CPMS_BOXMAP[160] = {
  0xC4,0xC4,0xB3,0xB3,0xC4,0xC4,0xB3,0xB3,0xC4,0xC4,0xB3,0xB3,0xDA,0xDA,0xDA,0xDA,
  0xBF,0xBF,0xBF,0xBF,0xC0,0xC0,0xC0,0xC0,0xD9,0xD9,0xD9,0xD9,0xC3,0xC3,0xC3,0xC3,
  0xC3,0xC3,0xC3,0xC3,0xB4,0xB4,0xB4,0xB4,0xB4,0xB4,0xB4,0xB4,0xC2,0xC2,0xC2,0xC2,
  0xC2,0xC2,0xC2,0xC2,0xC1,0xC1,0xC1,0xC1,0xC1,0xC1,0xC1,0xC1,0xC5,0xC5,0xC5,0xC5,
  0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC4,0xC4,0xB3,0xB3,
  0xCD,0xBA,0xD5,0xD6,0xC9,0xB8,0xB7,0xBB,0xD4,0xD3,0xC8,0xBE,0xBD,0xBC,0xC6,0xC7,
  0xCC,0xB5,0xB6,0xB9,0xD1,0xD2,0xCB,0xCF,0xD0,0xCA,0xD8,0xD7,0xCE,0xDA,0xBF,0xD9,
  0xC0,0x2F,0x5C,0x58,0xC4,0xB3,0xC4,0xB3,0xC4,0xB3,0xC4,0xB3,0xC4,0xB3,0xC4,0xB3,
  0xDF,0x5F,0x5F,0x5F,0xDC,0xDC,0xDC,0xDC,0xDB,0xDB,0xDD,0xDD,0xDD,0xDD,0xDD,0xDD,
  0xDE,0xB0,0xB1,0xB2,0xDF,0xDE,0xDC,0xDC,0xDF,0xDB,0xB1,0xDB,0xDB,0xDF,0xB1,0xDB,
};
struct CpmsUMap { uint16_t u; uint8_t c; };
// Other characters (Greek, math, symbols, typographic quotes...)
static const CpmsUMap CPMS_MISC[] = {
  {0x0192,0x9F},
  {0x0393,0xE2},
  {0x0398,0xE9},
  {0x03A3,0xE4},
  {0x03A6,0xE8},
  {0x03A9,0xEA},
  {0x03B1,0xE0},
  {0x03B4,0xEB},
  {0x03B5,0xEE},
  {0x03BC,0xE6},
  {0x03C0,0xE3},
  {0x03C3,0xE5},
  {0x03C4,0xE7},
  {0x03C6,0xED},
  {0x2013,0x2D},
  {0x2014,0x2D},
  {0x2018,0x27},
  {0x2019,0x27},
  {0x201A,0x2C},
  {0x201C,0x22},
  {0x201D,0x22},
  {0x201E,0x22},
  {0x2022,0x07},
  {0x2026,0x2E},
  {0x203C,0x13},
  {0x207F,0xFC},
  {0x20A7,0x9E},
  {0x20AC,0x45},
  {0x2190,0x1B},
  {0x2191,0x18},
  {0x2192,0x1A},
  {0x2193,0x19},
  {0x2194,0x1D},
  {0x2195,0x12},
  {0x21A8,0x17},
  {0x2212,0x2D},
  {0x2215,0x2F},
  {0x2219,0xF9},
  {0x221A,0xFB},
  {0x221E,0xEC},
  {0x221F,0x1C},
  {0x2229,0xEF},
  {0x2248,0xF7},
  {0x2261,0xF0},
  {0x2264,0xF3},
  {0x2265,0xF2},
  {0x2302,0x7F},
  {0x2310,0xA9},
  {0x2320,0xF4},
  {0x2321,0xF5},
  {0x25A0,0xFE},
  {0x25AA,0xFE},
  {0x25AC,0x16},
  {0x25B2,0x1E},
  {0x25B6,0x10},
  {0x25BA,0x10},
  {0x25BC,0x1F},
  {0x25C0,0x11},
  {0x25C4,0x11},
  {0x25CB,0x09},
  {0x25CF,0x07},
  {0x25D8,0x08},
  {0x25D9,0x0A},
  {0x25E6,0x09},
  {0x2605,0x2A},
  {0x2606,0x2A},
  {0x263A,0x01},
  {0x263B,0x02},
  {0x263C,0x0F},
  {0x2640,0x0C},
  {0x2642,0x0B},
  {0x2660,0x06},
  {0x2663,0x05},
  {0x2665,0x03},
  {0x2666,0x04},
  {0x266A,0x0D},
  {0x266B,0x0E},
  {0x2713,0xFB},
  {0x2714,0xFB},
  {0x2764,0x03},
};

// ---- PETSCII graphic glyphs (C64 screen codes 0x40..0x7F) in a 6x8 cell; bit 7 = leftmost pixel ----
static const uint8_t CPMS_PET[64][8] = {
  {0x00,0x00,0x00,0x00,0xFC,0x00,0x00,0x00},  // 40
  {0x30,0x78,0xFC,0xFC,0xFC,0x30,0x78,0x00},  // 41
  {0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10},  // 42
  {0x00,0x00,0x00,0xFC,0x00,0x00,0x00,0x00},  // 43
  {0x00,0x00,0xFC,0x00,0x00,0x00,0x00,0x00},  // 44
  {0x00,0xFC,0x00,0x00,0x00,0x00,0x00,0x00},  // 45
  {0x00,0x00,0x00,0x00,0x00,0xFC,0x00,0x00},  // 46
  {0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20},  // 47
  {0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08},  // 48
  {0x00,0x00,0x00,0x00,0xE0,0x10,0x10,0x10},  // 49
  {0x10,0x10,0x10,0x10,0x0C,0x00,0x00,0x00},  // 4A
  {0x10,0x10,0x10,0x10,0xE0,0x00,0x00,0x00},  // 4B
  {0x80,0x80,0x80,0x80,0x80,0x80,0x80,0xFC},  // 4C
  {0x80,0x80,0x40,0x20,0x10,0x10,0x08,0x04},  // 4D
  {0x04,0x04,0x08,0x10,0x20,0x20,0x40,0x80},  // 4E
  {0xFC,0x80,0x80,0x80,0x80,0x80,0x80,0x80},  // 4F
  {0xFC,0x04,0x04,0x04,0x04,0x04,0x04,0x04},  // 50
  {0x00,0x00,0x30,0x78,0x78,0x30,0x00,0x00},  // 51
  {0x00,0x00,0x00,0x00,0x00,0x00,0xFC,0x00},  // 52
  {0x00,0xD8,0xFC,0xFC,0xFC,0x78,0x30,0x00},  // 53
  {0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x40},  // 54
  {0x00,0x00,0x00,0x00,0x0C,0x10,0x10,0x10},  // 55
  {0x84,0x84,0x48,0x30,0x30,0x30,0x48,0x84},  // 56
  {0x00,0x30,0x48,0x84,0x84,0x48,0x30,0x00},  // 57
  {0x30,0x78,0x30,0xCC,0xFC,0xCC,0x30,0x78},  // 58
  {0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04},  // 59
  {0x00,0x30,0x78,0xFC,0xFC,0x78,0x30,0x00},  // 5A
  {0x10,0x10,0x10,0x10,0xFC,0x10,0x10,0x10},  // 5B
  {0xA0,0x40,0xA0,0x40,0xA0,0x40,0xA0,0x40},  // 5C
  {0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10},  // 5D
  {0x00,0x00,0xFC,0x48,0x48,0x48,0x4C,0x00},  // 5E
  {0xFC,0xFC,0x7C,0x3C,0x1C,0x1C,0x0C,0x04},  // 5F
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 60
  {0xE0,0xE0,0xE0,0xE0,0xE0,0xE0,0xE0,0xE0},  // 61
  {0x00,0x00,0x00,0x00,0xFC,0xFC,0xFC,0xFC},  // 62
  {0xFC,0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 63
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFC},  // 64
  {0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80},  // 65
  {0xA8,0x54,0xA8,0x54,0xA8,0x54,0xA8,0x54},  // 66
  {0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04},  // 67
  {0x00,0x00,0x00,0x00,0xA8,0x54,0xA8,0x54},  // 68
  {0xFC,0xFC,0xF8,0xF0,0xE0,0xE0,0xC0,0x80},  // 69
  {0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C},  // 6A
  {0x10,0x10,0x10,0x10,0x1C,0x10,0x10,0x10},  // 6B
  {0x00,0x00,0x00,0x00,0x1C,0x1C,0x1C,0x1C},  // 6C
  {0x10,0x10,0x10,0x10,0x1C,0x00,0x00,0x00},  // 6D
  {0x00,0x00,0x00,0x00,0xF0,0x10,0x10,0x10},  // 6E
  {0x00,0x00,0x00,0x00,0x00,0x00,0xFC,0xFC},  // 6F
  {0x00,0x00,0x00,0x00,0x1C,0x10,0x10,0x10},  // 70
  {0x10,0x10,0x10,0x10,0xFC,0x00,0x00,0x00},  // 71
  {0x00,0x00,0x00,0x00,0xFC,0x10,0x10,0x10},  // 72
  {0x10,0x10,0x10,0x10,0xF0,0x10,0x10,0x10},  // 73
  {0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0},  // 74
  {0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0},  // 75
  {0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C},  // 76
  {0xFC,0xFC,0x00,0x00,0x00,0x00,0x00,0x00},  // 77
  {0xFC,0xFC,0xFC,0x00,0x00,0x00,0x00,0x00},  // 78
  {0x00,0x00,0x00,0x00,0x00,0xFC,0xFC,0xFC},  // 79
  {0x04,0x04,0x04,0x04,0x04,0x04,0x04,0xFC},  // 7A
  {0x00,0x00,0x00,0x00,0xE0,0xE0,0xE0,0xE0},  // 7B
  {0x1C,0x1C,0x1C,0x1C,0x00,0x00,0x00,0x00},  // 7C
  {0x10,0x10,0x10,0x10,0xF0,0x00,0x00,0x00},  // 7D
  {0xE0,0xE0,0xE0,0xE0,0x00,0x00,0x00,0x00},  // 7E
  {0xE0,0xE0,0xE0,0xE0,0x1C,0x1C,0x1C,0x1C},  // 7F
};

class CardputerModemSerial : public Stream {
public:
  static const int COLS = 40;     // 240 px / 6 px (font 0, size 1)
  static const int ROWS = 16;     // 16 * 8 = 128 px
  static const int MAXR = 25;     // rows of the virtual screen
  static const int MAXC = 80;     // width of the virtual screen
  static const int BAR_Y = 128;   // status bar: 7 px (128..134) of 135
  static const int BAR_H = 7;
  static const int KB_BUF = 64;

  explicit CardputerModemSerial(HardwareSerial &uart) : _uart(uart) {}

  // ---- Initialization (call AFTER M5Cardputer.begin()) ----
  // Can be called multiple times (Zimodem does so when baud/config changes).
  void begin(unsigned long baud, uint32_t config, int rxPin, int txPin) {
    initDisplay();
    _baud = baud; _config = config;
    if (_pinsSaved) { _rx = _cfgRx; _tx = _cfgTx; }          // pins chosen in the menu
    else            { _rx = rxPin; _tx = txPin; _cfgRx = rxPin; _cfgTx = txPin; }
    _uart.setRxBufferSize(_rxBufSize);
    _uart.begin(baud, config, _rx, _tx);
    holdRxIdle();
    _started = true;
  }

  void begin(unsigned long baud, uint32_t config = SERIAL_8N1) {
    begin(baud, config, CARDPUTER_MODEM_RX, CARDPUTER_MODEM_TX);
  }

  // Zimodem calls this AFTER begin(): on ESP32 the RX buffer can only be changed
  // while the UART is stopped, so we restart it with the stored parameters.
  size_t setRxBufferSize(size_t n) {
    if (n == _rxBufSize) return n;
    _rxBufSize = n;
    if (_started) {
      _uart.end();
      _uart.setRxBufferSize(_rxBufSize);
      _uart.begin(_baud, _config, _rx, _tx);
      holdRxIdle();
    }
    return n;
  }

  void updateBaudRate(unsigned long baud) { _baud = baud; _uart.updateBaudRate(baud); }
  uint32_t baudRate()                     { return _uart.baudRate(); }
  int availableForWrite()                 { return _uart.availableForWrite(); }
  void end()                              { _uart.end(); _started = false; }

  // Number of open connections (shown in the status bar).
  void setCalls(int n) { _calls = n; }

  // Zimodem calls this function from its loop(): if a baud change is needed
  // (chosen in the menu, or saved) it returns the new value, otherwise 0.
  unsigned long takeBaudRequest() {
    unsigned long r;
    portENTER_CRITICAL(&_kbMux);
    r = _reqBaud; _reqBaud = 0;
    portEXIT_CRITICAL(&_kbMux);
    return r;
  }

  // Kept for compatibility: keyboard and display run in their own task.
  void pump() {}

  // ---- Stream ----
  int available() override { return (int)_kbCount + uartAvail(); }

  int read() override {
    int c = kbPop();
    if (c >= 0) return c;
    if (!_uartIn) { uartAvail(); return -1; }
    return _uart.read();
  }

  int peek() override {
    int c = kbPeek();
    if (c >= 0) return c;
    return _uartIn ? _uart.peek() : -1;
  }

  void flush() override { _uart.flush(); }

  size_t write(uint8_t c) override {
    if (_inited) {
      xSemaphoreTake(_termMux, portMAX_DELAY);
      termPut(c);
      xSemaphoreGive(_termMux);
    }
    return _uart.write(c);
  }

  size_t write(const uint8_t *buf, size_t n) override {
    if (_inited) {
      xSemaphoreTake(_termMux, portMAX_DELAY);
      for (size_t i = 0; i < n; i++) termPut(buf[i]);
      xSemaphoreGive(_termMux);
    }
    return _uart.write(buf, n);
  }
  using Print::write;

private:
  // ---------------------------------------------------------------- types
  struct Cell { uint8_t c; uint8_t a; };   // c = CP437 glyph index   // a = (background << 4) | foreground

  // HID codes used by the Fn layer of the M5Cardputer library
  enum : uint8_t {
    HID_F2KEY = 0x3B, HID_KEY2 = 0x1F,
    HID_ENTER = 0x28, HID_ESC = 0x29, HID_BS = 0x2A, HID_DEL = 0x4C,
    HID_F3KEY = 0x3C, HID_KEY3 = 0x20,   // Fn+2 opens the phonebook, Fn+3 changes encoding
    HID_F1KEY = 0x3A, HID_KEY1 = 0x1E,   // Fn+1 opens the settings menu
    HID_RIGHT = 0x4F, HID_LEFT = 0x50, HID_DOWN = 0x51, HID_UP = 0x52
  };

  // -------------------------------------------------------------- members
  HardwareSerial &_uart;
  lgfx::LGFX_Sprite _canvas;     // created in initDisplay(), not in the constructor
  bool _inited = false;
  bool _spriteOk = false;
  bool _started = false;
  size_t _rxBufSize = 256;
  unsigned long _baud = 0;
  uint32_t _config = SERIAL_8N1;
  int _rx = CARDPUTER_MODEM_RX, _tx = CARDPUTER_MODEM_TX;
  volatile int _calls = 0;
  uint16_t _pal[16];

  // keyboard -> ring buffer (producer: display task, consumer: Zimodem loop)
  uint8_t _kb[KB_BUF];
  volatile uint8_t _kbHead = 0, _kbTail = 0, _kbCount = 0;
  portMUX_TYPE _kbMux = portMUX_INITIALIZER_UNLOCKED;

  // keyboard state (display task only)
  uint8_t _lastCnt = 0;
  std::vector<char> _prevWord;
  std::vector<uint8_t> _prevHid;
  bool _prevEnter = false, _prevTab = false, _prevDel = false;
  uint8_t _repKey = 0;           // scroll key being repeated (0 = none)
  uint32_t _repAt = 0;
  uint8_t _dbgKey = 0;           // last unrecognized Fn key (shown in bar)
  uint32_t _dbgUntil = 0;

  // terminal (protected by _termMux)
  SemaphoreHandle_t _termMux = nullptr;
  Cell _scr[MAXR][MAXC];
  Cell _snap[ROWS][COLS];        // visible window only (COLS columns)
  Cell *_hist = nullptr;         // ring of rows scrolled off the screen
  int _histCap = 0, _histHead = 0, _histCount = 0;
  int _rows = ROWS;              // logical rows: 16, or 25 (virtual screen)
  bool _tall = true;
  int _panY = 0;                 // first screen row shown (window of ROWS rows)
  int _lastCurRow = -1;
  int _viewOff = 0;              // 0 = live, >0 = rows back in scrollback
  int _row = 0, _col = 0;        // _col == COLS means "wrap pending"
  int _saveRow = 0, _saveCol = 0;
  bool _curVisible = true;
  uint8_t _fg = 7, _bg = 0;
  bool _bold = false, _rev = false;
  uint8_t _attr = 7;             // current attribute, already computed
  int _cols = COLS;              // logical width: 40, or 80 (virtual screen)
  bool _wide = false;
  int _panX = 0;                 // first visible column (0 or MAXC-COLS)
  uint32_t _followUntil = 0;     // after a typed key the cursor is followed
  uint8_t _pst = 0;              // parser: 0 normal, 1 ESC, 2 CSI, 3 OSC, 4 ESC+1 byte
  int _p[8];
  uint8_t _np = 0;
  bool _priv = false;
  bool _pet = false;             // PETSCII (exclusive with _utf8)
  bool _petLower = false;        // PETSCII: lowercase/uppercase set (0x0E / 0x8E)
  uint16_t _palPet[16];
  bool _utf8 = CARDPUTER_TERM_UTF8_DEFAULT;   // encoding of bytes >= 0x80
  uint32_t _u8cp = 0;            // UTF-8: code point being built
  uint8_t _u8need = 0;           // UTF-8: continuation bytes expected

  // settings (menu), saved in NVS
  Preferences _prefs;
  uint8_t _fgIdx = 0;
  int _cfgRx = CARDPUTER_MODEM_RX, _cfgTx = CARDPUTER_MODEM_TX;
  bool _pinsSaved = false;
  bool _uartIn = true;             // false = ignore bytes arriving from the UART
  unsigned long _cfgBaud = 0;      // baud chosen in the menu (0 = not set)
  bool _baudTouched = false;
  unsigned long _baudAtOpen = 0, _atwBaud = 0;
  bool _atwPending = false;        // after baud change: send AT&W to Zimodem
  uint32_t _atwAt = 0;
  char _note[16] = "";             // short message in the status bar
  uint32_t _noteUntil = 0;
  volatile unsigned long _reqBaud = 0;   // baud change request for the Zimodem loop
  bool _menu = false;
  uint8_t _mSel = 0;
  enum { M_N = 9 };

  // phonebook: 10 BBSes (name + host[:port]), saved in NVS
  enum { B_N = 10, B_NAME = 16, B_ADDR = 38 };
  bool _book = false, _bEdit = false;
  uint8_t _bSel = 0, _bField = 0;
  char _bName[B_N][B_NAME + 1] = {{0}};
  char _bAddr[B_N][B_ADDR + 1] = {{0}};
  char _eName[B_NAME + 1] = "", _eAddr[B_ADDR + 1] = "";
  char _bMsg[28] = "";

  // status bar / drawing state (display task only)
  volatile bool _dirty = false;
  bool _blink = false;
  bool _wifiOk = false;
  char _ip[16] = "";
  uint32_t _lastStatus = 0;

  // ------------------------------------------------------- settings
  enum { NCOL = 7 };
  static uint16_t colorOf(int i) {
    static const uint16_t t[NCOL] = { CARDPUTER_TERM_DEFAULT_FG, 0x07E0, 0xFD40, 0xFFFF, 0xAD55, 0x07FF, 0xFFE0 };
    return t[(i >= 0 && i < NCOL) ? i : 0];
  }
  static const char *colorName(int i) {
    static const char *n[NCOL] = { "Default", "Green", "Amber", "White", "Gray", "Cyan", "Yellow" };
    return n[(i >= 0 && i < NCOL) ? i : 0];
  }
  // Free GPIOs on Cardputer ADV (excluding display, SD, keyboard/I2C, audio, IR, USB, flash, G0)
  static const uint8_t *pinList(int &n) {
    static const uint8_t p[] = { 1, 2, 3, 4, 5, 6, 7, 10, 13, 15, 16, 17, 18, 21, 47, 48 };
    n = (int)sizeof(p);
    return p;
  }
  static bool pinOk(int v) {
    int n; const uint8_t *p = pinList(n);
    for (int i = 0; i < n; i++) if (p[i] == v) return true;
    return false;
  }
  static int nextPin(int cur, int d, int other) {
    int n; const uint8_t *p = pinList(n);
    int i = 0;
    for (; i < n; i++) if (p[i] == cur) break;
    if (i == n) i = 0;
    for (int k = 0; k < n; k++) {
      i = (i + d + n) % n;
      if (p[i] != other) return p[i];
    }
    return cur;
  }

  static const unsigned long *baudList(int &n) {
    static const unsigned long b[] = { 300, 1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600, 115200, 230400 };
    n = (int)(sizeof(b) / sizeof(b[0]));
    return b;
  }
  static bool baudOk(unsigned long v) {
    int n; const unsigned long *b = baudList(n);
    for (int i = 0; i < n; i++) if (b[i] == v) return true;
    return false;
  }
  static unsigned long nextBaud(unsigned long cur, int d) {
    int n; const unsigned long *b = baudList(n);
    int i = 0, best = 0; unsigned long bd = 0xFFFFFFFF;
    for (; i < n; i++) {                       // entry closest to the current value
      unsigned long df = b[i] > cur ? b[i] - cur : cur - b[i];
      if (df < bd) { bd = df; best = i; }
    }
    if (b[best] == cur) best = (best + d + n) % n;       // exact value: move to the neighbor
    else if (d > 0 && b[best] < cur) best = (best + 1) % n;   // between two entries: the next / previous
    else if (d < 0 && b[best] > cur) best = (best + n - 1) % n;
    return b[best];
  }

  void loadSettings() {
    if (!_prefs.begin("cpmodem", false)) return;
    bool w = _prefs.getBool("w", false);
    uint8_t c = _prefs.getUChar("c", 0);
    uint8_t rx = _prefs.getUChar("rx", 255), tx = _prefs.getUChar("tx", 255);
    _tall = _prefs.getBool("t", true);
    _uartIn = _prefs.getBool("ri", true);
    uint8_t e = _prefs.getUChar("e", _utf8 ? 1 : 0);
    _prefs.end();
    _wide = w;
    _rows = _tall ? MAXR : ROWS;
    setEncRaw(e <= 2 ? e : 0);                 // also applies the columns
    _fgIdx = (c < NCOL) ? c : 0;
    _pal[7] = colorOf(_fgIdx);
    if (pinOk(rx) && pinOk(tx) && rx != tx) { _cfgRx = rx; _cfgTx = tx; _pinsSaved = true; }
    loadBook();
  }

  static void cleanTxt(char *t) {                  // printable ASCII only
    for (char *c = t; *c; c++) if ((uint8_t)*c < 32 || (uint8_t)*c > 126) *c = ' ';
  }
  void loadBook() {
    if (!_prefs.begin("cpmodem", false)) return;
    for (int i = 0; i < B_N; i++) {
      char k[6];
      snprintf(k, sizeof(k), "n%d", i); _bName[i][0] = 0; _prefs.getString(k, _bName[i], sizeof(_bName[i])); cleanTxt(_bName[i]);
      snprintf(k, sizeof(k), "a%d", i); _bAddr[i][0] = 0; _prefs.getString(k, _bAddr[i], sizeof(_bAddr[i])); cleanTxt(_bAddr[i]);
    }
    _prefs.end();
  }
  void saveBookSlot(int i) {
    if (!_prefs.begin("cpmodem", false)) return;
    char k[6];
    snprintf(k, sizeof(k), "n%d", i); _prefs.putString(k, _bName[i]);
    snprintf(k, sizeof(k), "a%d", i); _prefs.putString(k, _bAddr[i]);
    _prefs.end();
  }

  void saveSettings() {
    if (!_prefs.begin("cpmodem", false)) return;
    _prefs.putBool("w", _wide);
    _prefs.putBool("t", _tall);
    _prefs.putBool("ri", _uartIn);
    _prefs.putUChar("c", _fgIdx);
    _prefs.putUChar("rx", (uint8_t)_cfgRx);
    _prefs.putUChar("tx", (uint8_t)_cfgTx);
    _prefs.putUChar("e", (uint8_t)encoding());
    _prefs.end();
    _pinsSaved = true;
  }

  // UART input disabled: incoming bytes are discarded.
  int uartAvail() {
    if (_uartIn) return _uart.available();
    while (_uart.available() > 0) _uart.read();
    return 0;
  }

  // The RX line at rest must stay high (UART idle): with the pin unconnected
  // the internal pull-up keeps noise and crosstalk from the nearby TX pin
  // from being read as characters.
  void holdRxIdle() {
#ifdef ARDUINO_ARCH_ESP32
    if (_rx >= 0) gpio_set_pull_mode((gpio_num_t)_rx, GPIO_PULLUP_ONLY);
#endif
  }

  // Restarts the UART if the pins changed.
  void applyPins() {
    if (_cfgRx == _rx && _cfgTx == _tx) return;
    _rx = _cfgRx; _tx = _cfgTx;
    if (_started) {
      _uart.end();
      _uart.setRxBufferSize(_rxBufSize);
      _uart.begin(_baud, _config, _rx, _tx);
      holdRxIdle();
    }
  }

  bool wide80() const { return _wide && !_pet; }     // PETSCII is always 40 columns

  void applyCols() {
    _cols = wide80() ? MAXC : COLS;
    if (_cols == COLS) _panX = 0;
    if (_col > _cols) _col = _cols;
    if (_saveCol > _cols) _saveCol = _cols;
  }

  // Logical rows 16 / 25. Switching to 16 scrolls the excess rows into scrollback.
  void applyRows() {
    int want = _tall ? MAXR : ROWS;
    if (want < _rows) {
      int extra = _rows - want;
      for (int i = 0; i < extra; i++) {
        pushHistory(_scr[0]);
        memmove(_scr[0], _scr[1], sizeof(Cell) * MAXC * (_rows - 1));
        Cell b = blank();
        for (int c = 0; c < MAXC; c++) _scr[_rows - 1][c] = b;
        if (_row > 0) _row--;
        if (_saveRow > 0) _saveRow--;
      }
    }
    _rows = want;
    if (_row >= _rows) _row = _rows - 1;
    if (_saveRow >= _rows) _saveRow = _rows - 1;
    if (_panY > _rows - ROWS) _panY = _rows - ROWS;
    if (_panY < 0) _panY = 0;
    _lastCurRow = -1;
  }

  void setTall(bool t) {
    xSemaphoreTake(_termMux, portMAX_DELAY);
    _tall = t; applyRows(); _dirty = true;
    xSemaphoreGive(_termMux);
  }

  // Sets the encoding: 0 CP437, 1 UTF-8, 2 PETSCII (call with _termMux held).
  void setEncRaw(int e) {
    bool wasPet = _pet;
    _utf8 = (e == 1); _pet = (e == 2);
    _u8need = 0; _pst = 0;
    if (_pet != wasPet) {                      // starting colors/attributes of the new mode
      _rev = false; _bold = false; _bg = 0; _petLower = false;
      _fg = _pet ? 14 : 7;                     // C64: light blue; ANSI: default color
      updateAttr();
    }
    applyCols();
    _dirty = true;
  }
  int encoding() const { return _pet ? 2 : (_utf8 ? 1 : 0); }

  void cycleEncoding(int d) {
    xSemaphoreTake(_termMux, portMAX_DELAY);
    setEncRaw((encoding() + d + 3) % 3);
    xSemaphoreGive(_termMux);
  }

  void setWide(bool w) {
    xSemaphoreTake(_termMux, portMAX_DELAY);
    _wide = w; applyCols();
    if (!w) {                                  // no leftovers in the hidden half
      Cell b = blank();
      for (int r = 0; r < MAXR; r++) for (int c = COLS; c < MAXC; c++) _scr[r][c] = b;
    }
    _dirty = true;
    xSemaphoreGive(_termMux);
  }

  // Moves the visible window by d columns (continuous scrolling).
  void panBy(int d) {
    if (!wide80()) return;
    xSemaphoreTake(_termMux, portMAX_DELAY);
    int v = _panX + d;
    if (v < 0) v = 0;
    if (v > MAXC - COLS) v = MAXC - COLS;
    if (v != _panX) { _panX = v; _dirty = true; }
    _followUntil = 0;                          // manual choice takes precedence
    xSemaphoreGive(_termMux);
  }

  void openMenu()  {
    _menu = true; _mSel = 0; _repKey = 0; _dirty = true;
    _cfgBaud = _baud; _baudAtOpen = _baud; _baudTouched = false;   // show the current baud
  }
  void closeMenu() {
    _menu = false; applyPins();
    if (_baudTouched && _cfgBaud && _cfgBaud != _baud) {
      portENTER_CRITICAL(&_kbMux); _reqBaud = _cfgBaud; portEXIT_CRITICAL(&_kbMux);
      _atwBaud = _cfgBaud; _atwAt = millis();
      if (_calls > 0) {                          // in a call AT&W would reach the BBS: do not send it
        snprintf(_note, sizeof(_note), "USE AT&W"); _noteUntil = millis() + 5000;
      } else {
        _atwPending = true;
      }
    }
    saveSettings(); _dirty = true;
  }

  // act: 0 up, 1 down, 2 left, 3 right, 4 enter, 5 close
  void menuKey(int act) {
    switch (act) {
      case 0: _mSel = (_mSel + M_N - 1) % M_N; break;
      case 1: _mSel = (_mSel + 1) % M_N; break;
      case 5: closeMenu(); return;
      default: {
        int d = (act == 2) ? -1 : +1;
        switch (_mSel) {
          case 0: setWide(!_wide); break;
          case 1: setTall(!_tall); break;
          case 2: _fgIdx = (uint8_t)((_fgIdx + d + NCOL) % NCOL); _pal[7] = colorOf(_fgIdx); break;
          case 3: _cfgRx = nextPin(_cfgRx, d, _cfgTx); break;
          case 4: _cfgTx = nextPin(_cfgTx, d, _cfgRx); break;
          case 5: _cfgBaud = nextBaud(_cfgBaud ? _cfgBaud : _baud, d); _baudTouched = true; break;
          case 6: _uartIn = !_uartIn; break;
          case 7: cycleEncoding(d); break;
          case 8: if (act == 4) { closeMenu(); return; } break;
        }
      }
    }
    _dirty = true;
  }

  void onPressMenu(Keyboard_Class::KeysState &st) {
    std::vector<uint8_t> nh;
    newItems(_prevHid, st.hid_keys, nh, false);
    bool acted = false;
    for (uint8_t k : nh) {
      switch (k) {
        case HID_UP:    menuKey(0); acted = true; break;
        case HID_DOWN:  menuKey(1); acted = true; break;
        case HID_LEFT:  menuKey(2); acted = true; break;
        case HID_RIGHT: menuKey(3); acted = true; break;
        case HID_ESC: case HID_F1KEY: case HID_KEY1: menuKey(5); return;
        default: break;
      }
    }
    if (!acted) {                              // old libraries / keys ; . , / without Fn
      std::vector<char> nw;
      newItems(_prevWord, st.word, nw, true);
      for (char c : nw) {
        if (c == ';') menuKey(0); else if (c == '.') menuKey(1);
        else if (c == ',') menuKey(2); else if (c == '/') menuKey(3);
        else if (c == '`' || (st.fn && (c == '1' || c == '!'))) { menuKey(5); return; }
      }
    }
    if (st.enter && !_prevEnter) menuKey(4);
    if (st.del && !_prevDel && !st.fn) menuKey(5);
  }

  // ----------------------------------------------------------- phonebook
  void openBook() {
    _book = true; _bEdit = false; _bSel = 0; _bMsg[0] = 0; _repKey = 0; _dirty = true;
  }
  void closeBook() { _book = false; _bEdit = false; _dirty = true; }

  void bookEditStart() {
    strcpy(_eName, _bName[_bSel]); strcpy(_eAddr, _bAddr[_bSel]);
    _bEdit = true; _bField = 0; _bMsg[0] = 0;
  }
  void bookEditSave() {
    // strip leading/trailing spaces from the address
    char *a = _eAddr; while (*a == ' ') a++;
    size_t n = strlen(a); while (n && a[n - 1] == ' ') a[--n] = 0;
    strcpy(_bName[_bSel], _eName); strcpy(_bAddr[_bSel], a);
    saveBookSlot(_bSel);
    _bEdit = false; snprintf(_bMsg, sizeof(_bMsg), "Slot %d saved", _bSel + 1);
  }
  void bookDial() {
    const char *a = _bAddr[_bSel];
    if (!a[0]) { snprintf(_bMsg, sizeof(_bMsg), "Slot empty"); return; }
    if (_calls > 0) { snprintf(_bMsg, sizeof(_bMsg), "In call: ATH first"); return; }
    if (_kbCount > 8) { snprintf(_bMsg, sizeof(_bMsg), "Keyboard busy"); return; }
    closeBook(); goLive();
    for (const char *c = "ATDT "; *c; c++) kbPush((uint8_t)*c);   // always ASCII (outside a call)
    for (const char *c = a; *c; c++) kbPush((uint8_t)*c);
    kbPush('\r');
  }

  // act: 0 up, 1 down, 2 enter, 3 tab, 4 exit/cancel, 5 backspace
  void bookKey(int act) {
    _bMsg[0] = 0;
    if (_bEdit) {
      switch (act) {
        case 0: case 1: case 3: _bField ^= 1; break;
        case 2: bookEditSave(); break;
        case 4: _bEdit = false; break;
        case 5: { char *t = _bField ? _eAddr : _eName; size_t n = strlen(t); if (n) t[n - 1] = 0; } break;
      }
    } else {
      switch (act) {
        case 0: _bSel = (_bSel + B_N - 1) % B_N; break;
        case 1: _bSel = (_bSel + 1) % B_N; break;
        case 2: bookDial(); break;
        case 3: bookEditStart(); break;
        case 4: closeBook(); return;
      }
    }
    _dirty = true;
  }
  void bookChar(char c) {
    if (c < 32 || c > 126) return;
    char *t = _bField ? _eAddr : _eName; size_t mx = _bField ? B_ADDR : B_NAME;
    size_t n = strlen(t); if (n >= mx) return;
    t[n] = c; t[n + 1] = 0; _bMsg[0] = 0; _dirty = true;
  }

  void onPressBook(Keyboard_Class::KeysState &st) {
    std::vector<uint8_t> nh;
    newItems(_prevHid, st.hid_keys, nh, false);
    std::vector<char> nw;
    newItems(_prevWord, st.word, nw, true);
    for (uint8_t k : nh) {
      switch (k) {
        case HID_UP:   bookKey(0); break;
        case HID_DOWN: bookKey(1); break;
        case HID_ESC: case HID_F2KEY: case HID_KEY2: bookKey(4); if (!_book) return; break;
        default: break;
      }
    }
    for (char c : nw) {
      if (st.fn) {                              // old libraries: with Fn the characters stay in word
        if (c == ';') bookKey(0); else if (c == '.') bookKey(1);
        else if (c == '`' || c == '2' || c == '@') { bookKey(4); if (!_book) return; }
      } else if (_bEdit) {
        bookChar(c);
      } else {
        if (c == ';') bookKey(0); else if (c == '.') bookKey(1);
        else if (c == '`') { bookKey(4); return; }
      }
    }
    if (st.enter && !_prevEnter) bookKey(2);
    if (st.tab && !_prevTab) bookKey(3);
    if (!st.fn && ((st.del && !_prevDel) || hidHas(nh, HID_BS))) {
      if (_bEdit) bookKey(5); else bookKey(4);
    }
  }

  // ------------------------------------------------------------- init
  void initPalette() {
    auto rgb = [](int r, int g, int b) -> uint16_t {
      return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    };
    static const uint8_t c[16][3] = {
      {0, 0, 0},     {170, 0, 0},   {0, 170, 0},   {170, 85, 0},
      {0, 0, 170},   {170, 0, 170}, {0, 170, 170}, {170, 170, 170},
      {85, 85, 85},  {255, 85, 85}, {85, 255, 85}, {255, 255, 85},
      {85, 85, 255}, {255, 85, 255},{85, 255, 255},{255, 255, 255}};
    for (int i = 0; i < 16; i++) _pal[i] = rgb(c[i][0], c[i][1], c[i][2]);
    _pal[7] = CARDPUTER_TERM_DEFAULT_FG;     // 7 = default color
    // C64 palette (index = C64 color number)
    static const uint8_t k[16][3] = {
      {0, 0, 0},       {255, 255, 255}, {129, 51, 56},   {117, 206, 200},
      {142, 60, 151},  {86, 172, 77},   {70, 68, 190},   {237, 241, 113},
      {142, 80, 41},   {85, 56, 0},     {196, 108, 113}, {99, 99, 99},
      {123, 123, 123}, {169, 255, 159}, {112, 109, 235}, {178, 178, 178}};
    for (int i = 0; i < 16; i++) _palPet[i] = rgb(k[i][0], k[i][1], k[i][2]);
  }

  void initDisplay() {
    if (_inited) return;
    initPalette();
    loadSettings();
    _termMux = xSemaphoreCreateMutex();
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(128);
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    _canvas.setColorDepth(16);
    _spriteOk = (_canvas.createSprite(240, 135) != nullptr);
    if (!_spriteOk) {                        // low contiguous RAM: fall back to 8 bit
      _canvas.setColorDepth(8);
      _spriteOk = (_canvas.createSprite(240, 135) != nullptr);
    }
    _canvas.setTextSize(1);
    _canvas.setTextWrap(false);
    _canvas.cp437(true);                     // glyphs 0x80..0xFF = CP437

    _histCap = CARDPUTER_SCROLLBACK_LINES;
    _hist = (Cell *)malloc(sizeof(Cell) * MAXC * _histCap);
    if (!_hist) _histCap = 0;

    if (_hist) { Cell b = blank(); for (size_t i = 0; i < (size_t)MAXC * _histCap; i++) _hist[i] = b; }
    updateAttr();
    clearAll();
    _row = _col = 0;
    _dirty = true;
    _inited = true;
    // Task on core 0 (Arduino's loop() runs on core 1)
    xTaskCreatePinnedToCore(taskEntry, "cpTerm", 8192, this, 1, nullptr, 0);
  }

  static void taskEntry(void *p) {
    static_cast<CardputerModemSerial *>(p)->taskLoop();
  }

  void taskLoop() {
    uint32_t lastDraw = 0;
    for (;;) {
      M5Cardputer.update();
      pollKeyboard();
      checkAtw();
      uint32_t now = millis();
      bool blink = ((now / 500) & 1);
      if (blink != _blink) { _blink = blink; _dirty = true; }
      if (now - _lastStatus >= 500) {        // WiFi status: every half second
        _lastStatus = now;
        pollStatus();
      }
      if (_dirty && now - lastDraw >= 33) {  // ~30 fps max
        _dirty = false;                      // first: new writes set it back to true
        redraw();
        lastDraw = millis();
      }
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

  // After "Save and close" with a new baud: once Zimodem has applied the baud
  // and there is no call, writes AT&W as if typed, so the baud
  // stays in Zimodem's configuration.
  void checkAtw() {
    if (!_atwPending) return;
    uint32_t now = millis();
    if (now - _atwAt > 20000) { _atwPending = false; return; }      // Zimodem did not respond
    if (_calls > 0) {
      _atwPending = false;
      snprintf(_note, sizeof(_note), "USE AT&W"); _noteUntil = now + 5000; _dirty = true;
      return;
    }
    if (_reqBaud != 0 || _baud != _atwBaud || _kbCount != 0) return;
    static const char cmd[] = "AT&W\r";
    for (const char *c = cmd; *c; c++) kbPush((uint8_t)*c);
    _atwPending = false;
    snprintf(_note, sizeof(_note), "BAUD SAVED"); _noteUntil = now + 3000; _dirty = true;
  }

  void pollStatus() {
    bool ok = (WiFi.status() == WL_CONNECTED);
    char ip[16] = "";
    if (ok) {
      String s = WiFi.localIP().toString();
      strncpy(ip, s.c_str(), sizeof(ip) - 1);
      ip[sizeof(ip) - 1] = 0;
    }
    if (ok != _wifiOk || strcmp(ip, _ip) != 0) {
      _wifiOk = ok;
      strcpy(_ip, ip);
      _dirty = true;
    }
    static int lastCalls = -1;
    static unsigned long lastBaud = 0;
    if (lastCalls != _calls || lastBaud != _baud) {
      lastCalls = _calls; lastBaud = _baud;
      _dirty = true;
    }
  }

  // ------------------------------------------------- keyboard ring buffer
  void kbPush(uint8_t c) {
    portENTER_CRITICAL(&_kbMux);
    if (_kbCount < KB_BUF) {
      _kb[_kbHead] = c;
      _kbHead = (_kbHead + 1) % KB_BUF;
      _kbCount = _kbCount + 1;
    }
    portEXIT_CRITICAL(&_kbMux);
  }

  int kbPop() {
    int c = -1;
    portENTER_CRITICAL(&_kbMux);
    if (_kbCount) {
      c = _kb[_kbTail];
      _kbTail = (_kbTail + 1) % KB_BUF;
      _kbCount = _kbCount - 1;
    }
    portEXIT_CRITICAL(&_kbMux);
    return c;
  }

  int kbPeek() {
    int c = -1;
    portENTER_CRITICAL(&_kbMux);
    if (_kbCount) c = _kb[_kbTail];
    portEXIT_CRITICAL(&_kbMux);
    return c;
  }

  // In PETSCII, during a call, the keyboard speaks PETSCII (like a C64):
  // lowercase -> 0x41.., uppercase (Shift) -> 0xC1.., Backspace -> 0x14, arrows -> 0x91/0x11/0x9D/0x1D.
  // Outside calls it stays ASCII, so AT commands and passwords work.
  bool petKeys() const { return _pet && _calls > 0; }

  static uint8_t petKeyOut(uint8_t c) {
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 0x20);
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c + 0x80);
    return c;
  }

  void kbPushArrow(char dir) {             // ESC [ A/B/C/D
    if (petKeys()) {
      kbPush(dir == 'A' ? 0x91 : dir == 'B' ? 0x11 : dir == 'C' ? 0x1D : 0x9D);
      return;
    }
    kbPush(0x1B); kbPush('['); kbPush((uint8_t)dir);
  }

  // ------------------------------------------------------------ keyboard
  // Elements present in 'cur' and not in 'prev' (compared as a multiset).
  template <typename T>
  static void newItems(const std::vector<T> &prev, const std::vector<T> &cur,
                       std::vector<T> &out, bool ignoreCase) {
    std::vector<T> rem(prev);
    for (T c : cur) {
      bool found = false;
      for (size_t i = 0; i < rem.size(); i++) {
        bool eq = ignoreCase ? (tolower((unsigned char)rem[i]) == tolower((unsigned char)c))
                             : (rem[i] == c);
        if (eq) { rem.erase(rem.begin() + i); found = true; break; }
      }
      if (!found) out.push_back(c);
    }
  }

  static bool hidHas(const std::vector<uint8_t> &v, uint8_t k) {
    for (uint8_t x : v) if (x == k) return true;
    return false;
  }

  void goLive() {
    xSemaphoreTake(_termMux, portMAX_DELAY);
    if (_viewOff != 0) { _viewOff = 0; _dirty = true; }
    _lastCurRow = -1;                          // bring the window back to the cursor
    xSemaphoreGive(_termMux);
  }

  // Scrolls the window: first over the virtual screen rows, then into the scrollback.
  void scrollBy(int lines) {
    xSemaphoreTake(_termMux, portMAX_DELAY);
    int n = lines < 0 ? -lines : lines;
    for (int i = 0; i < n; i++) {
      if (lines > 0) {                                   // upward
        if (_viewOff == 0 && _panY > 0) _panY--;
        else if (_viewOff < _histCount) _viewOff++;
        else break;
      } else {                                           // downward
        if (_viewOff > 0) _viewOff--;
        else if (_panY < _rows - ROWS) _panY++;
        else break;
      }
      _dirty = true;
    }
    xSemaphoreGive(_termMux);
  }

  // Scrollback action bound to an arrow key (Fn layer).
  void scrollKey(uint8_t hid) {
    switch (hid) {
      case HID_UP:    scrollBy(+1);    break;
      case HID_DOWN:  scrollBy(-1);    break;
      case HID_LEFT:  if (wide80()) panBy(-1); else scrollBy(+ROWS); break;   // pan / page back
      case HID_RIGHT: if (wide80()) panBy(+1); else scrollBy(-ROWS); break;   // pan / page forward
    }
  }

  static char arrowLetter(uint8_t hid) {
    switch (hid) {
      case HID_UP: return 'A'; case HID_DOWN: return 'B';
      case HID_RIGHT: return 'C'; default: return 'D';
    }
  }

  void onPress(Keyboard_Class::KeysState &st, uint32_t now) {
    if (_menu) { onPressMenu(st); return; }
    if (_book) { onPressBook(st); return; }
    if (st.fn) {
      // --- Fn layer: the library (>= 1.1) puts arrows/ESC/Del in hid_keys ---
      bool tog = false, menuReq = false, bookReq = false;       // one action per keypress
      std::vector<uint8_t> nh;
      newItems(_prevHid, st.hid_keys, nh, false);
      for (uint8_t k : nh) {
        switch (k) {
          case HID_UP: case HID_DOWN: case HID_LEFT: case HID_RIGHT:
            if (st.shift) { goLive(); kbPushArrow(arrowLetter(k)); }      // arrow to the host
            else { scrollKey(k); _repKey = k; _repAt = now + 400; }       // scrollback
            break;
          case HID_ESC: goLive(); kbPush(0x1B); break;
          case HID_F1KEY: case HID_KEY1: menuReq = true; break;
          case HID_F2KEY: case HID_KEY2: bookReq = true; break;
          case HID_F3KEY: case HID_KEY3: tog = true; break;
          case HID_DEL: goLive(); if (petKeys()) kbPush(0x14); else { kbPush(0x1B); kbPush('['); kbPush('3'); kbPush('~'); } break;
          default: _dbgKey = k; _dbgUntil = now + 3000; _dirty = true; break;   // status bar diagnostics
        }
      }
      // --- older libraries: with Fn the characters stay in word ---
      std::vector<char> nw;
      newItems(_prevWord, st.word, nw, true);
      for (char c : nw) {
        switch (c) {
          case '`': goLive(); kbPush(0x1B); break;
          case '1': case '!': menuReq = true; break;
          case '2': case '@': bookReq = true; break;
          case '3': case '#': tog = true; break;
          case ';': if (st.shift) { goLive(); kbPushArrow('A'); } else { scrollKey(HID_UP);   _repKey = HID_UP;   _repAt = now + 400; } break;
          case '.': if (st.shift) { goLive(); kbPushArrow('B'); } else { scrollKey(HID_DOWN); _repKey = HID_DOWN; _repAt = now + 400; } break;
          case ',': if (st.shift) { goLive(); kbPushArrow('D'); } else { scrollKey(HID_LEFT); _repKey = HID_LEFT; _repAt = now + 400; } break;
          case '/': if (st.shift) { goLive(); kbPushArrow('C'); } else { scrollKey(HID_RIGHT);_repKey = HID_RIGHT;_repAt = now + 400; } break;
          default: break;
        }
      }
      if (menuReq) { openMenu(); return; }
      if (bookReq) { openBook(); return; }
      if (tog) toggleEncoding();
      return;
    }

    // --- normal keys ---
    bool any = false;
    std::vector<char> nw;
    newItems(_prevWord, st.word, nw, true);
    for (char c : nw) {
      if (st.ctrl && isalpha((unsigned char)c)) c = toupper((unsigned char)c) & 0x1F;
      kbPush(petKeys() ? petKeyOut((uint8_t)c) : (uint8_t)c);
      any = true;
    }
    if (st.enter && !_prevEnter) { kbPush('\r'); any = true; }
    if (st.tab && !_prevTab)     { kbPush('\t'); any = true; }
    // Backspace: old libraries -> st.del; new (>= 1.1) -> HID 0x2A in hid_keys
    std::vector<uint8_t> nh;
    newItems(_prevHid, st.hid_keys, nh, false);
    if ((st.del && !_prevDel) || hidHas(nh, HID_BS)) { kbPush(petKeys() ? 0x14 : 8); any = true; }
    if (any) goLive();
  }

  void pollKeyboard() {
    auto &kb = M5Cardputer.Keyboard;
    uint8_t cnt = kb.isPressed();
    uint32_t now = millis();
    if (cnt == 0) {
      _prevWord.clear(); _prevHid.clear();
      _prevEnter = _prevTab = _prevDel = false;
      _lastCnt = 0; _repKey = 0;
      return;
    }
    auto &st = kb.keysState();

    // auto-repeat of the scrollback while Fn+arrow is held down
    if (_repKey && !_menu) {
      bool held = st.fn && !st.shift && hidHas(st.hid_keys, _repKey);
      if (!held) {
        // old libraries: the key is a character in word
        char oldc = (_repKey == HID_UP) ? ';' : (_repKey == HID_DOWN) ? '.' :
                    (_repKey == HID_LEFT) ? ',' : '/';
        held = st.fn && !st.shift && std::find(st.word.begin(), st.word.end(), oldc) != st.word.end();
      }
      if (!held) _repKey = 0;
      else if (now >= _repAt) {
        scrollKey(_repKey);
        _repAt = now + ((_repKey == HID_UP || _repKey == HID_DOWN) ? 60 : (wide80() ? 35 : 180));
      }
    }

    if (cnt != _lastCnt) {
      if (cnt > _lastCnt) onPress(st, now);   // new keys pressed
      _prevWord = st.word;
      _prevHid = st.hid_keys;
      _prevEnter = st.enter; _prevTab = st.tab; _prevDel = st.del;
      _lastCnt = cnt;
    }
  }

  // ------------------------------------------------------------ terminal
  // (all the following functions must be called with _termMux held)
  void updateAttr() {
    uint8_t f = _fg, b = _bg;
    if (_bold && f < 8) f += 8;
    if (_rev) { uint8_t t = f; f = b; b = t; }
    _attr = (uint8_t)((b << 4) | f);
  }

  Cell blank() const { Cell c; c.c = ' '; c.a = (uint8_t)((_bg << 4) | 7); return c; }

  void clearAll() {
    Cell b = blank();
    for (int r = 0; r < MAXR; r++)
      for (int c = 0; c < MAXC; c++) _scr[r][c] = b;
  }

  void clearCells(int r, int c0, int c1) {          // [c0, c1)
    Cell b = blank();
    for (int c = c0; c < c1 && c < MAXC; c++) if (c >= 0) _scr[r][c] = b;
  }

  void pushHistory(const Cell *line) {
    if (!_histCap) return;
    memcpy(_hist + (size_t)_histHead * MAXC, line, sizeof(Cell) * MAXC);
    _histHead = (_histHead + 1) % _histCap;
    if (_histCount < _histCap) _histCount++;
    // if reading the scrollback, keep the page still
    if (_viewOff > 0) { _viewOff++; if (_viewOff > _histCount) _viewOff = _histCount; }
  }

  void scrollUp() {
    pushHistory(_scr[0]);
    memmove(_scr[0], _scr[1], sizeof(Cell) * MAXC * (_rows - 1));
    clearCells(_rows - 1, 0, MAXC);
  }

  void newline() {                 // like a tty with onlcr: LF = CR + LF
    _col = 0;
    if (++_row >= _rows) { _row = _rows - 1; scrollUp(); }
  }

  void putPrintable(uint8_t c) {
    if (_col >= _cols) newline();   // "lazy" wrap, like real terminals
    _scr[_row][_col].c = c;
    _scr[_row][_col].a = _attr;
    _col++;
  }

  // ----- encodings -----
  void toggleEncoding() { cycleEncoding(+1); }

  // Unicode code point -> CP437 glyph (0 = nothing to draw).
  static uint8_t unicodeToCp437(uint32_t u) {
    if (u >= 0xA0 && u <= 0xFF) return CPMS_LATIN1[u - 0xA0];
    if (u >= 0x2500 && u <= 0x259F) return CPMS_BOXMAP[u - 0x2500];
    if ((u >= 0x80 && u < 0xA0) || (u >= 0x200B && u <= 0x200F) ||
        (u >= 0x0300 && u <= 0x036F) || (u >= 0xFE00 && u <= 0xFE0F) || u == 0xFEFF) return 0;
    if (u < 0x10000) {
      for (size_t i = 0; i < sizeof(CPMS_MISC) / sizeof(CPMS_MISC[0]); i++)
        if (CPMS_MISC[i].u == u) return CPMS_MISC[i].c;
    }
    return '?';
  }

  void emitGlyph(uint8_t g) { if (g) putPrintable(g); }

  // Byte >= 0x80 (or ASCII in the middle of a sequence) in UTF-8 mode.
  // Returns true if the byte was consumed.
  bool utf8Byte(uint8_t c) {
    if (c >= 0xC2 && c <= 0xF4) {                  // leading byte
      if (_u8need) emitGlyph('?');                 // previous sequence interrupted
      _u8need = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : 1;
      _u8cp = c & (c >= 0xF0 ? 0x07 : c >= 0xE0 ? 0x0F : 0x1F);
      return true;
    }
    if (c >= 0x80 && c <= 0xBF) {                  // continuation
      if (!_u8need) { emitGlyph('?'); return true; }
      _u8cp = (_u8cp << 6) | (c & 0x3F);
      if (--_u8need == 0) emitGlyph(unicodeToCp437(_u8cp));
      return true;
    }
    if (c >= 0x80) { emitGlyph('?'); _u8need = 0; return true; }   // C0, C1, F5..FF
    if (_u8need) { emitGlyph('?'); _u8need = 0; }  // ASCII in the middle: then handled normally
    return false;
  }

  int clampCol(int c) const { return c < 0 ? 0 : (c >= _cols ? _cols - 1 : c); }
  int clampRow(int r) const { return r < 0 ? 0 : (r >= _rows ? _rows - 1 : r); }

  static uint8_t map256(int n) {   // 256 color -> one of the 16 base
    if (n < 16) return (uint8_t)n;
    if (n >= 232) { int g = n - 232; return g < 4 ? 0 : g < 12 ? 8 : g < 20 ? 7 : 15; }
    n -= 16;
    int r = n / 36, g = (n / 6) % 6, b = n % 6;
    int idx = (r >= 3 ? 1 : 0) | (g >= 3 ? 2 : 0) | (b >= 3 ? 4 : 0);
    bool bright = (r == 5 || g == 5 || b == 5);
    if (idx == 0) return (r | g | b) ? 8 : 0;
    return (uint8_t)(idx + (bright ? 8 : 0));
  }

  void sgr() {
    if (_np == 0) { _fg = 7; _bg = 0; _bold = false; _rev = false; updateAttr(); return; }
    for (int i = 0; i < _np; i++) {
      int v = _p[i];
      if (v == 0)               { _fg = 7; _bg = 0; _bold = false; _rev = false; }
      else if (v == 1)          _bold = true;
      else if (v == 22)         _bold = false;
      else if (v == 7)          _rev = true;
      else if (v == 27)         _rev = false;
      else if (v >= 30 && v <= 37)   _fg = v - 30;
      else if (v == 39)         _fg = 7;
      else if (v >= 40 && v <= 47)   _bg = v - 40;
      else if (v == 49)         _bg = 0;
      else if (v >= 90 && v <= 97)   _fg = v - 90 + 8;
      else if (v >= 100 && v <= 107) _bg = v - 100 + 8;
      else if (v == 38 || v == 48) {
        uint8_t *dst = (v == 38) ? &_fg : &_bg;
        if (i + 2 < _np && _p[i + 1] == 5) { *dst = map256(_p[i + 2]); i += 2; }
        else if (i + 4 < _np && _p[i + 1] == 2) { i += 4; }   // truecolor: ignored
      }
    }
    updateAttr();
  }

  int P(int i, int def) const { return (i < _np && _p[i] > 0) ? _p[i] : def; }

  void csi(char f) {
    if (_priv) {                               // ESC [ ? ... h/l
      if ((f == 'h' || f == 'l') && _np >= 1 && _p[0] == 25) _curVisible = (f == 'h');
      return;
    }
    switch (f) {
      case 'm': sgr(); break;
      case 'A': _row = clampRow(_row - P(0, 1)); break;
      case 'B': _row = clampRow(_row + P(0, 1)); break;
      case 'C': _col = clampCol((_col >= _cols ? _cols - 1 : _col) + P(0, 1)); break;
      case 'D': _col = clampCol((_col >= _cols ? _cols - 1 : _col) - P(0, 1)); break;
      case 'G': _col = clampCol(P(0, 1) - 1); break;
      case 'd': _row = clampRow(P(0, 1) - 1); break;
      case 'H': case 'f': _row = clampRow(P(0, 1) - 1); _col = clampCol(P(1, 1) - 1); break;
      case 'J': {
        int m = (_np > 0) ? _p[0] : 0;
        int c = clampCol(_col);
        if (m == 0)      { clearCells(_row, c, _cols); for (int r = _row + 1; r < _rows; r++) clearCells(r, 0, _cols); }
        else if (m == 1) { for (int r = 0; r < _row; r++) clearCells(r, 0, _cols); clearCells(_row, 0, c + 1); }
        else             { clearAll(); }
        break;
      }
      case 'K': {
        int m = (_np > 0) ? _p[0] : 0;
        int c = clampCol(_col);
        if (m == 0)      clearCells(_row, c, _cols);
        else if (m == 1) clearCells(_row, 0, c + 1);
        else             clearCells(_row, 0, _cols);
        break;
      }
      case 's': _saveRow = _row; _saveCol = _col; break;
      case 'u': _row = _saveRow; _col = _saveCol; break;
      default: break;                          // the rest is ignored
    }
  }

  // ----- PETSCII (display) -----
  // PETSCII byte -> glyph id in the cell (0 = no glyph).
  // Letters/digits/punctuation: ASCII. Graphics: 0x80 + (C64 screen code - 0x40).
  uint8_t petGlyph(uint8_t b) const {
    if (b < 0x20) return 0;
    if (b <= 0x3F) return b;
    if (b == 0x40) return '@';
    if (b <= 0x5A) return _petLower ? (uint8_t)(b + 0x20) : b;
    if (b == 0x5B) return '[';
    if (b == 0x5C) return 0x9C;                // pound sign (CP437 glyph)
    if (b == 0x5D) return ']';
    if (b == 0x5E) return 0x18;                // up arrow
    if (b == 0x5F) return 0x1B;                // left arrow
    int sc;
    if (b >= 0x60 && b <= 0x7F)      sc = b - 0x20;
    else if (b >= 0xA0 && b <= 0xBF) sc = b - 0x40;
    else if (b >= 0xC0 && b <= 0xFE) sc = b - 0x80;
    else if (b == 0xFF)              sc = 0x5E;           // pi
    else return 0;
    if (_petLower && sc >= 0x41 && sc <= 0x5A) return (uint8_t)sc;   // uppercase in the lowercase set
    if (sc == 0x60) return ' ';
    return (uint8_t)(0x80 + (sc - 0x40));
  }

  static int petColor(uint8_t c) {             // PETSCII color code -> C64 color number (-1 = none)
    switch (c) {
      case 0x90: return 0;  case 0x05: return 1;  case 0x1C: return 2;  case 0x9F: return 3;
      case 0x9C: return 4;  case 0x1E: return 5;  case 0x1F: return 6;  case 0x9E: return 7;
      case 0x81: return 8;  case 0x95: return 9;  case 0x96: return 10; case 0x97: return 11;
      case 0x98: return 12; case 0x99: return 13; case 0x9A: return 14; case 0x9B: return 15;
      default: return -1;
    }
  }

  void petPutGlyph(uint8_t g) {
    putPrintable(g);
    if (_col >= _cols) newline();              // the C64 wraps right after the last column
  }

  void petPut(uint8_t c) {
    int col = petColor(c);
    if (col >= 0) { _fg = (uint8_t)col; updateAttr(); return; }
    switch (c) {
      case 0x0D: case 0x8D: _rev = false; updateAttr(); newline(); break;   // RETURN cancels reverse
      case 0x0A: break;                                                     // LF ignored
      case 0x12: _rev = true;  updateAttr(); break;
      case 0x92: _rev = false; updateAttr(); break;
      case 0x0E: _petLower = true;  break;
      case 0x8E: _petLower = false; break;
      case 0x13: _row = 0; _col = 0; break;                                 // HOME
      case 0x93: clearAll(); _row = 0; _col = 0; break;                     // CLR
      case 0x11:                                                            // down
        if (_col >= _cols) _col = _cols - 1;
        if (_row + 1 >= _rows) scrollUp(); else _row++;
        break;
      case 0x91: if (_col >= _cols) _col = _cols - 1; if (_row > 0) _row--; break;   // up
      case 0x1D:                                                            // right
        if (_col < _cols - 1) _col++; else newline();
        break;
      case 0x9D:                                                            // left
        if (_col >= _cols) _col = _cols - 1;
        if (_col > 0) _col--; else if (_row > 0) { _row--; _col = _cols - 1; }
        break;
      case 0x14: {                                                          // DEL
        if (_col >= _cols) _col = _cols - 1;
        if (_col > 0) {
          _col--;
          for (int x = _col; x < _cols - 1; x++) _scr[_row][x] = _scr[_row][x + 1];
          clearCells(_row, _cols - 1, _cols);
        }
        break;
      }
      case 0x94: {                                                          // INS
        if (_col >= _cols) _col = _cols - 1;
        for (int x = _cols - 1; x > _col; x--) _scr[_row][x] = _scr[_row][x - 1];
        clearCells(_row, _col, _col + 1);
        break;
      }
      default: {
        uint8_t g = petGlyph(c);
        if (g) petPutGlyph(g);
        break;
      }
    }
  }

  void termPut(uint8_t c) {
    // PETSCII only during a call: in command mode Zimodem speaks ASCII
    // (key echo, AT replies), which would show up as symbols in PETSCII.
    if (_pet) {
      if (_calls > 0) { petPut(c); _dirty = true; return; }
      if (_rev || _fg != 14) { _rev = false; _fg = 14; updateAttr(); }   // clean restart
    }
    // --- escape sequence parser ---
    switch (_pst) {
      case 1:                                  // after ESC
        _pst = 0;
        if (c == '[') { _pst = 2; _np = 0; _priv = false; memset(_p, 0, sizeof(_p)); }
        else if (c == ']') _pst = 3;           // OSC: ignore until BEL / ESC
        else if (c == '(' || c == ')' || c == '#') _pst = 4;
        else if (c == '7') { _saveRow = _row; _saveCol = _col; }
        else if (c == '8') { _row = _saveRow; _col = _saveCol; }
        else if (c == 'D') newline();
        else if (c == 'E') newline();
        else if (c == 'c') { _fg = 7; _bg = 0; _bold = _rev = false; updateAttr(); clearAll(); _row = _col = 0; }
        _dirty = true;
        return;
      case 2:                                  // inside a CSI sequence
        if (c >= '0' && c <= '9') {
          if (_np == 0) _np = 1;
          int &v = _p[_np - 1];
          v = v * 10 + (c - '0');
          if (v > 9999) v = 9999;
        } else if (c == ';') {
          if (_np == 0) _np = 1;
          if (_np < 8) _np++;
        } else if (c == '?' || c == '>' || c == '!' || c == '=' || c == '<') {
          _priv = true;
        } else if (c >= 0x20 && c <= 0x3F) {
          // unhandled intermediate bytes / separators (e.g. ':'): ignored
        } else if (c >= 0x40 && c <= 0x7E) {   // final byte
          _pst = 0;
          csi((char)c);
          _dirty = true;
        } else {                               // unexpected control char: abort the sequence
          _pst = 0;
        }
        return;
      case 3:                                  // OSC
        if (c == 0x07) _pst = 0;
        else if (c == 0x1B) _pst = 1;
        return;
      case 4:                                  // ESC ( x  /  ESC # x
        _pst = 0;
        return;
      default: break;
    }
    if (c == 0x1B) { _pst = 1; _u8need = 0; return; }
    if (c >= 0x80) {
      if (_pet) putPrintable('?'); else if (_utf8) utf8Byte(c); else putPrintable(c);   // CP437: the byte is already the glyph index
      _dirty = true;
      return;
    }
    if (_utf8 && _u8need && utf8Byte(c)) { _dirty = true; return; }

    switch (c) {
      case '\r': _col = 0; break;
      case '\n': newline(); break;
      case '\t': do { putPrintable(' '); } while (_col % 8 != 0 && _col < _cols); break;
      case 8:
      case 127:  if (_col > 0) { if (_col >= _cols) _col = _cols - 1; else _col--; _scr[_row][_col].c = ' '; } break;
      case '\f': clearAll(); _row = _col = 0; break;
      default:
        if (c >= 32 && c < 127) putPrintable(c);
        break;
    }
    _dirty = true;
  }

  // -------------------------------------------------------------- drawing
  // Display task only.
  void redraw() {
    if (!_spriteOk) return;
    if (_menu) { drawMenu(); _canvas.pushSprite(&M5Cardputer.Display, 0, 0); return; }
    if (_book) { drawBook(); _canvas.pushSprite(&M5Cardputer.Display, 0, 0); return; }
    int row, col, view, hist, panX, panY, cols;
    bool curVis, pet;
    xSemaphoreTake(_termMux, portMAX_DELAY);
    view = _viewOff; hist = _histCount; row = _row; col = _col; curVis = _curVisible; cols = _cols; pet = _pet;
    panX = wide80() ? _panX : 0;
    if (_panY > _rows - ROWS) _panY = _rows - ROWS;
    if (view == 0 && _rows > ROWS && row != _lastCurRow) {       // the window follows the cursor
      _lastCurRow = row;
      if (row < _panY) _panY = row; else if (row >= _panY + ROWS) _panY = row - ROWS + 1;
    }
    panY = (_rows > ROWS) ? _panY : 0;
    for (int r = 0; r < ROWS; r++) {
      int k = hist + panY - view + r;                 // index into the "virtual line"
      const Cell *src;
      if (k < hist) src = _hist + (size_t)((_histHead - hist + k + _histCap) % _histCap) * MAXC;
      else          src = _scr[k - hist];
      memcpy(_snap[r], src + panX, sizeof(Cell) * COLS);
    }
    xSemaphoreGive(_termMux);

    const uint16_t *pal = pet ? _palPet : _pal;
    _canvas.fillSprite(TFT_BLACK);
    char buf[COLS + 1];
    for (int r = 0; r < ROWS; r++) {
      int y = r * 8;
      int c = 0;
      while (c < COLS) {                       // a "run" = consecutive cells with the same attribute
        uint8_t a = _snap[r][c].a;
        int e = c;
        while (e < COLS && _snap[r][e].a == a) e++;
        uint16_t fg = pal[a & 15], bg = pal[a >> 4];
        if (bg != TFT_BLACK) _canvas.fillRect(c * 6, y, (e - c) * 6, 8, bg);
        if (fg != bg) {
          _canvas.setTextColor(fg);            // transparent background
          int i = c;
          while (i < e) {
            uint8_t g = _snap[r][i].c;
            if (isSpecial(g)) { drawSpecial(g, i * 6, y, fg); i++; continue; }
            int j = i, n = 0;                  // ASCII segment: a single drawString
            while (j < e && !isSpecial(_snap[r][j].c)) buf[n++] = (char)_snap[r][j++].c;
            while (n > 0 && buf[n - 1] == ' ') n--;   // no trailing spaces
            buf[n] = 0;
            if (n) _canvas.drawString(buf, i * 6, y);
            i = j;
          }
        }
        c = e;
      }
    }

    if (view == 0 && curVis && _blink) {       // block cursor (live mode only)
      int cx = ((col >= cols) ? cols - 1 : col) - panX;
      if (cx >= 0 && cx < COLS) {
      int vr = row - panY;
      if (vr >= 0 && vr < ROWS) {
      uint8_t a = _snap[vr][cx].a;
      uint16_t fg = pal[a & 15], bg = pal[a >> 4];
      if (fg == bg) { fg = pet ? _palPet[14] : _pal[7]; bg = TFT_BLACK; }
      _canvas.fillRect(cx * 6, vr * 8, 6, 8, fg);
      uint8_t g = _snap[vr][cx].c;
      if (g != ' ') {
        _canvas.setTextColor(bg);
        if (isSpecial(g)) drawSpecial(g, cx * 6, vr * 8, bg);
        else { char t[2] = { (char)g, 0 }; _canvas.drawString(t, cx * 6, vr * 8); }
      }
      }
      }
    }

    drawStatusBar(view, hist);
    _canvas.pushSprite(&M5Cardputer.Display, 0, 0);
  }

  static bool isSpecial(uint8_t g) { return g < 32 || g >= 127; }

  // Non-ASCII glyph: boxes/blocks 0xB0..0xDF from generated bitmap (continuous),
  // the rest from font 0 in CP437 mode.
  void drawSpecial(uint8_t g, int x, int y, uint16_t fg) {
    if (_pet && g >= 0x80 && g <= 0xBF) {
      _canvas.drawBitmap(x, y, CPMS_PET[g - 0x80], 6, 8, fg);
    } else if (g >= 0xB0 && g <= 0xDF) {
      _canvas.drawBitmap(x, y, CPMS_BOX[g - 0xB0], 6, 8, fg);
    } else {
      _canvas.setTextColor(fg);
      _canvas.drawChar(g, x, y);
    }
  }

  // Status bar in the bottom 7 px: uppercase/digits only (the 8 px font
  // would clip the descenders of lowercase letters).
  void drawStatusBar(int view, int hist) {
    const uint16_t BAR_BG = 0x2104;            // dark gray
    const uint16_t TXT = 0xC618;               // light gray
    _canvas.fillRect(0, BAR_Y, 240, BAR_H, BAR_BG);

    // --- right part: SCROLL or [pan] [encoding] [call status] ---
    char st[24], pn[24] = "", rn[32] = "", en[8], dg[24] = "";
    const char *encs = _pet ? "PETSCII" : (_utf8 ? "UTF8" : "CP437");
    strcpy(en, encs);
    int rw = 0;                                // width of the right-hand group
    bool dbg = millis() < _dbgUntil;
    bool note = millis() < _noteUntil;
    if (view > 0) {
      snprintf(st, sizeof(st), "SCROLL %d/%d", view, hist);
      rw = (int)strlen(st) * 6 + 4;
    } else {
      int calls = _calls;
      if (calls > 0) snprintf(st, sizeof(st), "CALL %d", calls); else snprintf(st, sizeof(st), "IDLE");
      if (_rows > ROWS) snprintf(rn, sizeof(rn), "R%d-%d", _panY + 1, _panY + ROWS);
      if (wide80()) snprintf(pn, sizeof(pn), "%d-%d", _panX + 1, _panX + COLS);
      if (note) snprintf(dg, sizeof(dg), "%s", _note);
      else if (dbg) snprintf(dg, sizeof(dg), "FN KEY %02X", _dbgKey);
      rw = (int)strlen(st) * 6 + 2 + 6 + (int)((dbg || note) ? strlen(dg) : strlen(en)) * 6;
      if (pn[0]) rw += 6 + (int)strlen(pn) * 6;
      if (rn[0]) rw += 6 + (int)strlen(rn) * 6;
    }
    int room = 240 - rw - 6;                   // room for the left part

    // --- left part: WIFI, baud, IP (IP only if there is room) ---
    int x = 2;
    _canvas.setTextColor(_wifiOk ? (uint16_t)0x07E0 : (uint16_t)0xF800);
    _canvas.drawString("WIFI", x, BAR_Y);
    x += 4 * 6 + 3;
    _canvas.setTextColor(TXT);
    char bs[24];
    const char *cfg = cfgString();             // "8N1" is the normal case: takes no space
    if (strcmp(cfg, "8N1") == 0) snprintf(bs, sizeof(bs), "%lu", _baud);
    else                         snprintf(bs, sizeof(bs), "%lu %s", _baud, cfg);
    const char *ipt = _wifiOk ? _ip : "OFF";
    int ipw = (int)strlen(ipt) * 6 + 5, bw = (int)strlen(bs) * 6;
    if (x + ipw + bw <= room) { _canvas.drawString(ipt, x, BAR_Y); x += ipw; }
    if (x + bw <= room) _canvas.drawString(bs, x, BAR_Y);

    if (view > 0) {
      _canvas.fillRect(240 - rw, BAR_Y, rw, BAR_H, (uint16_t)0xFFE0);   // yellow: clearly visible
      _canvas.setTextColor(TFT_BLACK);
      _canvas.drawString(st, 240 - rw + 2, BAR_Y);
    } else {
      int rx = 240 - (int)strlen(st) * 6 - 2;
      _canvas.setTextColor((uint16_t)(_calls > 0 ? 0x07E0 : TXT));
      _canvas.drawString(st, rx, BAR_Y);
      rx -= 6 + (int)((dbg || note) ? strlen(dg) : strlen(en)) * 6;
      if (dbg || note) { _canvas.setTextColor((uint16_t)0xFFE0); _canvas.drawString(dg, rx, BAR_Y); }   // diagnostics
      else     { _canvas.setTextColor((uint16_t)0x07FF); _canvas.drawString(en, rx, BAR_Y); }   // encoding
      if (rn[0]) {
        rx -= 6 + (int)strlen(rn) * 6;
        _canvas.setTextColor((uint16_t)0xFFE0);                         // 25-row window
        _canvas.drawString(rn, rx, BAR_Y);
      }
      if (pn[0]) {
        rx -= 6 + (int)strlen(pn) * 6;
        _canvas.setTextColor((uint16_t)0xFFE0);                         // 80-column window
        _canvas.drawString(pn, rx, BAR_Y);
      }
    }
  }

  // Settings screen (full screen).
  void drawMenu() {
    const uint16_t TXT = 0xC618, SEL = 0x39E7;
    _canvas.fillSprite(TFT_BLACK);
    _canvas.fillRect(0, 0, 240, 10, (uint16_t)0x001F);
    _canvas.setTextColor((uint16_t)0xFFFF);
    _canvas.drawString("SETTINGS", 4, 1);
    static const char *lab[M_N] = { "Virtual screen 80 col", "Virtual screen 25 rows", "Text color",
                                    "Pin UART RX (GPIO)", "Pin UART TX (GPIO)",
                                    "Baud rate", "UART input", "Encoding", "Save and close" };
    for (int i = 0; i < M_N; i++) {
      int y = 12 + i * 10;
      if (i == _mSel) _canvas.fillRect(0, y - 1, 240, 10, SEL);
      char v[20] = "";
      uint16_t vc = 0xFFFF;
      switch (i) {
        case 0: strcpy(v, _wide ? "ON" : "OFF"); vc = _wide ? 0x07E0 : 0xF800; break;
        case 1: strcpy(v, _tall ? "ON" : "OFF"); vc = _tall ? 0x07E0 : 0xF800; break;
        case 2: snprintf(v, sizeof(v), "%s", colorName(_fgIdx)); vc = colorOf(_fgIdx); break;
        case 3: snprintf(v, sizeof(v), "%d", _cfgRx); vc = 0xFFE0; break;
        case 4: snprintf(v, sizeof(v), "%d", _cfgTx); vc = 0xFFE0; break;
        case 5: snprintf(v, sizeof(v), "%lu", _cfgBaud ? _cfgBaud : _baud); vc = 0xFFE0; break;
        case 6: strcpy(v, _uartIn ? "ON" : "OFF"); vc = _uartIn ? 0x07E0 : 0xF800; break;
        case 7: strcpy(v, _pet ? "PETSCII" : (_utf8 ? "UTF-8" : "CP437")); vc = 0x07FF; break;
        default: break;
      }
      _canvas.setTextColor(i == 8 ? (uint16_t)0xFFFF : TXT);
      _canvas.drawString(lab[i], i == 8 ? 90 : 6, y);
      if (v[0]) {
        char b[28];
        snprintf(b, sizeof(b), i == _mSel ? "< %s >" : "%s", v);
        _canvas.setTextColor(vc);
        _canvas.drawString(b, 234 - (int)strlen(b) * 6, y);
      }
    }
    if (_cfgRx != _rx || _cfgTx != _tx || (_baudTouched && _cfgBaud != _baud)) {
      _canvas.setTextColor((uint16_t)0xFFE0);
      _canvas.drawString("Pins and baud apply on close", 6, 107);
    }
    _canvas.fillRect(0, BAR_Y - 2, 240, BAR_H + 2, (uint16_t)0x2104);
    _canvas.setTextColor(TXT);
    _canvas.drawString("Up/Down item  Left/Right value  Esc exit", 4, BAR_Y - 1);
  }

  // BBS phonebook (full screen).
  void drawBook() {
    const uint16_t TXT = 0xC618, SEL = 0x39E7, DIM = 0x7BEF;
    _canvas.fillSprite(TFT_BLACK);
    _canvas.fillRect(0, 0, 240, 10, (uint16_t)0x001F);
    _canvas.setTextColor((uint16_t)0xFFFF);
    char t[32];
    if (_bEdit) snprintf(t, sizeof(t), "EDIT SLOT %d", _bSel + 1); else snprintf(t, sizeof(t), "BBS PHONEBOOK");
    _canvas.drawString(t, 4, 1);
    if (_bEdit) {
      for (int f = 0; f < 2; f++) {
        int y = 16 + f * 40;
        _canvas.setTextColor(f == _bField ? (uint16_t)0xFFE0 : TXT);
        _canvas.drawString(f ? "Address (host or host:port)" : "Nome", 4, y);
        _canvas.fillRect(2, y + 11, 236, 12, f == _bField ? SEL : (uint16_t)0x2104);
        char b[B_ADDR + 2];
        snprintf(b, sizeof(b), "%s%s", f ? _eAddr : _eName, f == _bField ? "_" : "");
        _canvas.setTextColor((uint16_t)0xFFFF);
        _canvas.drawString(b, 4, y + 13);
      }
    } else {
      for (int i = 0; i < B_N; i++) {
        int y = 11 + i * 9;
        if (i == _bSel) _canvas.fillRect(0, y - 1, 240, 9, SEL);
        char b[B_NAME + 8];
        snprintf(b, sizeof(b), "%d %s", i + 1, _bName[i][0] ? _bName[i] : (_bAddr[i][0] ? "-" : "(empty)"));
        _canvas.setTextColor(_bName[i][0] || _bAddr[i][0] ? (uint16_t)0xFFFF : DIM);
        _canvas.drawString(b, 6, y);
      }
      _canvas.setTextColor((uint16_t)0x07FF);
      char a[B_ADDR + 8];
      snprintf(a, sizeof(a), "%s", _bAddr[_bSel][0] ? _bAddr[_bSel] : "");
      _canvas.drawString(a, 4, 103);
    }
    if (_bMsg[0]) { _canvas.setTextColor((uint16_t)0xFFE0); _canvas.drawString(_bMsg, 4, 115); }
    _canvas.fillRect(0, BAR_Y - 2, 240, BAR_H + 2, (uint16_t)0x2104);
    _canvas.setTextColor(TXT);
    _canvas.drawString(_bEdit ? "Up/Down field  Enter save  Esc cancel" : "Enter call  Tab edit  Esc exit", 4, BAR_Y - 1);
  }

  // "8N1", "7E1", ... from the Arduino-ESP32 SerialConfig value
  const char *cfgString() {
    static char b[5];
    int data = ((_config >> 2) & 3) + 5;
    int par = _config & 3;
    int stop = (_config >> 4) & 3;
    b[0] = '0' + data;
    b[1] = (par == 2) ? 'E' : (par == 3) ? 'O' : 'N';
    b[2] = (stop == 3) ? '2' : '1';
    b[3] = 0;
    return b;
  }
};
