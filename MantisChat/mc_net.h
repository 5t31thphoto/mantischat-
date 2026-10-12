// =============================================================================
//  mc_net.h  --  MantisNow v1 transport, peer table, paced TX queue
// =============================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>

#define MAX_PEERS 32           // slot 0 is always this device
#define MC_CALL_MAX 8          // MantisNow callsign field

struct Peer {
    uint8_t  mac[6];
    uint32_t node;
    char     call[12];
    uint32_t seenChat;         // millis, 0 = never
    uint32_t seenBoard;
    bool     used;
    bool     legacy;           // heard via old MNW1 frames
};

extern Peer          g_peers[MAX_PEERS];
extern char          g_call[12];
extern uint8_t       g_mac[6];
extern uint32_t      g_node;
extern const uint8_t BCAST_MAC[6];

bool net_begin();
bool net_up();
void net_poll();
int  net_tx_free();
bool net_send_v1(uint8_t kind, uint8_t sub, uint32_t dst, const void* payload, size_t len, uint8_t flags = 0);
void net_send_text(int peer, const char* text);    // peer <= 0: channel 0 broadcast
int  net_peers_recent(uint32_t windowMs, bool boardOnly);
void net_copy_call(char* dst, const char* src, int n);
