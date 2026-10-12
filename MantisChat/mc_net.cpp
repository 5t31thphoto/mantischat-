// =============================================================================
//  mc_net.cpp  --  MantisNow v1 over ESP-NOW, channel 1, broadcast peer
// =============================================================================
//  Bring-up follows the v1 build spec (STA, promiscuous channel lock, init,
//  broadcast peer). RX callback only copies into a ring; parsing happens in
//  net_poll(). Receive rules: len >= 28, magic 0x4D, version 1, drop own
//  srcNode, dedup (srcNode,pktId) over the last 32, reassemble FRAG, drop
//  unknown kinds, ACK when flags bit1 && dst == self, PONG every PING.
//  Legacy MNW1 (MantisOS <= 1.5.8) is accepted and echoed for group/DM text.
// =============================================================================
#include "mc_net.h"
#include "mc_app.h"
#include "mc_proto.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#if __has_include(<esp_random.h>)
#include <esp_random.h>
#else
#include <esp_system.h>
#endif
#include <string.h>

Peer          g_peers[MAX_PEERS];
char          g_call[12] = "MANTIS";
uint8_t       g_mac[6];
uint32_t      g_node = 0;
const uint8_t BCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static bool     s_up = false;
static uint16_t s_seq = 0;
static uint32_t s_lastPing = 0, s_lastLegacyBeacon = 0;

// ---- RX ring --------------------------------------------------------------------
struct RxSlot { uint8_t mac[6]; uint16_t len; uint8_t data[250]; };
static const int RXQ = 24;
static RxSlot s_rx[RXQ];
static volatile int s_rxHead = 0, s_rxTail = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void on_recv_raw(const uint8_t* mac, const uint8_t* data, int len) {
    if (!mac || !data || len < 4 || len > 250 || data[0] != 0x4D) return;
    portENTER_CRITICAL_ISR(&s_mux);
    int next = (s_rxHead + 1) % RXQ;
    if (next != s_rxTail) {
        RxSlot& s = s_rx[s_rxHead];
        memcpy(s.mac, mac, 6); s.len = (uint16_t)len; memcpy(s.data, data, len);
        s_rxHead = next;
    }
    portEXIT_CRITICAL_ISR(&s_mux);
}
#if ESP_IDF_VERSION_MAJOR >= 5
static void on_recv(const esp_now_recv_info_t* info, const uint8_t* d, int len) { on_recv_raw(info ? info->src_addr : nullptr, d, len); }
#else
static void on_recv(const uint8_t* mac, const uint8_t* d, int len) { on_recv_raw(mac, d, len); }
#endif

// ---- TX queue -------------------------------------------------------------------
struct TxSlot { uint8_t to[6]; uint16_t len; uint8_t data[250]; };
static const int TXQ = 40;
static TxSlot s_tx[TXQ];
static int s_txHead = 0, s_txTail = 0;
int net_tx_free() { return (TXQ - 1) - ((s_txHead - s_txTail + TXQ) % TXQ); }

static bool ensure_peer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) return true;
    esp_now_peer_info_t pi;
    memset(&pi, 0, sizeof(pi));
    memcpy(pi.peer_addr, mac, 6);
    pi.channel = MESH_CHANNEL;
    pi.ifidx = WIFI_IF_STA;
    pi.encrypt = false;
    return esp_now_add_peer(&pi) == ESP_OK;
}

static bool enqueue(const uint8_t* to, const void* d, size_t len) {
    if (!s_up || !len || len > 250 || net_tx_free() <= 0) return false;
    TxSlot& t = s_tx[s_txHead];
    memcpy(t.to, to, 6); t.len = (uint16_t)len; memcpy(t.data, d, len);
    s_txHead = (s_txHead + 1) % TXQ;
    return true;
}

static void tx_pump() {
    for (int sent = 0; sent < 3 && s_txTail != s_txHead; sent++) {
        TxSlot& t = s_tx[s_txTail];
        if (memcmp(t.to, BCAST_MAC, 6) != 0) ensure_peer(t.to);   // legacy DM only
        if (esp_now_send(t.to, t.data, t.len) == ESP_ERR_ESPNOW_NO_MEM) return;
        s_txTail = (s_txTail + 1) % TXQ;
    }
}

bool net_send_v1(uint8_t kind, uint8_t sub, uint32_t dst, const void* payload, size_t len, uint8_t flags) {
    if (len > (size_t)MN_MAX_PAYLOAD) return false;
    uint8_t buf[250];
    MnHdr h;
    memset(&h, 0, sizeof(h));
    h.magic = 0x4D; h.version = 1; h.kind = kind; h.flags = flags;
    h.srcNode = g_node; h.dstNode = dst; h.pktId = esp_random();
    h.seq = s_seq++; h.ttl = 0; h.chanOrSub = sub;
    strncpy(h.callsign, g_call, sizeof(h.callsign));     // null-padded, may fill all 8
    memcpy(buf, &h, sizeof(h));
    if (len) memcpy(buf + sizeof(h), payload, len);
    return enqueue(BCAST_MAC, buf, sizeof(h) + len);
}

// ---- peers ----------------------------------------------------------------------
void net_copy_call(char* dst, const char* src, int n) {
    int j = 0;
    for (int i = 0; i < n && src[i] && j < 11; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 32 && c < 127) dst[j++] = (char)c;
        else if (c >= 0xC0) dst[j++] = '?';
    }
    dst[j] = 0;
    if (!j) strcpy(dst, "?");
}

static uint32_t node_from_mac(const uint8_t* m) {
    return (uint32_t)m[0] | ((uint32_t)m[1] << 8) | ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24);
}

static int peer_touch(const uint8_t* mac, uint32_t node, const char* call, int callLen,
                      bool board, bool legacy, bool* isNew) {
    if (isNew) *isNew = false;
    uint32_t now = millis();
    int freeSlot = -1, oldest = -1;
    uint32_t oldestT = 0xFFFFFFFF;
    for (int i = 1; i < MAX_PEERS; i++) {
        Peer& p = g_peers[i];
        if (!p.used) { if (freeSlot < 0) freeSlot = i; continue; }
        if (memcmp(p.mac, mac, 6) == 0) {
            if (call) net_copy_call(p.call, call, callLen);
            if (board) p.seenBoard = now; else p.seenChat = now;
            p.node = node;
            if (legacy) p.legacy = true;
            return i;
        }
        uint32_t last = p.seenChat > p.seenBoard ? p.seenChat : p.seenBoard;
        if (last < oldestT) { oldestT = last; oldest = i; }
    }
    int i = freeSlot;
    if (i < 0) { i = oldest; board_forget_peer(i); }
    Peer& p = g_peers[i];
    memset(&p, 0, sizeof(p));
    p.used = true; p.legacy = legacy; p.node = node;
    memcpy(p.mac, mac, 6);
    if (call) net_copy_call(p.call, call, callLen); else strcpy(p.call, "?");
    if (board) p.seenBoard = now; else p.seenChat = now;
    if (isNew) *isNew = true;
    return i;
}

int net_peers_recent(uint32_t windowMs, bool boardOnly) {
    uint32_t now = millis();
    int n = 0;
    for (int i = 1; i < MAX_PEERS; i++) {
        const Peer& p = g_peers[i];
        if (!p.used) continue;
        uint32_t t = boardOnly ? p.seenBoard : (p.seenChat > p.seenBoard ? p.seenChat : p.seenBoard);
        if (t && now - t < windowMs) n++;
    }
    return n;
}

// ---- text -----------------------------------------------------------------------
void net_send_text(int peer, const char* text) {
    size_t n = strlen(text);
    if (n == 0) return;
    if (n > (size_t)MN_TEXT_MAX) n = MN_TEXT_MAX;
    bool dm = peer > 0 && peer < MAX_PEERS && g_peers[peer].used;
    net_send_v1(MN_TEXT, 0, dm ? g_peers[peer].node : NODE_ALL, text, n);
#if MC_LEGACY_MNW1
    if (!dm || g_peers[peer].legacy) {
        MnwPacket p;
        memset(&p, 0, sizeof(p));
        memcpy(p.magic, "MNW1", 4);
        p.type = dm ? MNW_DIRECT : MNW_GROUP;
        strncpy(p.callsign, g_call, sizeof(p.callsign) - 1);
        strncpy(p.text, text, sizeof(p.text) - 1);
        enqueue(dm ? g_peers[peer].mac : BCAST_MAC, &p, sizeof(p));
    }
#endif
}

// ---- dedup + fragments ---------------------------------------------------------
struct Seen { uint32_t src, pkt; };
static Seen s_seen[32];
static int  s_seenI = 0;
static bool seen_check(uint32_t src, uint32_t pkt) {
    for (int i = 0; i < 32; i++) if (s_seen[i].src == src && s_seen[i].pkt == pkt) return true;
    s_seen[s_seenI] = {src, pkt};
    s_seenI = (s_seenI + 1) % 32;
    return false;
}

struct Reasm { bool used; uint32_t src, msgId, started; uint8_t total, innerKind, got; uint16_t len[16]; uint8_t data[16 * MN_FRAG_CHUNK]; };
static Reasm* s_reasm = nullptr;          // 4 streams, PSRAM

static void dispatch(int peer, bool isNew, const MnHdr& h, const uint8_t* pl, int plen);

static void on_frag(int peer, bool isNew, const MnHdr& h, const uint8_t* pl, int plen) {
    if (!s_reasm || plen < (int)sizeof(MnFragHdr)) return;
    MnFragHdr f;
    memcpy(&f, pl, sizeof(f));
    if (f.total == 0 || f.total > 16 || f.idx >= f.total) return;
    uint32_t now = millis();
    Reasm* r = nullptr;
    for (int i = 0; i < 4; i++) {
        if (s_reasm[i].used && now - s_reasm[i].started > 2000) s_reasm[i].used = false;   // 2 s timeout
        if (s_reasm[i].used && s_reasm[i].src == h.srcNode && s_reasm[i].msgId == f.msgId) r = &s_reasm[i];
    }
    if (!r) {
        for (int i = 0; i < 4 && !r; i++) if (!s_reasm[i].used) r = &s_reasm[i];
        if (!r) return;
        memset(r, 0, sizeof(Reasm) - sizeof(r->data));
        r->used = true; r->src = h.srcNode; r->msgId = f.msgId; r->started = now;
        r->total = f.total; r->innerKind = f.innerKind;
    }
    int clen = plen - (int)sizeof(f);
    if (clen <= 0 || clen > MN_FRAG_CHUNK || r->len[f.idx]) return;
    memcpy(r->data + f.idx * MN_FRAG_CHUNK, pl + sizeof(f), clen);
    r->len[f.idx] = clen;
    if (++r->got < r->total) return;
    static uint8_t whole[16 * MN_FRAG_CHUNK];
    int w = 0;
    for (int i = 0; i < r->total; i++) { memcpy(whole + w, r->data + i * MN_FRAG_CHUNK, r->len[i]); w += r->len[i]; }
    MnHdr inner = h;
    inner.kind = r->innerKind;
    inner.flags &= ~MNF_FRAG;
    r->used = false;
    dispatch(peer, isNew, inner, whole, w);
}

static void dispatch(int peer, bool isNew, const MnHdr& h, const uint8_t* pl, int plen) {
    bool forMe = h.dstNode == NODE_ALL || h.dstNode == g_node;
    switch (h.kind) {
    case MN_TEXT: {
        if (!forMe) { if (isNew) chat_on_peer(peer); return; }
        char text[MN_TEXT_MAX + 1];
        int n = plen > MN_TEXT_MAX ? MN_TEXT_MAX : plen;
        memcpy(text, pl, n); text[n] = 0;
        if (isNew) chat_on_peer(peer);
        if (n > 0) chat_on_text(peer, h.dstNode != NODE_ALL, h.chanOrSub, text);
        break;
    }
    case MN_PING: {
        uint32_t pong[2] = {h.pktId, h.srcNode};
        net_send_v1(MN_PONG, 0, h.srcNode, pong, sizeof(pong));
        if (isNew) chat_on_peer(peer);
        break;
    }
    case MN_PONG: case MN_POS: case MN_ACK: case MN_KID_PING: case MN_KID_ACK:
    case MN_VS: case MN_SHR: case MN_DANCE:
        if (isNew) chat_on_peer(peer);                 // presence only
        break;
    case MN_WB:
        board_on_wb(peer, h.chanOrSub, pl, plen);
        break;
    default: break;                                    // unknown: drop silently
    }
}

static void handle_v1(const RxSlot& m) {
    if (m.len < (int)sizeof(MnHdr)) return;
    MnHdr h;
    memcpy(&h, m.data, sizeof(h));
    if (h.magic != 0x4D || h.version != 1) return;
    if (h.srcNode == g_node) return;
    if (seen_check(h.srcNode, h.pktId)) return;
    bool board = h.kind == MN_WB;
    bool isNew = false;
    int peer = peer_touch(m.mac, h.srcNode, h.callsign, 8, board, false, &isNew);
    if ((h.flags & MNF_ACKREQ) && h.dstNode == g_node) {
        uint32_t id = h.pktId;
        net_send_v1(MN_ACK, 0, h.srcNode, &id, sizeof(id));
    }
    const uint8_t* pl = m.data + sizeof(MnHdr);
    int plen = m.len - (int)sizeof(MnHdr);
    if (h.flags & MNF_FRAG) on_frag(peer, isNew, h, pl, plen);
    else dispatch(peer, isNew, h, pl, plen);
}

static void handle_legacy(const RxSlot& m) {
#if MC_LEGACY_MNW1
    if (m.len < (int)sizeof(MnwPacket)) return;
    MnwPacket p;
    memcpy(&p, m.data, sizeof(p));
    p.callsign[11] = 0; p.text[179] = 0;
    bool isNew = false;
    int peer = peer_touch(m.mac, node_from_mac(m.mac), p.callsign, 11, false, true, &isNew);
    if (isNew) chat_on_peer(peer);
    if (p.type == MNW_GROUP || p.type == MNW_DIRECT) chat_on_text(peer, p.type == MNW_DIRECT, 0, p.text);
#else
    (void)m;
#endif
}

// ---- lifecycle ------------------------------------------------------------------
bool net_begin() {
    uint64_t efuse = ESP.getEfuseMac();
    g_node = (uint32_t)(efuse & 0xFFFFFFFFu);          // mac[0] is the low byte
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(MESH_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    esp_wifi_get_mac(WIFI_IF_STA, g_mac);

    memset(g_peers, 0, sizeof(g_peers));
    g_peers[0].used = true;
    g_peers[0].node = g_node;
    memcpy(g_peers[0].mac, g_mac, 6);
    s_reasm = (Reasm*)ps_malloc(sizeof(Reasm) * 4);
    if (!s_reasm) s_reasm = (Reasm*)malloc(sizeof(Reasm) * 4);
    if (s_reasm) memset(s_reasm, 0, sizeof(Reasm) * 4);

    if (esp_now_init() != ESP_OK) { s_up = false; return false; }
    esp_now_register_recv_cb(on_recv);
    ensure_peer(BCAST_MAC);
    s_up = true;
    s_seq = (uint16_t)esp_random();
    s_lastPing = millis() - 20000;                     // ping on the first loop
    s_lastLegacyBeacon = millis() - 6000;
    return true;
}

bool net_up() { return s_up; }

void net_poll() {
    if (!s_up) return;
    strncpy(g_peers[0].call, g_call, 11);
    for (int k = 0; k < 10; k++) {
        static RxSlot m;
        bool got = false;
        portENTER_CRITICAL(&s_mux);
        if (s_rxTail != s_rxHead) { m = s_rx[s_rxTail]; s_rxTail = (s_rxTail + 1) % RXQ; got = true; }
        portEXIT_CRITICAL(&s_mux);
        if (!got) break;
        if (memcmp(m.mac, g_mac, 6) == 0) continue;
        if (m.len >= 4 && memcmp(m.data, "MNW1", 4) == 0) handle_legacy(m);
        else handle_v1(m);
    }
    uint32_t now = millis();
    if (now - s_lastPing > 10000) { s_lastPing = now; net_send_v1(MN_PING, 0, NODE_ALL, nullptr, 0); }
#if MC_LEGACY_MNW1
    if (now - s_lastLegacyBeacon > 5000) {              // old watches list us as a contact
        s_lastLegacyBeacon = now;
        MnwPacket p;
        memset(&p, 0, sizeof(p));
        memcpy(p.magic, "MNW1", 4);
        p.type = MNW_BEACON;
        strncpy(p.callsign, g_call, sizeof(p.callsign) - 1);
        enqueue(BCAST_MAC, &p, sizeof(p));
    }
#endif
    tx_pump();
}
