// =============================================================================
//  mc_app.h  --  shared state, colours, screen router
// =============================================================================
#pragma once
#include <M5Unified.h>
#include <stdint.h>
#include "mc_proto.h"

#define MC_VERSION "1.0.0"

constexpr uint16_t rgb565(uint32_t h) {
    return (uint16_t)((((h >> 16) & 0xF8) << 8) | (((h >> 8) & 0xFC) << 3) | ((h & 0xFF) >> 3));
}
// MantisOS palette (mw_theme.h)
constexpr uint16_t C_BG        = rgb565(0x000000);
constexpr uint16_t C_SURFACE   = rgb565(0x101014);
constexpr uint16_t C_SURFACE2  = rgb565(0x1C1C22);
constexpr uint16_t C_HAIR      = rgb565(0x2A2A32);
constexpr uint16_t C_TEXT      = rgb565(0xFFFFFF);
constexpr uint16_t C_DIM       = rgb565(0x8A8A93);
constexpr uint16_t C_LIME      = rgb565(0x9CFF2E);
constexpr uint16_t C_LIME_DIM  = rgb565(0x6FB81F);
constexpr uint16_t C_TEAL      = rgb565(0x007373);
constexpr uint16_t C_TEAL_BRT  = rgb565(0x00A6A6);
constexpr uint16_t C_PURPLE    = rgb565(0x5D005D);
constexpr uint16_t C_PURPLE_BRT= rgb565(0x8A008A);
constexpr uint16_t C_RED       = rgb565(0xFF4040);
constexpr uint16_t C_BOARD     = rgb565(0x101014);

enum Screen : uint8_t { SCR_SPLASH, SCR_HOME, SCR_CHAT, SCR_CONTACTS, SCR_BOARD, SCR_CALLSIGN };

struct TouchIn { bool pressed, down, released; int16_t x, y; };
struct Toast   { char who[12]; char text[64]; uint32_t until; uint32_t seq; };

extern M5Canvas g_fb;          // full-screen frame (PSRAM)
extern Screen   g_screen;
extern TouchIn  g_t;
extern Toast    g_toast;
extern bool     g_mute;

void app_setup();
void app_loop();
void app_go(Screen s);
void app_notify(const char* who, const char* text);
void app_save_callsign(const char* c);

// ui helpers (draw into g_fb)
void ui_present();
void ui_txt(const char* s, int x, int y, uint16_t col, uint8_t datum);
void ui_mascot(int x, int y, bool big);
void ui_header(const char* title, const char* right);
bool ui_hit(int x, int y, int w, int h);   // g_t.pressed inside rect

// screens
void chat_begin();
void chat_enter();
void chat_tick();
void contacts_enter();
void contacts_tick();
int  chat_unread();
void chat_set_dm(int peer);

void board_begin();
void board_enter();
void board_leave();
void board_tick();
void board_service();          // runs every loop on every screen
void board_on_wb(int peer, uint8_t sub, const uint8_t* payload, int len);
void board_forget_peer(int peer);
int  board_my_strokes();

void chat_on_peer(int peer);                                   // first time heard
void chat_on_text(int peer, bool dm, uint8_t chan, const char* text);
