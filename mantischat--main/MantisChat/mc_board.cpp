// =============================================================================
//  mc_board.cpp  --  MantisChat live community whiteboard (Core2 <-> Core2)
// =============================================================================
//  * MantisNow v1 kind 0x40 (WHITEBOARD.md). Strokes stream live as WB_SEG.
//  * Every board keeps every stroke it has heard (PSRAM), keyed by
//    (owner peer, sid), so a delete can redraw the board without that stroke.
//  * Ownership is enforced by the RECEIVER: DEL/CLEAR only touch strokes whose
//    owner MAC is the packet's source MAC. You can erase, undo or clear your
//    own ink everywhere; you can never touch anyone else's.
//  * Joining: entering the board broadcasts WB_SYNC; every board replays its
//    OWN live strokes (paced), so a newcomer sees what is already up.
//  * Deletes are sent 3x (0 / 150 / 600 ms) and tombstoned, so a late or
//    replayed chunk can't resurrect a stroke.
// =============================================================================
#include "mc_app.h"
#include "mc_net.h"
#include <string.h>
#include <math.h>
#if __has_include(<esp_random.h>)
#include <esp_random.h>
#else
#include <esp_system.h>
#endif

static const int BAR_H = 240 - BOARD_H;     // 34 px toolbar

// ---- palette / sizes ------------------------------------------------------------
static const uint16_t INKS[6] = {
    rgb565(0xFFFFFF), rgb565(0x9CFF2E), rgb565(0x00D0D0),
    rgb565(0xE050E0), rgb565(0xFFD400), rgb565(0xFF5A4A),
};
static const uint8_t SIZES[4] = {2, 4, 8, 14};

// ---- stroke store ---------------------------------------------------------------
struct Rec {
    uint32_t sid;
    uint32_t off;      // first point in pool
    uint16_t seq;
    uint16_t color;
    uint16_t n;
    uint8_t  peer;     // index into g_peers; 0 = mine
    uint8_t  width;
    uint8_t  alive;
};
static Rec*     s_recs = nullptr;
static int      s_maxRecs = 0, s_nRecs = 0;
static int16_t* s_pool = nullptr;       // x,y pairs
static uint32_t s_poolMax = 0, s_poolUsed = 0;

static M5Canvas s_cv(&M5.Display);
static bool     s_cvOk = false;

// per-peer CLEAR watermark: ignore chunks with sid older than this (same session)
struct Floor { uint32_t sid; bool set; };
static Floor s_floor[MAX_PEERS];
struct Tomb { uint32_t sid; uint8_t peer; };
static Tomb s_tomb[128];
static int  s_tombN = 0, s_tombI = 0;

// own pen state
static uint32_t s_nextSid = 0;
static uint8_t  s_ink = 1, s_size = 1;
static bool     s_erase = false;
static bool     s_drawing = false;
static uint32_t s_curSid = 0;
static uint16_t s_curSeq = 0;
static int16_t  s_pend[MCW_MAX_PTS * 2];
static int      s_npend = 0;
static int      s_lastX = 0, s_lastY = 0;
static uint32_t s_lastFlush = 0;

// services
static int      s_replayIdx = -1;
static uint32_t s_replayAt = 0;
struct PendSend { uint32_t sid; uint32_t at; uint8_t left; uint8_t type; };
static PendSend s_pend3[10];
static uint32_t s_lastHello = 0;
static uint32_t s_clearArmUntil = 0;
static bool     s_onScreen = false;
static bool     s_barDirty = true;
static bool     s_needRedraw = false;
static uint32_t s_toastShown = 0;
static bool     s_toastUp = false;
// dirty rect (board space) to mirror canvas -> LCD
static int s_dx0 = 9999, s_dy0 = 9999, s_dx1 = -1, s_dy1 = -1;

static inline bool sid_before(uint32_t a, uint32_t b) {     // a older than b, same session
    return (a >> 16) == (b >> 16) && (int16_t)((a & 0xFFFF) - (b & 0xFFFF)) < 0;
}

static void dirty(int x0, int y0, int x1, int y1) {
    if (x0 < s_dx0) s_dx0 = x0;
    if (y0 < s_dy0) s_dy0 = y0;
    if (x1 > s_dx1) s_dx1 = x1;
    if (y1 > s_dy1) s_dy1 = y1;
}

// ---- rendering ------------------------------------------------------------------
static void render_pts(const int16_t* p, int n, uint16_t color, uint8_t width) {
    if (!s_cvOk || n < 1) return;
    float r = width * 0.5f;
    if (r < 1.0f) r = 1.0f;
    int pad = (int)r + 2;
    int x0 = p[0], y0 = p[1], x1 = p[0], y1 = p[1];
    if (n == 1) {
        s_cv.fillCircle(p[0], p[1], (int)(r + 0.5f), color);
    } else {
        for (int i = 1; i < n; i++) {
            s_cv.drawWideLine(p[2 * i - 2], p[2 * i - 1], p[2 * i], p[2 * i + 1], r, color);
            if (p[2 * i] < x0) x0 = p[2 * i];
            if (p[2 * i] > x1) x1 = p[2 * i];
            if (p[2 * i + 1] < y0) y0 = p[2 * i + 1];
            if (p[2 * i + 1] > y1) y1 = p[2 * i + 1];
        }
    }
    dirty(x0 - pad, y0 - pad, x1 + pad, y1 + pad);
}

static void redraw_all() {
    if (!s_cvOk) return;
    s_cv.fillScreen(C_BOARD);
    for (int i = 0; i < s_nRecs; i++) {
        const Rec& r = s_recs[i];
        if (r.alive) render_pts(&s_pool[r.off * 2], r.n, r.color, r.width);
    }
    if (s_drawing && s_npend > 0)                 // unflushed tail of the stroke in progress
        render_pts(s_pend, s_npend, INKS[s_ink], SIZES[s_size]);
    dirty(0, 0, BOARD_W, BOARD_H);
    s_needRedraw = false;
}

static void flush_lcd() {
    if (!s_onScreen || !s_cvOk || s_dx1 < 0) return;
    int x0 = s_dx0 < 0 ? 0 : s_dx0, y0 = s_dy0 < 0 ? 0 : s_dy0;
    int x1 = s_dx1 >= BOARD_W ? BOARD_W - 1 : s_dx1, y1 = s_dy1 >= BOARD_H ? BOARD_H - 1 : s_dy1;
    if (x1 >= x0 && y1 >= y0) {
        // clip so only the changed rectangle crosses the SPI bus
        M5.Display.setClipRect(x0, y0 + BAR_H, x1 - x0 + 1, y1 - y0 + 1);
        s_cv.pushSprite(0, BAR_H);
        M5.Display.clearClipRect();
    }
    s_dx0 = s_dy0 = 9999; s_dx1 = s_dy1 = -1;
}

// ---- store ops ------------------------------------------------------------------
static void compact() {
    int w = 0;
    uint32_t used = 0;
    for (int i = 0; i < s_nRecs; i++) {
        Rec r = s_recs[i];
        if (!r.alive) continue;
        if (r.off != used) memmove(&s_pool[used * 2], &s_pool[r.off * 2], r.n * 4);
        r.off = used;
        used += r.n;
        s_recs[w++] = r;
    }
    if (s_replayIdx > w) s_replayIdx = w;
    s_nRecs = w;
    s_poolUsed = used;
}

static bool kill_stroke(uint8_t peer, uint32_t sid) {
    bool any = false;
    for (int i = 0; i < s_nRecs; i++)
        if (s_recs[i].alive && s_recs[i].peer == peer && s_recs[i].sid == sid) { s_recs[i].alive = 0; any = true; }
    return any;
}

static bool rec_add(uint8_t peer, uint32_t sid, uint16_t seq, uint16_t color, uint8_t width,
                    const int16_t* pts, int n) {
    if (!s_recs || n < 1 || n > MCW_MAX_PTS) return false;
    for (int guard = 0; guard < 64; guard++) {
        if (s_nRecs < s_maxRecs && s_poolUsed + n <= s_poolMax) break;
        compact();
        if (s_nRecs < s_maxRecs && s_poolUsed + n <= s_poolMax) break;
        // full of live ink: the oldest stroke falls off this board (locally)
        int i = 0;
        while (i < s_nRecs && !s_recs[i].alive) i++;
        if (i >= s_nRecs) return false;
        kill_stroke(s_recs[i].peer, s_recs[i].sid);
        s_needRedraw = true;
    }
    if (s_nRecs >= s_maxRecs || s_poolUsed + n > s_poolMax) return false;
    Rec& r = s_recs[s_nRecs++];
    r.sid = sid; r.seq = seq; r.color = color; r.width = width;
    r.peer = peer; r.alive = 1; r.n = n; r.off = s_poolUsed;
    memcpy(&s_pool[s_poolUsed * 2], pts, n * 4);
    s_poolUsed += n;
    return true;
}

static bool rec_has(uint8_t peer, uint32_t sid, uint16_t seq) {
    for (int i = s_nRecs - 1; i >= 0; i--) {
        const Rec& r = s_recs[i];
        if (r.peer == peer && r.sid == sid && r.seq == seq && r.alive) return true;
    }
    return false;
}

static void tomb_add(uint8_t peer, uint32_t sid) {
    s_tomb[s_tombI] = {sid, peer};
    s_tombI = (s_tombI + 1) % 128;
    if (s_tombN < 128) s_tombN++;
}
static bool tomb_has(uint8_t peer, uint32_t sid) {
    for (int i = 0; i < s_tombN; i++) if (s_tomb[i].peer == peer && s_tomb[i].sid == sid) return true;
    return false;
}

void board_forget_peer(int peer) {
    if (peer <= 0 || peer >= MAX_PEERS) return;
    bool any = false;
    for (int i = 0; i < s_nRecs; i++) if (s_recs[i].peer == peer && s_recs[i].alive) { s_recs[i].alive = 0; any = true; }
    for (int i = 0; i < s_tombN; i++) if (s_tomb[i].peer == peer) s_tomb[i].peer = 0xFF;
    s_floor[peer].set = false;
    if (any) s_needRedraw = true;
}

int board_my_strokes() {
    int n = 0;
    uint32_t last = 0xFFFFFFFF;
    for (int i = 0; i < s_nRecs; i++)
        if (s_recs[i].alive && s_recs[i].peer == 0 && s_recs[i].sid != last) { n++; last = s_recs[i].sid; }
    return n;
}

// ---- TX helpers -----------------------------------------------------------------
static bool send_seg(uint32_t sid, uint16_t seq, uint16_t color, uint8_t width, const int16_t* pts, int n) {
    WbSeg s;
    s.sid = sid; s.seq = seq; s.color = color; s.width = width; s.n = (uint8_t)n;
    memcpy(s.pts, pts, n * 4);
    return net_send_v1(MN_WB, WB_SEG, NODE_ALL, &s, WB_SEG_BASE + n * 4);
}
static void send_sid(uint8_t sub, uint32_t sid) {
    WbSid p; p.sid = sid;
    net_send_v1(MN_WB, sub, NODE_ALL, &p, sizeof(p));
}
static void send_bare(uint8_t sub) { net_send_v1(MN_WB, sub, NODE_ALL, nullptr, 0); }
static void send_3x(uint8_t type, uint32_t sid) {
    send_sid(type, sid);
    uint32_t now = millis();
    int put = 0;
    for (int i = 0; i < 10 && put < 2; i++) {
        if (s_pend3[i].left) continue;
        s_pend3[i] = {sid, now + (put ? 600u : 150u), 1, type};
        put++;
    }
}

// ---- own strokes ----------------------------------------------------------------
static void flush_chunk(bool final) {
    if (s_npend < 1) return;
    if (s_npend == 1 && s_curSeq > 0) { if (final) s_npend = 0; return; }   // just the joint
    uint16_t col = INKS[s_ink];
    uint8_t  w   = SIZES[s_size];
    rec_add(0, s_curSid, s_curSeq, col, w, s_pend, s_npend);
    send_seg(s_curSid, s_curSeq, col, w, s_pend, s_npend);
    s_curSeq++;
    // next chunk starts at the last point so the line stays continuous
    s_pend[0] = s_pend[(s_npend - 1) * 2];
    s_pend[1] = s_pend[(s_npend - 1) * 2 + 1];
    s_npend = final ? 0 : 1;
    s_lastFlush = millis();
}

static void pen_down(int x, int y) {
    s_drawing = true;
    s_curSid = s_nextSid++;
    s_curSeq = 0;
    s_pend[0] = x; s_pend[1] = y; s_npend = 1;
    s_lastX = x; s_lastY = y;
    s_lastFlush = millis();
    render_pts(s_pend, 1, INKS[s_ink], SIZES[s_size]);
}
static void pen_move(int x, int y) {
    int dx = x - s_lastX, dy = y - s_lastY;
    if (dx * dx + dy * dy < 9) return;               // 3 px minimum step
    int16_t seg[4] = {(int16_t)s_lastX, (int16_t)s_lastY, (int16_t)x, (int16_t)y};
    render_pts(seg, 2, INKS[s_ink], SIZES[s_size]);
    s_pend[s_npend * 2] = x; s_pend[s_npend * 2 + 1] = y; s_npend++;
    s_lastX = x; s_lastY = y;
    if (s_npend >= MCW_MAX_PTS || millis() - s_lastFlush > 45) flush_chunk(false);
}
static void pen_up() {
    if (!s_drawing) return;
    flush_chunk(true);
    s_drawing = false;
}

static void delete_mine(uint32_t sid) {
    kill_stroke(0, sid);
    send_3x(WB_DEL, sid);
    redraw_all();
}

static int seg_dist2(int px, int py, int ax, int ay, int bx, int by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float L = vx * vx + vy * vy;
    float t = L > 0 ? (wx * vx + wy * vy) / L : 0;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    float dx = ax + t * vx - px, dy = ay + t * vy - py;
    return (int)(dx * dx + dy * dy);
}

static void erase_at(int x, int y) {          // topmost OWN stroke under the finger
    for (int i = s_nRecs - 1; i >= 0; i--) {
        const Rec& r = s_recs[i];
        if (!r.alive || r.peer != 0) continue;
        int tol = r.width / 2 + 9;
        const int16_t* p = &s_pool[r.off * 2];
        for (int k = 0; k < r.n; k++) {
            int d = (k == 0) ? (p[0] - x) * (p[0] - x) + (p[1] - y) * (p[1] - y)
                             : seg_dist2(x, y, p[2 * k - 2], p[2 * k - 1], p[2 * k], p[2 * k + 1]);
            if (d <= tol * tol) { delete_mine(r.sid); return; }
        }
    }
}

static void undo_mine() {
    for (int i = s_nRecs - 1; i >= 0; i--)
        if (s_recs[i].alive && s_recs[i].peer == 0) { delete_mine(s_recs[i].sid); return; }
}

static void clear_mine() {
    for (int i = 0; i < s_nRecs; i++) if (s_recs[i].peer == 0) s_recs[i].alive = 0;
    send_3x(WB_CLEAR, s_nextSid);
    redraw_all();
}

// ---- RX -------------------------------------------------------------------------
void board_on_wb(int peer, uint8_t sub, const uint8_t* data, int len) {
    if (peer <= 0 || peer >= MAX_PEERS) return;
    switch (sub) {
    case WB_SEG: {
        if (len < WB_SEG_BASE) return;
        WbSeg s;
        memset(&s, 0, sizeof(s));
        memcpy(&s, data, len > (int)sizeof(s) ? (int)sizeof(s) : len);
        int n = s.n;
        if (n < 1 || n > MCW_MAX_PTS || len < WB_SEG_BASE + n * 4) return;
        if (s_floor[peer].set && sid_before(s.sid, s_floor[peer].sid)) return;
        if (tomb_has(peer, s.sid) || rec_has(peer, s.sid, s.seq)) return;
        int16_t pts[MCW_MAX_PTS * 2];
        for (int i = 0; i < n * 2; i++) {
            int v = s.pts[i];
            int lim = (i & 1) ? BOARD_H + 32 : BOARD_W + 32;
            pts[i] = (int16_t)(v < -32 ? -32 : (v > lim ? lim : v));
        }
        uint8_t w = s.width < 1 ? 1 : (s.width > 32 ? 32 : s.width);
        if (rec_add(peer, s.sid, s.seq, s.color, w, pts, n) && !s_needRedraw)
            render_pts(pts, n, s.color, w);
        break;
    }
    case WB_DEL: {
        if (len < (int)sizeof(WbSid)) return;
        WbSid p; memcpy(&p, data, sizeof(p));
        if (!tomb_has(peer, p.sid)) tomb_add(peer, p.sid);
        if (kill_stroke(peer, p.sid)) s_needRedraw = true;   // only the SENDER's strokes
        break;
    }
    case WB_CLEAR: {
        if (len < (int)sizeof(WbSid)) return;
        WbSid p; memcpy(&p, data, sizeof(p));
        bool any = false;
        for (int i = 0; i < s_nRecs; i++)
            if (s_recs[i].peer == peer && s_recs[i].alive) { s_recs[i].alive = 0; any = true; }
        s_floor[peer].sid = p.sid;
        s_floor[peer].set = true;
        if (any) s_needRedraw = true;
        break;
    }
    case WB_SYNC:
        s_replayIdx = 0;                             // restart replay of my strokes
        s_replayAt = millis() + 60 + (esp_random() % 400);
        break;
    default: break;                                  // WB_HELLO / unknown: presence only
    }
}

// ---- toolbar ---------------------------------------------------------------------
// [<]  6 inks  [size] [ERASE] [UNDO] [CLEAR]
static const int X_INK = 32, INK_W = 24, X_SIZE = 178, X_ERASE = 210, X_UNDO = 248, X_CLEAR = 284;

static void draw_two(int x, int w, const char* a, const char* b, uint16_t bg, uint16_t fg) {
    M5.Display.fillRoundRect(x + 2, 3, w - 4, BAR_H - 6, 6, bg);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(fg);
    M5.Display.drawString(a, x + w / 2, 13);
    M5.Display.setTextColor(fg == C_BG ? C_BG : C_DIM);
    M5.Display.drawString(b, x + w / 2, 24);
}

static void draw_bar() {
    M5.Display.startWrite();
    M5.Display.fillRect(0, 0, 320, BAR_H, C_BG);
    M5.Display.drawFastHLine(0, BAR_H - 1, 320, C_HAIR);
    // back
    M5.Display.setFont(&fonts::Font2);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(C_LIME);
    M5.Display.drawString("<", 14, BAR_H / 2);
    // inks
    for (int i = 0; i < 6; i++) {
        int x = X_INK + i * INK_W;
        if (i == s_ink && !s_erase) M5.Display.fillRoundRect(x + 1, 4, INK_W - 2, BAR_H - 8, 6, C_TEXT);
        M5.Display.fillRoundRect(x + 4, 7, INK_W - 8, BAR_H - 14, 4, INKS[i]);
    }
    // size
    M5.Display.fillRoundRect(X_SIZE + 2, 3, 28, BAR_H - 6, 6, C_SURFACE2);
    M5.Display.fillCircle(X_SIZE + 16, BAR_H / 2, SIZES[s_size] / 2 + 1, INKS[s_ink]);
    // tools (all of them only ever touch YOUR strokes)
    draw_two(X_ERASE, 38, "ERASE", "mine", s_erase ? C_LIME : C_SURFACE2, s_erase ? C_BG : C_TEXT);
    draw_two(X_UNDO, 36, "UNDO", "mine", C_SURFACE2, C_TEXT);
    bool armed = millis() < s_clearArmUntil;
    draw_two(X_CLEAR, 36, armed ? "SURE?" : "CLEAR", armed ? "tap" : "mine",
             armed ? C_RED : C_SURFACE2, armed ? C_BG : C_TEXT);
    M5.Display.endWrite();
    s_barDirty = false;
}

static void draw_toast() {
    M5.Display.startWrite();
    M5.Display.fillRect(0, 0, 320, BAR_H, C_PURPLE);
    M5.Display.setFont(&fonts::Font2);
    M5.Display.setTextDatum(middle_left);
    M5.Display.setTextColor(C_LIME);
    M5.Display.drawString(g_toast.who, 8, BAR_H / 2);
    int wx = 8 + M5.Display.textWidth(g_toast.who) + 8;
    char t[64];
    strncpy(t, g_toast.text, sizeof(t) - 1); t[63] = 0;
    while (strlen(t) > 1 && M5.Display.textWidth(t) > 300 - wx) t[strlen(t) - 1] = 0;
    M5.Display.setTextColor(C_TEXT);
    M5.Display.drawString(t, wx, BAR_H / 2);
    M5.Display.endWrite();
}

static void bar_tap(int x) {
    if (x < X_INK) { app_go(SCR_HOME); return; }
    if (x < X_SIZE) { s_ink = (x - X_INK) / INK_W; if (s_ink > 5) s_ink = 5; s_erase = false; }
    else if (x < X_ERASE) { s_size = (s_size + 1) % 4; }
    else if (x < X_UNDO) { s_erase = !s_erase; }
    else if (x < X_CLEAR) { undo_mine(); }
    else {
        if (millis() < s_clearArmUntil) { clear_mine(); s_clearArmUntil = 0; }
        else s_clearArmUntil = millis() + 2500;
    }
    s_barDirty = true;
}

// ---- lifecycle -------------------------------------------------------------------
void board_begin() {
    s_maxRecs = 4000;
    s_poolMax = 60000;
    s_recs = (Rec*)ps_malloc(sizeof(Rec) * s_maxRecs);
    s_pool = (int16_t*)ps_malloc(s_poolMax * 4);
    if (!s_recs || !s_pool) {                         // no PSRAM: smaller board memory
        free(s_recs); free(s_pool);
        s_maxRecs = 600; s_poolMax = 6000;
        s_recs = (Rec*)malloc(sizeof(Rec) * s_maxRecs);
        s_pool = (int16_t*)malloc(s_poolMax * 4);
        if (!s_recs || !s_pool) { s_maxRecs = 0; s_poolMax = 0; }
    }
    s_cv.setPsram(true);
    s_cv.setColorDepth(16);
    s_cvOk = s_cv.createSprite(BOARD_W, BOARD_H) != nullptr;
    if (!s_cvOk) {
        s_cv.setPsram(false);
        s_cv.setColorDepth(8);
        s_cvOk = s_cv.createSprite(BOARD_W, BOARD_H) != nullptr;
    }
    // sid = random 16-bit boot session | counter -> no clash with pre-reboot strokes
    s_nextSid = (esp_random() & 0xFFFF0000u) | 1u;
    memset(s_floor, 0, sizeof(s_floor));
    redraw_all();
}

void board_enter() {
    s_onScreen = true;
    s_barDirty = true;
    s_toastUp = false;
    s_toastShown = g_toast.seq;
    if (s_needRedraw) redraw_all();
    draw_bar();
    if (s_cvOk) s_cv.pushSprite(0, BAR_H);
    else {
        M5.Display.fillRect(0, BAR_H, 320, BOARD_H, C_BG);
        M5.Display.setTextColor(C_RED);
        M5.Display.drawString("Not enough memory for the board", 160, 140);
    }
    s_dx0 = s_dy0 = 9999; s_dx1 = s_dy1 = -1;
    send_bare(WB_SYNC);                              // ask everyone to replay their ink
    send_bare(WB_HELLO);
    s_lastHello = millis();
}

void board_leave() {
    pen_up();
    s_onScreen = false;
}

void board_service() {
    uint32_t now = millis();
    // delayed delete/clear repeats
    for (int i = 0; i < 10; i++) {
        PendSend& p = s_pend3[i];
        if (p.left && (int32_t)(now - p.at) >= 0) { send_sid(p.type, p.sid); p.left = 0; }
    }
    // replay my strokes for a newcomer, paced by TX queue space
    if (s_replayIdx >= 0 && (int32_t)(now - s_replayAt) >= 0) {
        while (s_replayIdx < s_nRecs && net_tx_free() > 8) {
            const Rec& r = s_recs[s_replayIdx++];
            if (r.peer == 0 && r.alive) send_seg(r.sid, r.seq, r.color, r.width, &s_pool[r.off * 2], r.n);
        }
        if (s_replayIdx >= s_nRecs) s_replayIdx = -1;
    }
    if (s_onScreen && now - s_lastHello > 4000) { s_lastHello = now; send_bare(WB_HELLO); }
    if (s_needRedraw) redraw_all();
    if (s_onScreen) flush_lcd();
}

void board_tick() {
    const TouchIn& t = g_t;
    int bx = t.x, by = t.y - BAR_H;
    if (bx < 0) bx = 0;
    if (bx >= BOARD_W) bx = BOARD_W - 1;
    if (by >= BOARD_H) by = BOARD_H - 1;

    if (t.pressed) {
        if (t.y < BAR_H) {
            if (s_toastUp) { app_go(SCR_CHAT); return; }
            bar_tap(t.x);
            if (g_screen != SCR_BOARD) return;
        } else if (s_erase) {
            erase_at(bx, by);
        } else {
            pen_down(bx, by);
        }
    } else if (t.down && s_drawing) {
        if (by < 0) by = 0;
        pen_move(bx, by);
    } else if (t.released || (!t.down && s_drawing)) {
        pen_up();
    }
    if (s_drawing && millis() - s_lastFlush > 45 && s_npend > 1) flush_chunk(false);

    // chat toast in the toolbar
    uint32_t now = millis();
    if (g_toast.seq != s_toastShown && now < g_toast.until) {
        s_toastShown = g_toast.seq;
        s_toastUp = true;
        draw_toast();
    } else if (s_toastUp && now >= g_toast.until) {
        s_toastUp = false;
        s_barDirty = true;
    }
    static uint32_t armWas = 0;
    if (s_clearArmUntil && now >= s_clearArmUntil && armWas != s_clearArmUntil) { armWas = s_clearArmUntil; s_barDirty = true; }
    if (s_barDirty && !s_toastUp) draw_bar();
    flush_lcd();
}
