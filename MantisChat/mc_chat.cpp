// =============================================================================
//  mc_chat.cpp  --  #general group chat + DMs, wire-compatible with the
//                   MantisNow v1 TEXT (kind 0x01) + legacy MNW1.
// =============================================================================
#include "mc_app.h"
#include "mc_net.h"
#include "mc_kbd.h"
#include <string.h>
#include <stdio.h>

// ---- message store ------------------------------------------------------------
enum : uint8_t { MF_MINE = 1, MF_DM = 2, MF_SYS = 4 };
static const int MAX_MSGS = 48, MAX_LINES = 12, LINE_H = 16, BUBBLE_MAX = 236;
struct Msg {
    char    who[12];
    char    text[180];
    uint8_t flags;
    uint8_t nl;                 // wrapped line count
    uint8_t brk[MAX_LINES + 1]; // line start offsets (+ end)
    int16_t w, h;               // bubble size
};
static Msg* s_msgs = nullptr;   // ring
static int  s_head = 0, s_count = 0;

static bool    s_dm = false;
static int     s_dmPeer = -1;
static char    s_dmCall[12] = "";
static char    s_in[MC_TEXT_MAX + 1] = "";
static int     s_inLen = 0;
static bool    s_kb = false;
static Kbd     s_kbd;
static int     s_scroll = 0, s_maxScroll = 0;
static int     s_unread = 0;
static bool    s_dirty = true;
static int     s_dragY = -1, s_dragStart = 0;
static bool    s_dragged = false;

int chat_unread() { return s_unread; }

static Msg& msg_at(int i) {      // 0 = oldest
    int start = (s_head - s_count + MAX_MSGS) % MAX_MSGS;
    return s_msgs[(start + i) % MAX_MSGS];
}

static void sanitize(char* dst, const char* src, int max) {
    int j = 0;
    for (int i = 0; src[i] && j < max - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 32 && c < 127) dst[j++] = (char)c;
        else if (c == '\n' || c == '\t') dst[j++] = ' ';
        else if (c >= 0xC0) dst[j++] = '?';    // one '?' per UTF-8 sequence
    }
    dst[j] = 0;
}

// word-wrap once at insert time (Font2 widths are per-glyph, no kerning)
static void layout_msg(Msg& m) {
    g_fb.setFont(&fonts::Font2);
    const int maxW = (m.flags & MF_SYS) ? 300 : BUBBLE_MAX - 16;
    const char* t = m.text;
    int len = strlen(t), pos = 0, widest = 0;
    m.nl = 0;
    char one[2] = {0, 0};
    while (pos < len && m.nl < MAX_LINES) {
        m.brk[m.nl] = pos;
        int w = 0, i = pos, lastSpace = -1, wAtSpace = 0;
        while (i < len) {
            one[0] = t[i];
            int cw = g_fb.textWidth(one);
            if (w + cw > maxW) break;
            if (t[i] == ' ') { lastSpace = i; wAtSpace = w; }
            w += cw; i++;
        }
        int end = i;
        if (i < len && lastSpace > pos) { end = lastSpace; w = wAtSpace; }
        if (end == pos) { end = pos + 1; }
        if (w > widest) widest = w;
        m.nl++;
        pos = end;
        while (pos < len && t[pos] == ' ') pos++;
    }
    m.brk[m.nl] = pos < len ? pos : len;
    if (m.nl == 0) { m.brk[0] = 0; m.brk[1] = 0; m.nl = 1; }
    if (m.flags & MF_SYS) {
        m.w = 300; m.h = m.nl * LINE_H + 2;
    } else {
        char hdr[24];
        snprintf(hdr, sizeof(hdr), "%s%s", m.who, (m.flags & MF_DM) ? "  DM" : "");
        int hw = g_fb.textWidth(hdr);
        if (hw > widest) widest = hw;
        m.w = widest + 16;
        if (m.w > BUBBLE_MAX) m.w = BUBBLE_MAX;
        m.h = 6 + LINE_H + m.nl * LINE_H + 4;
    }
}

static void add_msg(const char* who, const char* text, uint8_t flags) {
    if (!s_msgs) return;
    Msg& m = s_msgs[s_head];
    memset(&m, 0, sizeof(m));
    strncpy(m.who, who, 11);
    sanitize(m.text, text, sizeof(m.text));
    m.flags = flags;
    layout_msg(m);
    s_head = (s_head + 1) % MAX_MSGS;
    if (s_count < MAX_MSGS) s_count++;
    if (s_scroll > 0) s_scroll += m.h + 6;     // keep your place if scrolled up
    s_dirty = true;
}

void chat_set_dm(int peer) {
    if (peer <= 0 || peer >= MAX_PEERS || !g_peers[peer].used) { s_dm = false; }
    else {
        s_dm = true;
        s_dmPeer = peer;
        strncpy(s_dmCall, g_peers[peer].call, 11);
        s_dmCall[11] = 0;
    }
    s_dirty = true;
}

// ---- RX from the air ------------------------------------------------------------
void chat_on_peer(int peer) {
    char b[48];
    snprintf(b, sizeof(b), "%s is in range", g_peers[peer].call);
    add_msg("", b, MF_SYS);
}

void chat_on_text(int peer, bool dm, uint8_t chan, const char* text) {
    const char* who = g_peers[peer].call;
    char tagged[200];
    if (chan) { snprintf(tagged, sizeof(tagged), "[ch%u] %s", (unsigned)chan, text); text = tagged; }
    add_msg(who, text, dm ? MF_DM : 0);
    if (g_screen != SCR_CHAT) s_unread++;
    app_notify(who, text);
}

void chat_begin() {
    s_msgs = (Msg*)ps_malloc(sizeof(Msg) * MAX_MSGS);
    if (!s_msgs) s_msgs = (Msg*)malloc(sizeof(Msg) * MAX_MSGS);
    add_msg("", "On air: MantisNow v1, ESP-NOW ch 1, chat channel 0 (general). Watches and Core2s in range show up under People.", MF_SYS);
}

// ---- layout ------------------------------------------------------------------------
static const int HDR_H = 26;
static int feed_bottom() { return s_kb ? 86 : 204; }
static int input_y()     { return s_kb ? 88 : 208; }

static void draw_msg(const Msg& m, int y) {
    g_fb.setFont(&fonts::Font2);
    if (m.flags & MF_SYS) {
        g_fb.setTextDatum(top_center);
        g_fb.setTextColor(C_DIM);
        for (int i = 0; i < m.nl; i++) {
            char line[181];
            int a = m.brk[i], b = m.brk[i + 1];
            if (i + 1 < m.nl) { b = m.brk[i + 1]; while (b > a && m.text[b - 1] == ' ') b--; }
            memcpy(line, m.text + a, b - a); line[b - a] = 0;
            g_fb.drawString(line, 160, y + i * LINE_H);
        }
        return;
    }
    bool mine = m.flags & MF_MINE;
    int x = mine ? 320 - 6 - m.w : 6;
    g_fb.fillRoundRect(x, y, m.w, m.h, 9, mine ? C_TEAL : C_SURFACE2);
    if (m.flags & MF_DM) g_fb.drawRoundRect(x, y, m.w, m.h, 9, C_PURPLE_BRT);
    g_fb.setTextDatum(top_left);
    g_fb.setTextColor(mine ? C_LIME : C_TEAL_BRT);
    g_fb.drawString(m.who, x + 8, y + 4);
    if (m.flags & MF_DM) {
        g_fb.setTextColor(rgb565(0xD070D0));
        g_fb.drawString("DM", x + 8 + g_fb.textWidth(m.who) + 8, y + 4);
    }
    g_fb.setTextColor(C_TEXT);
    for (int i = 0; i < m.nl; i++) {
        char line[181];
        int a = m.brk[i], b = m.brk[i + 1];
        while (b > a && m.text[b - 1] == ' ') b--;
        memcpy(line, m.text + a, b - a); line[b - a] = 0;
        g_fb.drawString(line, x + 8, y + 4 + LINE_H + i * LINE_H);
    }
}

static void draw_chat() {
    g_fb.fillScreen(C_BG);
    // header
    char title[32];
    if (s_dm) snprintf(title, sizeof(title), "DM  %s", s_dmCall);
    else      snprintf(title, sizeof(title), "#general");
    char right[16];
    snprintf(right, sizeof(right), "People %d", net_peers_recent(30000, false));
    ui_header(title, right);

    // feed (bottom-anchored, newest last)
    const int top = HDR_H + 2, bottom = feed_bottom();
    int total = 0;
    for (int i = 0; i < s_count; i++) total += msg_at(i).h + 6;
    s_maxScroll = total - (bottom - top);
    if (s_maxScroll < 0) s_maxScroll = 0;
    if (s_scroll > s_maxScroll) s_scroll = s_maxScroll;
    if (s_scroll < 0) s_scroll = 0;

    g_fb.setClipRect(0, top, 320, bottom - top);
    int y = bottom - 2 + s_scroll;
    for (int i = s_count - 1; i >= 0; i--) {
        const Msg& m = msg_at(i);
        y -= m.h;
        if (y + m.h < top) break;
        if (y < bottom) draw_msg(m, y);
        y -= 6;
    }
    g_fb.clearClipRect();
    if (s_scroll > 0) {                    // "jump to newest" hint
        g_fb.fillRoundRect(250, bottom - 22, 64, 18, 9, C_PURPLE);
        g_fb.setFont(&fonts::Font0);
        g_fb.setTextDatum(middle_center);
        g_fb.setTextColor(C_TEXT);
        g_fb.drawString("newest v", 282, bottom - 13);
    }

    // input bar
    const int iy = input_y();
    g_fb.fillRoundRect(6, iy, 246, 28, 14, C_SURFACE2);
    g_fb.drawRoundRect(6, iy, 246, 28, 14, s_kb ? C_LIME : C_TEAL);
    g_fb.setFont(&fonts::Font2);
    g_fb.setTextDatum(middle_left);
    if (s_inLen == 0 && !s_kb) {
        g_fb.setTextColor(C_DIM);
        g_fb.drawString(s_dm ? "Message (DM)" : "Message #general", 18, iy + 14);
    } else {
        // show the tail of the line that fits
        const char* p = s_in;
        while (*p && g_fb.textWidth(p) > 210) p++;
        g_fb.setTextColor(C_TEXT);
        g_fb.drawString(p, 18, iy + 14);
        if (s_kb && (millis() / 500) % 2 == 0)
            g_fb.fillRect(18 + g_fb.textWidth(p) + 1, iy + 6, 2, 16, C_LIME);
    }
    char cnt[8];
    if (s_kb) {
        snprintf(cnt, sizeof(cnt), "%d", MC_TEXT_MAX - s_inLen);
        g_fb.setFont(&fonts::Font0);
        g_fb.setTextColor(C_DIM);
        g_fb.setTextDatum(middle_right);
        g_fb.drawString(cnt, 246, iy + 14);
    }
    g_fb.fillRoundRect(258, iy, 56, 28, 14, s_inLen ? C_LIME : C_HAIR);
    g_fb.setFont(&fonts::Font2);
    g_fb.setTextDatum(middle_center);
    g_fb.setTextColor(s_inLen ? C_BG : C_DIM);
    g_fb.drawString("Send", 286, iy + 14);

    if (s_kb) s_kbd.draw(g_fb, "Send");
    ui_present();
}

static void send_now() {
    if (s_inLen == 0) return;
    net_send_text(s_dm ? s_dmPeer : -1, s_in);
    add_msg(g_call, s_in, MF_MINE | (s_dm ? MF_DM : 0));
    if (!net_up()) add_msg("", "Radio is down - message not sent.", MF_SYS);
    s_in[0] = 0; s_inLen = 0;
    s_scroll = 0;
}

void chat_enter() {
    s_unread = 0;
    s_dirty = true;
    s_scroll = 0;
}

void chat_tick() {
    static uint32_t lastBlink = 0;
    if (s_kb && millis() - lastBlink > 500) { lastBlink = millis(); s_dirty = true; }
    static uint32_t lastPeople = 0;
    if (millis() - lastPeople > 2000) { lastPeople = millis(); s_dirty = true; }

    const TouchIn& t = g_t;
    const int top = HDR_H + 2, bottom = feed_bottom(), iy = input_y();

    if (t.pressed) {
        s_dragY = -1;
        if (t.y < HDR_H) {
            if (t.x < 44) { app_go(SCR_HOME); return; }
            if (t.x > 220) { app_go(SCR_CONTACTS); return; }
            if (s_dm) { s_dm = false; s_dirty = true; }          // tap title: back to group
        } else if (s_kb && s_kbd.contains(t.x, t.y)) {
            char c = s_kbd.press(t.x, t.y);
            if (c == KB_ENTER) send_now();
            else if (c == KB_BKSP) { if (s_inLen) s_in[--s_inLen] = 0; }
            else if (c >= 32 && s_inLen < MC_TEXT_MAX) { s_in[s_inLen++] = c; s_in[s_inLen] = 0; }
            s_dirty = true;
        } else if (t.y >= iy && t.y < iy + 28) {
            if (t.x >= 258) send_now();
            else s_kb = true;
            s_dirty = true;
        } else if (t.y >= top && t.y < bottom) {
            if (s_scroll > 0 && t.x > 250 && t.y > bottom - 24) { s_scroll = 0; s_dirty = true; }
            else { s_dragY = t.y; s_dragStart = t.y; s_dragged = false; }
        }
    } else if (t.down) {
        if (s_kb) {
            char c = s_kbd.repeat(true, t.x, t.y);
            if (c == KB_BKSP && s_inLen) { s_in[--s_inLen] = 0; s_dirty = true; }
        }
        if (s_dragY >= 0 && t.y != s_dragY) {
            s_scroll += (t.y - s_dragY);
            s_dragY = t.y;
            if (abs(t.y - s_dragStart) > 6) s_dragged = true;
            s_dirty = true;
        }
    } else if (t.released) {
        if (s_dragY >= 0 && !s_dragged && s_kb) { s_kb = false; }  // tap feed hides keys
        s_dragY = -1;
        s_kbd.release();
        s_dirty = true;
    }

    if (M5.BtnB.wasPressed()) { s_kb = !s_kb; s_dirty = true; }

    if (s_dirty) { s_dirty = false; draw_chat(); }
}

// ==============================================================================
//  Contacts ("People")
// ==============================================================================
static int  s_cScroll = 0, s_cDragY = -1, s_cDragStart = 0;
static bool s_cDirty = true;
static int  s_order[MAX_PEERS];
static int  s_nOrder = 0;

static uint32_t last_seen(const Peer& p) { return p.seenChat > p.seenBoard ? p.seenChat : p.seenBoard; }

static void build_order() {
    s_nOrder = 0;
    for (int i = 1; i < MAX_PEERS; i++) if (g_peers[i].used) s_order[s_nOrder++] = i;
    for (int a = 1; a < s_nOrder; a++)          // newest first
        for (int b = a; b > 0 && last_seen(g_peers[s_order[b]]) > last_seen(g_peers[s_order[b - 1]]); b--) {
            int t = s_order[b]; s_order[b] = s_order[b - 1]; s_order[b - 1] = t;
        }
}

static const int ROW = 36;
static void draw_contacts() {
    build_order();
    g_fb.fillScreen(C_BG);
    ui_header("People", "");
    g_fb.setClipRect(0, HDR_H, 320, 240 - HDR_H);
    int y = HDR_H + 4 - s_cScroll;
    // group row
    g_fb.fillRoundRect(6, y, 308, ROW - 4, 8, s_dm ? C_SURFACE2 : C_TEAL);
    g_fb.setFont(&fonts::Font2);
    g_fb.setTextDatum(middle_left);
    g_fb.setTextColor(C_LIME);
    g_fb.drawString("# Group  #general", 16, y + (ROW - 4) / 2);
    y += ROW;
    uint32_t now = millis();
    for (int k = 0; k < s_nOrder; k++) {
        const Peer& p = g_peers[s_order[k]];
        bool active = s_dm && s_order[k] == s_dmPeer;
        g_fb.fillRoundRect(6, y, 308, ROW - 4, 8, active ? C_PURPLE : C_SURFACE2);
        uint32_t ago = (now - last_seen(p)) / 1000;
        uint16_t dot = ago < 15 ? C_LIME : (ago < 60 ? C_TEAL_BRT : C_DIM);
        g_fb.fillCircle(20, y + (ROW - 4) / 2, 4, dot);
        g_fb.setTextDatum(middle_left);
        g_fb.setTextColor(C_TEXT);
        g_fb.setFont(&fonts::Font2);
        g_fb.drawString(p.call, 32, y + (ROW - 4) / 2);
        char r[24];
        if (ago < 60) snprintf(r, sizeof(r), "%lus", (unsigned long)ago);
        else          snprintf(r, sizeof(r), "%lum", (unsigned long)(ago / 60));
        g_fb.setTextDatum(middle_right);
        g_fb.setTextColor(C_DIM);
        g_fb.drawString(r, 304, y + (ROW - 4) / 2);
        if (p.seenBoard) {
            g_fb.setFont(&fonts::Font0);
            g_fb.setTextColor(C_LIME_DIM);
            g_fb.drawString("board", 262, y + (ROW - 4) / 2);
        }
        y += ROW;
    }
    if (s_nOrder == 0) {
        g_fb.setFont(&fonts::Font2);
        g_fb.setTextDatum(top_center);
        g_fb.setTextColor(C_DIM);
        g_fb.drawString("Nobody heard yet.", 160, y + 16);
        g_fb.drawString("Open Mesh on a Mantis Watch nearby,", 160, y + 36);
        g_fb.drawString("or power up another MantisChat.", 160, y + 54);
    } else {
        g_fb.setFont(&fonts::Font0);
        g_fb.setTextDatum(top_center);
        g_fb.setTextColor(C_DIM);
        g_fb.drawString("Tap a name to DM.  Tap Group for #general.", 160, y + 4);
    }
    g_fb.clearClipRect();
    ui_present();
}

void contacts_enter() { s_cScroll = 0; s_cDirty = true; }

void contacts_tick() {
    static uint32_t last = 0;
    if (millis() - last > 1000) { last = millis(); s_cDirty = true; }
    const TouchIn& t = g_t;
    if (t.pressed) {
        if (t.y < HDR_H && t.x < 44) { app_go(SCR_CHAT); return; }
        s_cDragY = t.y; s_cDragStart = t.y;
    } else if (t.down && s_cDragY >= 0) {
        s_cScroll -= (t.y - s_cDragY);
        int maxS = (s_nOrder + 2) * ROW - (240 - HDR_H);
        if (maxS < 0) maxS = 0;
        if (s_cScroll > maxS) s_cScroll = maxS;
        if (s_cScroll < 0) s_cScroll = 0;
        s_cDragY = t.y;
        s_cDirty = true;
    } else if (t.released && s_cDragY >= 0) {
        bool tap = abs(t.y - s_cDragStart) < 8;
        s_cDragY = -1;
        if (tap && t.y >= HDR_H) {
            int row = (t.y - (HDR_H + 4) + s_cScroll) / ROW;
            if (row == 0) { chat_set_dm(-1); app_go(SCR_CHAT); return; }
            if (row - 1 < s_nOrder) { chat_set_dm(s_order[row - 1]); app_go(SCR_CHAT); return; }
        }
    }
    if (s_cDirty) { s_cDirty = false; draw_contacts(); }
}
