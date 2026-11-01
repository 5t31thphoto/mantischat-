# MantisChat Whiteboard — MantisNow v1 kind `0x40`

Live community whiteboard shared by every MantisChat in radio range.
Rides the standard MantisNow v1 frame (28-byte header, channel 1, broadcast,
`dstNode = 0xFFFFFFFF`). Sub-kind goes in `chanOrSub`. All fields little-endian.
Devices that don't know kind `0x40` drop it silently, per the v1 receive rules.

## Board space
- Canvas is **320 × 206** px (Core2 screen minus a 34 px toolbar). All coordinates
  are in this space. Receivers clamp to `-32 … W/H+32`.
- Colours are RGB565. Width is pen diameter in px, clamped to 1–32.

## Stroke identity and ownership
- A stroke is identified by **(owner, sid)**. The owner is the frame's sender
  (ESP-NOW source MAC / `srcNode`) — never a field inside the payload.
- `sid` (u32) = `(random 16-bit boot session << 16) | counter`. The session keeps
  post-reboot strokes from colliding with pre-reboot ones.
- **A receiver applies DEL and CLEAR only to strokes owned by that frame's
  sender.** You can remove your own ink from every board; nobody can remove anyone
  else's. This is enforced on the receiving side.

## Sub-kinds (`chanOrSub`)

| Sub | Name    | Payload | Meaning |
|-----|---------|---------|---------|
| 0   | HELLO   | empty | "I'm on the board" — sent on entering and every 4 s while there |
| 1   | SEG     | see below | One polyline chunk of a stroke, sent live while drawing |
| 2   | DEL     | `u32 sid` | Delete my stroke `sid` everywhere |
| 3   | CLEAR   | `u32 nextSid` | Delete **all** my strokes; `nextSid` is a watermark (see below) |
| 4   | SYNC    | empty | "Replay your strokes" — sent on entering the board |

### SEG payload (10 + 4·n bytes, max 202)

| Offset | Size | Field |
|---|---|---|
| 0  | 4 | `sid` |
| 4  | 2 | `seq` — chunk index within the stroke, from 0 |
| 6  | 2 | `color` RGB565 |
| 8  | 1 | `width` px |
| 9  | 1 | `n` points, 1–48 |
| 10 | 4n | `n × (i16 x, i16 y)` |

- A chunk is flushed every 48 points or 45 ms while drawing, and on pen-up.
- Each chunk after the first **starts with the previous chunk's last point**, so
  every chunk renders on its own as a continuous polyline; out-of-order arrival is fine.
- `n = 1` is a dot (filled circle of `width`).
- Receivers dedup on `(owner, sid, seq)`.

## Delivery
- DEL and CLEAR are sent **three times** (0, +150, +600 ms) since broadcast has no ACK.
- Receivers keep a tombstone ring of the last 128 `(owner, sid)` deletes; a late or
  replayed SEG for a tombstoned stroke is ignored.
- CLEAR watermark: after CLEAR from owner X with `nextSid = W`, a receiver ignores
  X's SEGs whose sid is in W's session and older than W. A new boot session is
  never blocked.

## Joining (SYNC)
- On SYNC, each board waits 60–460 ms (random, to spread replies) and then replays
  **only its own** live strokes as ordinary SEGs, paced by its TX queue.
- Ink from a device that has left stays on boards that already had it, but isn't
  passed on to newcomers.

## Local limits (Core2 reference)
- 4000 chunks / 60 000 points in PSRAM. When full, the oldest stroke falls off
  **that board only** — nothing is sent.
