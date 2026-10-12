#include "mc_kbd.h"
#include "mc_app.h"
#include <string.h>

// letters                                   symbols
//  q w e r t y u i o p                       1 2 3 4 5 6 7 8 9 0
//   a s d f g h j k l                        @ # $ % & * - + ( )
//  Shift z x c v b n m Del                   _ ! ? ' " : ; / Del
//  ?123 , [  space  ] . Send                 ABC  , [ space ] . Send
static const char* ROWS[2][3] = {
    { "qwertyuiop", "asdfghjkl", "zxcvbnm" },
    { "1234567890", "@#$%&*-+()", "!?'\":;/" },
};

int Kbd::layout(Key* k) const {
    int n = 0;
    const int y = y0;
    // row 0
    for (int i = 0; ROWS[layer][0][i]; i++) k[n++] = { (int16_t)(i * 32), (int16_t)y, 32, ROWS[layer][0][i], nullptr };
    // row 1 (letters are offset half a key)
    int len1 = strlen(ROWS[layer][1]);
    int off1 = (320 - len1 * 32) / 2;
    for (int i = 0; i < len1; i++) k[n++] = { (int16_t)(off1 + i * 32), (int16_t)(y + ROW_H), 32, ROWS[layer][1][i], nullptr };
    // row 2
    if (layer == 0) k[n++] = { 0, (int16_t)(y + 2 * ROW_H), 48, KB_SHIFT, "Shift" };
    else            k[n++] = { 0, (int16_t)(y + 2 * ROW_H), 48, '_', nullptr };
    for (int i = 0; ROWS[layer][2][i]; i++) k[n++] = { (int16_t)(48 + i * 32), (int16_t)(y + 2 * ROW_H), 32, ROWS[layer][2][i], nullptr };
    k[n++] = { 272, (int16_t)(y + 2 * ROW_H), 48, KB_BKSP, "Del" };
    // row 3
    const int16_t y3 = y + 3 * ROW_H;
    k[n++] = { 0,   y3, 56,  KB_LAYER, layer ? "ABC" : "?123" };
    k[n++] = { 56,  y3, 32,  ',', nullptr };
    k[n++] = { 88,  y3, 136, ' ', "space" };
    k[n++] = { 224, y3, 32,  '.', nullptr };
    k[n++] = { 256, y3, 64,  KB_ENTER, nullptr };     // label supplied at draw
    return n;
}

void Kbd::draw(M5Canvas& g, const char* enterLabel) {
    Key keys[48];
    int n = layout(keys);
    g.fillRect(0, y0, 320, H, C_SURFACE);
    g.setFont(&fonts::Font2);
    g.setTextDatum(middle_center);
    for (int i = 0; i < n; i++) {
        const Key& k = keys[i];
        bool special = k.label != nullptr || k.out == KB_ENTER;
        uint16_t bg = special ? C_SURFACE2 : C_HAIR;
        uint16_t fg = C_TEXT;
        if (k.out == KB_ENTER) { bg = C_LIME; fg = C_BG; }
        if (k.out == KB_SHIFT && caps) { bg = C_TEAL; }
        if (i == hot) { bg = C_LIME_DIM; fg = C_BG; }
        g.fillRoundRect(k.x + 2, k.y + 2, k.w - 4, ROW_H - 4, 5, bg);
        const char* lbl = k.label;
        if (k.out == KB_ENTER) lbl = enterLabel;
        if (!lbl) {
            char c = k.out;
            if (caps && c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            lbuf[0] = c;
            lbl = lbuf;
        }
        g.setTextColor(fg);
        g.drawString(lbl, k.x + k.w / 2, k.y + ROW_H / 2);
    }
}

char Kbd::press(int x, int y) {
    Key keys[48];
    int n = layout(keys);
    for (int i = 0; i < n; i++) {
        const Key& k = keys[i];
        if (x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + ROW_H) {
            hot = i;
            holdAt = millis();
            lastRep = holdAt;
            if (k.out == KB_SHIFT) { caps = !caps; return KB_NONE; }
            if (k.out == KB_LAYER) { layer ^= 1; caps = false; hot = -1; return KB_NONE; }
            char c = k.out;
            if (caps && c >= 'a' && c <= 'z') { c = c - 'a' + 'A'; caps = false; }
            return c;
        }
    }
    hot = -1;
    return KB_NONE;
}

char Kbd::repeat(bool down, int x, int y) {
    if (!down || hot < 0) return KB_NONE;
    Key keys[48];
    int n = layout(keys);
    if (hot >= n || keys[hot].out != KB_BKSP) return KB_NONE;
    const Key& k = keys[hot];
    if (!(x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + ROW_H)) return KB_NONE;
    uint32_t now = millis();
    if (now - holdAt > 450 && now - lastRep > 70) { lastRep = now; return KB_BKSP; }
    return KB_NONE;
}
