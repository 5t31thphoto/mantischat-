# MantisNow v1 — as implemented by MantisChat

MantisChat follows the MantisNow v1 build spec. This file records what this
firmware sends and understands, and registers the kind number it adds.

## Kind registry

| Kind | Name | Owner | Spec |
|---|---|---|---|
| `0x01` | TEXT | core | 1–191 bytes, `chanOrSub` = channel 0–15, DM if `dstNode` ≠ all |
| `0x02` | PING | core | empty; answer with PONG |
| `0x03` | PONG | core | `u32 pingPktId`, `u32 pingerNode`; `dst` = pinger |
| `0x04` | POS | core | |
| `0x05` | ACK | core | `u32 ackedPktId` |
| `0x06` | FRAG | core | `u32 msgId, u8 idx, u8 total, u8 innerKind`, chunk ≤ 215 |
| `0x10`/`0x11` | VS / SHR | MantisOS | |
| `0x20`/`0x21` | KID_PING / KID_ACK | MantisOS | |
| `0x30` | DANCE | MantisOS | |
| **`0x40`** | **WB** | **MantisChat** | **Whiteboard — see WHITEBOARD.md** |

## MantisChat behaviour
- **Radio:** `WiFi.mode(WIFI_STA)`, `esp_wifi_set_promiscuous(true)`,
  `esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE)`, `esp_wifi_set_promiscuous(false)`,
  `esp_now_init()`, broadcast peer, no encryption. MantisChat is a dedicated
  firmware, so the radio stays up for the whole session (chat arrives while you
  draw and vice versa).
- **nodeId:** `ESP.getEfuseMac() & 0xFFFFFFFF`.
- **Callsign:** up to 8 bytes, null-padded.
- **Sends:** TEXT on channel 0 (or DM to a peer's node), PING every 10 s, PONG to
  every PING, ACK when asked, WB (`0x40`). `seq` +1 per frame, `pktId` random, `ttl` 0.
- **Receive rules:** drop `len < 28`, bad magic/version, own `srcNode`; dedup
  `(srcNode, pktId)` over the last 32 before dispatch; reassemble FRAG (4 streams,
  16 fragments, 2 s timeout); ACK if flags bit1 and `dstNode == self`; unknown kinds
  dropped. Every valid frame marks its sender as present under People.
- TEXT on channels other than 0 is shown tagged `[chN]`. TEXT addressed to
  another node is ignored.
- Text input is capped at 178 characters so a message also fits the legacy format.

## Legacy compatibility (MantisOS ≤ 1.5.8, `MNW1`)
`MC_LEGACY_MNW1` (default `1`, in `mc_proto.h`) keeps older watches working:
MantisChat receives 197-byte `MNW1` frames (beacon/group/DM), beacons `MNW1`
every 5 s, and echoes each group message as `MNW1`. A DM to a peer heard over
`MNW1` is also sent as a unicast `MNW1` DM. Set it to `0` once every watch runs
MantisNow v1. Old watches drop v1 frames (wrong magic), and v1 devices drop
`MNW1` frames (byte 1 is `'N'`, not version 1), so the two coexist cleanly.
