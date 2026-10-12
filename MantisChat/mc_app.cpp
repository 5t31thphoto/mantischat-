// =============================================================================
//  mc_app.cpp  --  boot, router, splash, home, callsign setup, UI helpers
// =============================================================================
//  Physical touch buttons under the Core2 screen:
//      A = Home     B = Chat (in chat: show/hide keyboard)     C = Whiteboard
// =============================================================================
#include "mc_app.h"
#include "mc_net.h"
#include "mc_kbd.h"
#include "mc_logo.h"
#include <Preferences.h>
#include <string.h>
#include <stdio.h>

M5Canvas g_fb(&M5.Display);
Screen   g_screen = SCR_SPLASH;
TouchIn  g_t = {false, false, false, 0, 0};
Toast    g_toast;
bool     g_mute = false;

static bool     s_dirty = true;
static uint32_t s_splashUntil = 0;
static bool     s_needCall = false;
static uint32_t s_vibeOff = 0;
static Kbd      s_kbd;
static char     s_callEdit[12] = "";

// ---- helpers ----------------------------------------------------------------------
void ui_present() { g_fb.pushSprite(0, 0); }

void ui_txt(const char* s, int x, int y, uint16_t col, uint8_t datum) {
    g_fb.setTextDatum(datum);
    g_fb.setTextColor(col);
    g_fb.drawString(s, x, y);
}

void ui_mascot(int x, int y, bool big) {
    const uint16_t* img = big ? MASCOT_L : MASCOT_S;
    int w = big ? MASCOT_L_W : MASCOT_S_W, h = big ? MASCOT_L_H : MASCOT_S_H;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint16_t c = img[j * w + i];
            if (!big && c == MC_MINT_565) continue;      // small icon: transparent bg
            g_fb.drawPixel(x + i, y + j, c);
        }
}

void ui_header(const char* title, const char* right) {
    g_fb.fillRect(0, 0, 320, 26, C_SURFACE);
    g_fb.drawFastHLine(0, 26, 320, C_HAIR);
    g_fb.setFont(&fonts::Font2);
    ui_txt("<", 12, 13, C_LIME, middle_center);
    ui_mascot(24, 2, false);
    ui_txt(title, 52, 13, C_LIME, middle_left);
    if (right && right[0]) {
        int w = g_fb.textWidth(right) + 16;
        g_fb.fillRoundRect(314 - w, 4, w, 19, 9, C_TEAL);
        ui_txt(right, 314 - w / 2, 13, C_TEXT, middle_center);
    }
}

bool ui_hit(int x, int y, int w, int h) {
    return g_t.pressed && g_t.x >= x && g_t.x < x + w && g_t.y >= y && g_t.y < y + h;
}

void app_notify(const char* who, const char* text) {
    strncpy(g_toast.who, who, 11); g_toast.who[11] = 0;
    strncpy(g_toast.text, text, 63); g_toast.text[63] = 0;
    g_toast.until = millis() + 3000;
    g_toast.seq++;
    M5.Power.setVibration(150);
    s_vibeOff = millis() + 90;
    if (!g_mute) M5.Speaker.tone(2700, 45);
    if (g_screen == SCR_HOME) s_dirty = true;
}

void app_save_callsign(const char* c) {
    strncpy(g_call, c, MC_CALL_MAX); g_call[MC_CALL_MAX] = 0;
    Preferences p;
    if (p.begin("mantischat", false)) { p.putString("call", g_call); p.end(); }
}

static void save_mute() {
    Preferences p;
    if (p.begin("mantischat", false)) { p.putBool("mute", g_mute); p.end(); }
}

// ---- router -------------------------------------------------------------------------
static void callsign_enter();
void app_go(Screen s) {
    if (s == g_screen) return;
    if (g_screen == SCR_BOARD) board_leave();
    g_screen = s;
    s_dirty = true;
    switch (s) {
        case SCR_CHAT:     chat_enter(); break;
        case SCR_CONTACTS: contacts_enter(); break;
        case SCR_BOARD:    board_enter(); break;
        case SCR_CALLSIGN: callsign_enter(); break;
        default: break;
    }
}

// ---- splash ---------------------------------------------------------------------------
static void draw_splash() {
    g_fb.fillScreen(C_BG);
    g_fb.fillRoundRect(92, 14, 136, 136, 18, MC_MINT_565);
    ui_mascot(96, 18, true);
    g_fb.setFont(&fonts::FreeSansBold12pt7b);
    ui_txt("MantisChat", 160, 176, C_LIME, middle_center);
    g_fb.setFont(&fonts::Font2);
    ui_txt("chat with Mantis Watches + live whiteboard", 160, 204, C_DIM, middle_center);
    g_fb.setFont(&fonts::Font0);
    char v[40];
    snprintf(v, sizeof(v), "v%s  |  MantisNow v1  |  ch %d", MC_VERSION, MESH_CHANNEL);
    ui_txt(v, 160, 226, C_TEAL_BRT, middle_center);
    ui_present();
}

// ---- home -------------------------------------------------------------------------------
static void draw_home() {
    g_fb.fillScreen(C_BG);
    // top bar
    g_fb.setFont(&fonts::FreeSansBold9pt7b);
    ui_txt("MantisChat", 10, 14, C_LIME, middle_left);
    g_fb.setFont(&fonts::Font2);
    char b[32];
    int bat = M5.Power.getBatteryLevel();
    if (bat >= 0) snprintf(b, sizeof(b), "%d%%", bat); else snprintf(b, sizeof(b), "--");
    ui_txt(b, 312, 14, C_DIM, middle_right);
    g_fb.fillRoundRect(214, 4, 52, 20, 10, g_mute ? C_SURFACE2 : C_TEAL);
    ui_txt(g_mute ? "muted" : "sound", 240, 14, g_mute ? C_DIM : C_TEXT, middle_center);

    // mascot tile + callsign
    g_fb.fillRoundRect(8, 34, 136, 136, 18, MC_MINT_565);
    ui_mascot(12, 38, true);
    g_fb.fillRoundRect(8, 178, 136, 52, 12, C_SURFACE2);
    g_fb.setFont(&fonts::Font0);
    ui_txt("callsign  (tap to edit)", 76, 190, C_DIM, middle_center);
    g_fb.setFont(&fonts::Font4);
    ui_txt(g_call, 76, 213, C_LIME, middle_center);
    if (g_fb.textWidth(g_call) > 128) {               // long callsign: smaller face
        g_fb.fillRoundRect(10, 198, 132, 30, 8, C_SURFACE2);
        g_fb.setFont(&fonts::Font2);
        ui_txt(g_call, 76, 213, C_LIME, middle_center);
    }

    // Chat tile
    g_fb.fillRoundRect(152, 34, 160, 78, 14, C_SURFACE2);
    g_fb.drawRoundRect(152, 34, 160, 78, 14, C_TEAL);
    g_fb.setFont(&fonts::FreeSansBold12pt7b);
    ui_txt("Chat", 166, 60, C_TEXT, middle_left);
    g_fb.setFont(&fonts::Font2);
    snprintf(b, sizeof(b), "%d nearby", net_peers_recent(30000, false));
    ui_txt("#general + DMs", 166, 86, C_TEAL_BRT, middle_left);
    ui_txt(b, 166, 102, C_DIM, middle_left);
    int un = chat_unread();
    if (un > 0) {
        snprintf(b, sizeof(b), "%d", un > 99 ? 99 : un);
        g_fb.fillCircle(290, 56, 13, C_PURPLE_BRT);
        ui_txt(b, 290, 56, C_TEXT, middle_center);
    }

    // Whiteboard tile
    g_fb.fillRoundRect(152, 120, 160, 78, 14, C_SURFACE2);
    g_fb.drawRoundRect(152, 120, 160, 78, 14, C_LIME_DIM);
    g_fb.setFont(&fonts::FreeSansBold12pt7b);
    ui_txt("Whiteboard", 166, 146, C_TEXT, middle_left);
    g_fb.setFont(&fonts::Font2);
    snprintf(b, sizeof(b), "%d drawing nearby", net_peers_recent(15000, true));
    ui_txt("live, everyone in range", 166, 172, C_LIME_DIM, middle_left);
    ui_txt(b, 166, 188, C_DIM, middle_left);

    g_fb.setFont(&fonts::Font0);
    ui_txt(net_up() ? "A home   B chat   C board" : "RADIO FAILED - restart device",
           232, 220, net_up() ? C_DIM : C_RED, middle_center);
    ui_present();
}

static void home_tick() {
    static uint32_t last = 0;
    if (millis() - last > 2000) { last = millis(); s_dirty = true; }
    if (ui_hit(152, 34, 160, 78))  { app_go(SCR_CHAT); return; }
    if (ui_hit(152, 120, 160, 78)) { app_go(SCR_BOARD); return; }
    if (ui_hit(8, 178, 136, 52))   { app_go(SCR_CALLSIGN); return; }
    if (ui_hit(210, 0, 60, 28))    { g_mute = !g_mute; save_mute(); s_dirty = true; }
    if (s_dirty) { s_dirty = false; draw_home(); }
}

// ---- callsign ------------------------------------------------------------------------
static void callsign_enter() {
    strncpy(s_callEdit, s_needCall ? "" : g_call, MC_CALL_MAX);
    s_callEdit[11] = 0;
    s_kbd.caps = true;
}

static void draw_callsign() {
    g_fb.fillScreen(C_BG);
    g_fb.fillRoundRect(8, 8, 52, 52, 10, MC_MINT_565);
    ui_mascot(23, 23, false);
    g_fb.setFont(&fonts::FreeSansBold9pt7b);
    ui_txt("Your callsign", 70, 22, C_LIME, middle_left);
    g_fb.setFont(&fonts::Font0);
    ui_txt("Shown to Mantis Watches and other MantisChats.", 70, 42, C_DIM, middle_left);
    ui_txt("Up to 8 characters.", 70, 54, C_DIM, middle_left);
    g_fb.fillRoundRect(8, 70, 304, 40, 12, C_SURFACE2);
    g_fb.drawRoundRect(8, 70, 304, 40, 12, C_LIME);
    g_fb.setFont(&fonts::Font4);
    ui_txt(s_callEdit[0] ? s_callEdit : "", 20, 90, C_TEXT, middle_left);
    int cx = 20 + g_fb.textWidth(s_callEdit) + 2;
    if ((millis() / 500) % 2 == 0) g_fb.fillRect(cx, 78, 3, 24, C_LIME);
    s_kbd.draw(g_fb, "Save");
    ui_present();
}

static void callsign_tick() {
    static uint32_t blink = 0;
    if (millis() - blink > 500) { blink = millis(); s_dirty = true; }
    int len = strlen(s_callEdit);
    if (g_t.pressed && s_kbd.contains(g_t.x, g_t.y)) {
        char c = s_kbd.press(g_t.x, g_t.y);
        if (c == KB_ENTER) {
            while (len && s_callEdit[len - 1] == ' ') s_callEdit[--len] = 0;
            if (len) {
                app_save_callsign(s_callEdit);
                s_needCall = false;
                app_go(SCR_HOME);
                return;
            }
        } else if (c == KB_BKSP) {
            if (len) s_callEdit[len - 1] = 0;
        } else if (c > 32 && c < 127 && len < MC_CALL_MAX) {        // no spaces in callsigns
            s_callEdit[len] = c; s_callEdit[len + 1] = 0;
        }
        s_dirty = true;
    } else if (g_t.down) {
        if (s_kbd.repeat(true, g_t.x, g_t.y) == KB_BKSP && len) { s_callEdit[len - 1] = 0; s_dirty = true; }
    } else if (g_t.released) {
        s_kbd.release();
        s_dirty = true;
    }
    if (s_dirty) { s_dirty = false; draw_callsign(); }
}

// ---- setup / loop ---------------------------------------------------------------------
void app_setup() {
    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    M5.begin(cfg);
    M5.Display.setBrightness(170);
    M5.Speaker.setVolume(110);

    g_fb.setPsram(true);
    g_fb.setColorDepth(16);
    if (!g_fb.createSprite(320, 240)) {
        g_fb.setPsram(false);
        g_fb.setColorDepth(8);
        g_fb.createSprite(320, 240);
    }

    // callsign: saved one, else "C2-xxxx" from the MAC (after net_begin)
    char saved[12] = "";
    {
        Preferences p;
        if (p.begin("mantischat", true)) {
            p.getString("call", saved, sizeof(saved));
            g_mute = p.getBool("mute", false);
            p.end();
        }
    }
    net_begin();
    if (saved[0]) strncpy(g_call, saved, 11);
    else {
        snprintf(g_call, sizeof(g_call), "C2-%02X%02X", g_mac[4], g_mac[5]);
        s_needCall = true;
    }
    g_call[MC_CALL_MAX] = 0;

    chat_begin();
    board_begin();
    draw_splash();
    s_splashUntil = millis() + 2200;
    Serial.printf("[MantisChat] v%s  call=%s  mac=%02X:%02X:%02X:%02X:%02X:%02X  psram=%u\n",
                  MC_VERSION, g_call, g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5],
                  (unsigned)ESP.getPsramSize());
}

void app_loop() {
    M5.update();
    const auto& d = M5.Touch.getDetail();
    g_t.pressed  = d.wasPressed();
    g_t.down     = d.isPressed();
    g_t.released = d.wasReleased();
    g_t.x = d.x;
    g_t.y = d.y;
    if (g_t.pressed && g_t.y >= 240) g_t.pressed = false;   // Core2 button strip

    net_poll();
    board_service();
    if (s_vibeOff && millis() > s_vibeOff) { M5.Power.setVibration(0); s_vibeOff = 0; }

    if (g_screen == SCR_SPLASH) {
        if (millis() > s_splashUntil || g_t.pressed) app_go(s_needCall ? SCR_CALLSIGN : SCR_HOME);
        delay(5);
        return;
    }
    bool lockToCall = (g_screen == SCR_CALLSIGN && s_needCall);
    if (!lockToCall) {
        if (M5.BtnA.wasPressed()) app_go(SCR_HOME);
        if (M5.BtnB.wasPressed() && g_screen != SCR_CHAT) app_go(SCR_CHAT);
        if (M5.BtnC.wasPressed()) app_go(SCR_BOARD);
        if (g_screen == SCR_CALLSIGN && g_t.pressed && g_t.y < 64 && g_t.x < 64) app_go(SCR_HOME);
    }
    switch (g_screen) {
        case SCR_HOME:     home_tick(); break;
        case SCR_CHAT:     chat_tick(); break;
        case SCR_CONTACTS: contacts_tick(); break;
        case SCR_BOARD:    board_tick(); break;
        case SCR_CALLSIGN: callsign_tick(); break;
        default: break;
    }
    delay(1);
}
