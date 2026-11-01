// =============================================================================
//  mc_kbd.h  --  touch QWERTY for the Core2 (320 x 120, bottom half)
// =============================================================================
//  Two layers (letters / symbols), one-shot shift, hold-to-repeat backspace.
//  press() returns a printable char, KB_BKSP, KB_ENTER, or 0 (consumed).
// =============================================================================
#pragma once
#include <M5Unified.h>
#include <stdint.h>

enum : char { KB_NONE = 0, KB_SHIFT = 1, KB_LAYER = 2, KB_BKSP = 8, KB_ENTER = 10 };

class Kbd {
public:
    static const int H = 120, ROW_H = 30;
    int  y0 = 240 - H;
    bool caps = false;                 // shift latched for next letter

    void draw(M5Canvas& g, const char* enterLabel);
    char press(int x, int y);          // call on touch-down inside the keyboard
    char repeat(bool down, int x, int y);  // call every loop; backspace auto-repeat
    void release() { hot = -1; }
    bool contains(int x, int y) const { return y >= y0 && y < y0 + H && x >= 0 && x < 320; }

private:
    struct Key { int16_t x, y, w; char out; const char* label; };
    uint8_t  layer = 0;
    int8_t   hot = -1;
    uint32_t holdAt = 0, lastRep = 0;
    char     lbuf[2] = {0, 0};
    int  layout(Key* keys) const;      // returns key count for current layer
};
