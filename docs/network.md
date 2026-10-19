# Network design

The original game played over AppleTalk: DDP datagrams for traffic and NBP
for discovery. The port keeps the game's own protocol code unchanged and only
replaces the transport layer (the `.MPP` driver calls) with UDP.

## Roles

| Original role | Menu item | What it does | UDP equivalent |
|---|---|---|---|
| **Joiner** | Net Join | Registers an NBP name, then waits for a host | Binds a UDP port and waits |
| **Host** | Net Host | Looks up NBP names, picks a joiner, simulates the whole game | Connects to the joiner's IP:port |

Note that, at the transport level, the in-game "host" is the side that
initiates. The joiner listens, so it is the one that needs a reachable
(port-forwarded) UDP port. This is the reverse of most modern games.

## Mapping of AppleTalk calls (`.MPP`, `ioRefNum` -10)

| csCode | Call | HLE behaviour |
|---|---|---|
| 248 | POpenSkt | Open a UDP socket. Always return DDP socket `0x80`, so the socket check on received packets passes on both sides. |
| 247 | PCloseSkt | Close the UDP socket. |
| 246 | PWriteDDP | Gather the write-data structure and send one UDP datagram with the payload and DDP type. Complete synchronously: set `ioResult=0` and call the completion flag. |
| 253 | PRegisterName | Joiner: start listening and answer discovery probes. |
| 251 | PLookupName | Host: probe LAN broadcast, the `--connect` address, or the lobby. Write NBP tuples into the lookup buffer and bump `numGotten`. |
| 250 | PConfirmName | Always succeeds. |
| 252 / 254 | PRemoveName / PKillNBP | Stop advertising / cancel the lookup. |

## Delivering a received packet

1. Write the packet into the listener's buffer:
   - receive header at `A5-11376`:
     - byte 0 = my node
     - byte 1 = peer node
     - byte 2 = 2
     - bytes 3..4 = 13 + payload length
     - byte 15 = `0x80` (peer socket)
     - byte 16 = DDP type
   - payload at `A5-11359` (at most 587 bytes)
2. Call `ProcessDDPPacket` (seg 3 +$1284) from a safe poll point, such as
   `TickCount` or the event calls.

DDP types:

- `$E1`: address exchange (host to joiner)
- `$E2`: confirmation (joiner to host)
- `$AA`: game data

## Internet hardening (done in the shim, game logic untouched)

- **Jitter buffer.** On the joiner, queue `$AA` packets by sequence number
  and deliver at most one per game frame (2 ticks). The game double-buffers
  sprite updates and overwrites any frame that has not been consumed yet.
- **Registered-event dedup.** The receiver runs a registered entry every time
  it sees it, and the sender repeats it until it gets a receipt. The shim
  remembers ids it has already seen, strips repeats, and sends a receipt
  right away.
- **Redundancy.** Each `$AA` datagram is sent twice. The game drops
  duplicates via `seq <= last`.

## Wire format (UDP)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `"BONK"` magic |
| 4 | 1 | version |
| 5 | 1 | kind (0 = DDP, 1 = discovery probe, 2 = discovery reply) |
| 6 | 1 | DDP type |
| 7 | 1 | reserved |
| 8 | n | payload |

Default port: 24731.
