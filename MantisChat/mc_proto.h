// =============================================================================
//  mc_proto.h  --  MantisNow v1 framing + MantisChat whiteboard (kind 0x40)
// =============================================================================
//  Spec: MANTISNOW.md (chat/discovery) and WHITEBOARD.md (kind 0x40) in the
//  repo root. Legacy MantisOS <=1.5.8 "MNW1" chat is still understood and
//  echoed (MC_LEGACY_MNW1) so older watches keep working during the upgrade.
// =============================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifndef MC_LEGACY_MNW1
#define MC_LEGACY_MNW1 1
#endif

static const uint8_t  MESH_CHANNEL = 1;
static const uint32_t NODE_ALL     = 0xFFFFFFFFu;

// ---- MantisNow v1 header (28 bytes, packed, LE) ------------------------------
struct __attribute__((packed)) MnHdr {
    uint8_t  magic;       // 0x4D
    uint8_t  version;     // 0x01
    uint8_t  kind;
    uint8_t  flags;       // bit0 frag, bit1 ack-req
    uint32_t srcNode;
    uint32_t dstNode;     // NODE_ALL = broadcast
    uint32_t pktId;       // random
    uint16_t seq;         // +1 per packet
    uint8_t  ttl;         // 0
    uint8_t  chanOrSub;
    char     callsign[8]; // null-padded
};
static_assert(sizeof(MnHdr) == 28, "MantisNow header is 28 bytes");
static const int MN_MAX_PAYLOAD = 250 - 28;   // 222

enum : uint8_t {
    MN_TEXT = 0x01, MN_PING = 0x02, MN_PONG = 0x03, MN_POS = 0x04, MN_ACK = 0x05,
    MN_FRAG = 0x06, MN_VS = 0x10, MN_SHR = 0x11, MN_KID_PING = 0x20, MN_KID_ACK = 0x21,
    MN_DANCE = 0x30,
    MN_WB = 0x40,         // MantisChat whiteboard (this project) -- see WHITEBOARD.md
};
enum : uint8_t { MNF_FRAG = 0x01, MNF_ACKREQ = 0x02 };
static const int MN_TEXT_MAX = 191;
static const int MC_TEXT_MAX = 178;           // also fits the legacy watch textarea

struct __attribute__((packed)) MnFragHdr { uint32_t msgId; uint8_t idx, total, innerKind; };
static const int MN_FRAG_CHUNK = 215;

// ---- Legacy MantisOS chat (MNW1, 197 bytes) ----------------------------------
struct __attribute__((packed)) MnwPacket {
    char magic[4]; uint8_t type; char callsign[12]; char text[180];
};
static_assert(sizeof(MnwPacket) == 197, "MNW1 must stay 197 bytes");
enum : uint8_t { MNW_BEACON = 0, MNW_GROUP = 1, MNW_DIRECT = 2 };

// ---- Whiteboard (kind 0x40, sub-kind in chanOrSub) ----------------------------
enum : uint8_t { WB_HELLO = 0, WB_SEG = 1, WB_DEL = 2, WB_CLEAR = 3, WB_SYNC = 4 };
static const int MCW_MAX_PTS = 48;
struct __attribute__((packed)) WbSeg {
    uint32_t sid;         // stroke id = (boot session << 16) | counter
    uint16_t seq;         // chunk index within the stroke
    uint16_t color;       // RGB565
    uint8_t  width;       // pen diameter px
    uint8_t  n;           // 1..48 points
    int16_t  pts[MCW_MAX_PTS * 2];
};
static const int WB_SEG_BASE = 10;
static_assert(WB_SEG_BASE + MCW_MAX_PTS * 4 <= MN_MAX_PAYLOAD, "fits one frame");
struct __attribute__((packed)) WbSid { uint32_t sid; };

static const int BOARD_W = 320;
static const int BOARD_H = 206;
