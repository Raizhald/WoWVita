// console.cpp — libctru's touch-screen text console: 40x30 characters of 8x8 pixels on the 320x240 touch screen.
//
// The game prints its start-up lines and its debug / offline pages with printf ("\x1b[row;colH" moves the cursor).
// printf, vprintf, puts and putchar are wrapped at link time (-Wl,--wrap=...) so their text lands in a character grid;
// c3d_gl.cpp shows the grid's pixels on the touch screen while the console owns it: from consoleInit / consoleClear
// until the game's GPU target for the touch screen is drawn again (the same hand-over the 3DS build does).
//
// W3D_ECHO=1 (PC test) also passes the text on to the real stdout.
#include <3ds.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "console_font.h"
#include "platform.h"

namespace {

constexpr int kCols = 40, kRows = 30, kW = 320, kH = 240;
constexpr u32 kFg = 0xFFFFFFFFu, kBg = 0xFF000000u;   // RGBA8 in memory order R,G,B,A (little endian: A in the top byte)

std::mutex g_lock;
char g_grid[kRows][kCols];
int g_row = 0, g_col = 0;
bool g_shown = true;    // the console owns the touch screen (start-up: until the game's GPU target is drawn)
bool g_dirty = true;    // the grid changed since its pixels were taken
u32 g_pixels[kW * kH];
int g_echo = -1;

// escape-sequence parser state
enum { kText, kEsc, kCsi } g_state = kText;
int g_args[4], g_nargs = 0;

void clearLocked() {
    memset(g_grid, ' ', sizeof g_grid);
    g_row = g_col = 0;
    g_dirty = true;
}

void newline() {
    g_col = 0;
    if (++g_row >= kRows) {
        memmove(g_grid[0], g_grid[1], sizeof(g_grid[0]) * (kRows - 1));
        memset(g_grid[kRows - 1], ' ', kCols);
        g_row = kRows - 1;
    }
}

void csi(char cmd) {
    const int a0 = g_nargs > 0 ? g_args[0] : 0, a1 = g_nargs > 1 ? g_args[1] : 0;
    switch (cmd) {
        case 'H': case 'f':   // cursor position, 1-based
            g_row = (a0 > 0 ? a0 : 1) - 1;
            g_col = (a1 > 0 ? a1 : 1) - 1;
            if (g_row >= kRows) g_row = kRows - 1;
            if (g_col >= kCols) g_col = kCols - 1;
            break;
        case 'A': g_row -= a0 > 0 ? a0 : 1; if (g_row < 0) g_row = 0; break;
        case 'B': g_row += a0 > 0 ? a0 : 1; if (g_row >= kRows) g_row = kRows - 1; break;
        case 'C': g_col += a0 > 0 ? a0 : 1; if (g_col >= kCols) g_col = kCols - 1; break;
        case 'D': g_col -= a0 > 0 ? a0 : 1; if (g_col < 0) g_col = 0; break;
        case 'J': if (a0 == 2) clearLocked(); break;
        case 'K':   // erase to the end of the line
            memset(&g_grid[g_row][g_col], ' ', kCols - g_col);
            break;
        default: break;   // colours (m) and the rest: ignored, the game uses none
    }
}

void feed(const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)s[i];
        switch (g_state) {
            case kText:
                if (c == 0x1b) { g_state = kEsc; break; }
                if (c == '\n') { newline(); break; }
                if (c == '\r') { g_col = 0; break; }
                if (c == '\t') { g_col = (g_col + 4) & ~3; if (g_col >= kCols) newline(); break; }
                if (c < 32) break;
                if (g_col >= kCols) newline();
                g_grid[g_row][g_col++] = c < 127 ? (char)c : '?';
                break;
            case kEsc:
                if (c == '[') { g_state = kCsi; g_nargs = 0; g_args[0] = 0; }
                else g_state = kText;
                break;
            case kCsi:
                if (c >= '0' && c <= '9') {
                    if (g_nargs == 0) g_nargs = 1;
                    if (g_nargs <= 4) g_args[g_nargs - 1] = g_args[g_nargs - 1] * 10 + (c - '0');
                } else if (c == ';') {
                    if (g_nargs == 0) g_nargs = 1;
                    if (g_nargs < 4) g_args[g_nargs++] = 0;
                    else g_nargs = 5;
                } else {
                    if (g_nargs > 4) g_nargs = 4;
                    csi((char)c);
                    g_state = kText;
                }
                break;
        }
    }
    g_dirty = true;
}

void write(const char* s, size_t n) {
    {
        std::lock_guard<std::mutex> l(g_lock);
        feed(s, n);
    }
    if (g_echo < 0) {
        const char* e = getenv("W3D_ECHO");
        g_echo = e && *e == '1';
    }
    if (g_echo) fwrite(s, 1, n, stdout);
}

}  // namespace

namespace compat {

bool consoleShown() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_shown;
}
void consoleHide() {
    std::lock_guard<std::mutex> l(g_lock);
    g_shown = false;
}

// The console's 320x240 RGBA8 pixels (row 0 = top); nullptr when nothing changed since the last call.
const u32* consolePixels() {
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_dirty) return nullptr;
    g_dirty = false;
    for (int r = 0; r < kRows; ++r)
        for (int c = 0; c < kCols; ++c) {
            const unsigned char ch = (unsigned char)g_grid[r][c];
            const u8* glyph = ch >= 32 && ch < 127 ? kConsoleFont[ch - 32] : kConsoleFont[0];
            for (int y = 0; y < 8; ++y) {
                u32* p = &g_pixels[(r * 8 + y) * kW + c * 8];
                const u8 bits = glyph[y];
                for (int x = 0; x < 8; ++x) p[x] = (bits & (0x80 >> x)) ? kFg : kBg;
            }
        }
    return g_pixels;
}

}  // namespace compat

extern "C" {

PrintConsole* consoleInit(gfxScreen_t screen, PrintConsole* console) {
    (void)screen;
    static PrintConsole def{kCols, kRows};
    {
        std::lock_guard<std::mutex> l(g_lock);
        clearLocked();
        g_shown = true;
    }
    return console ? console : &def;
}
PrintConsole* consoleSelect(PrintConsole* console) { return console; }
void consoleClear(void) {
    std::lock_guard<std::mutex> l(g_lock);
    clearLocked();
    g_shown = true;
}

// --wrap=printf,vprintf,puts,putchar: the game's stdout is the console
int __wrap_vprintf(const char* fmt, va_list ap) {
    char buf[1024];
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(buf, sizeof buf, fmt, cp);
    va_end(cp);
    if (n < 0) return n;
    if ((size_t)n < sizeof buf) {
        write(buf, (size_t)n);
    } else {
        char* big = (char*)malloc((size_t)n + 1);
        if (!big) return -1;
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        write(big, (size_t)n);
        free(big);
    }
    return n;
}
int __wrap_printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = __wrap_vprintf(fmt, ap);
    va_end(ap);
    return n;
}
int __wrap_puts(const char* s) {
    const size_t n = strlen(s);
    write(s, n);
    write("\n", 1);
    return (int)n + 1;
}
int __wrap_putchar(int c) {
    const char ch = (char)c;
    write(&ch, 1);
    return (unsigned char)c;
}

}  // extern "C"
