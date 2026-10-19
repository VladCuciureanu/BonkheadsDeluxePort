# Network design

The original game played over AppleTalk. It sent DDP datagrams for traffic
and used NBP (Name Binding Protocol) for discovery. The port leaves the
game's own protocol code unchanged and replaces only the transport layer,
the calls into the `.MPP` driver, with UDP. All of it is in
`src/toolbox/net.c`.

## Roles

| Menu item | Game's role | UDP behaviour |
|---|---|---|
| **NET HOST** | Looks up NBP names, lets the player pick a joiner, then runs the whole simulation. | Binds the well-known port (24731, or `--port N`). Shows a banner with its LAN address and the port to forward. Waits for announcements. |
| **NET JOIN** | Registers an NBP name and waits for a host to pick it. | Asks for the host's address, which is remembered in `network.cfg`; `--connect` skips the prompt. Binds any free port. Sends an *announce* datagram every half second to the host address, to each `--connect` address, and to the LAN broadcast address on the well-known port, until a host replies. |

The game's own roles are unchanged; only the direction of first contact is
flipped. The joiner announces itself and the host learns the joiner's
address, including any NAT mapping, from that traffic. Only the host needs a
reachable port.

Each instance picks a random node number from 2 to 251 at startup. The
number is written to the game's AppleTalk globals and to each datagram, and
it doubles as the peer id. Two instances that pick the same number can't
talk to each other; restart one of them. The odds are about 1 in 250.

## Mapping of AppleTalk calls (`.MPP`, `ioRefNum` -10)

| csCode | Call | What the port does |
|---|---|---|
| 248 | POpenSkt | Always returns DDP socket `0x80`, so the socket check on received packets passes on both sides. The UDP socket itself is opened once the role is known. |
| 247 | PCloseSkt | Closes the UDP socket and forgets all peers. |
| 246 | PWriteDDP | Gathers the write-data structure. Sends one UDP datagram, or two for game data (see Redundancy), to the peer that owns the destination node. |
| 253 | PRegisterName | Joiner. Shows the address prompt unless `--connect` was given, binds a socket and starts announcing. |
| 251 | PLookupName | Host. Binds the well-known port. Every announcing joiner becomes an NBP tuple in the lookup buffer, and `numGotten` goes up. The call stays in progress until it is killed. |
| 250 | PConfirmName | Always succeeds. |
| 252 | PRemoveName | Stops announcing. |
| 254 | PKillNBP | Ends the outstanding lookup. |

All calls complete at once. A completion routine runs only when the game
made the call asynchronously.

## Delivering a received packet

Packets are queued as they arrive and delivered from safe poll points, such
as `TickCount` and the event calls, never while game code is running.
Delivery has two steps:

1. Write the packet where the game's own listener would have put it:
   - the receive header at `A5-11376`:

     | Byte | Value |
     |---|---|
     | 0 | my node |
     | 1 | the peer's node |
     | 2 | 2 |
     | 3–4 | 13 + payload length |
     | 15 | `0x80` (peer socket) |
     | 16 | DDP type |

   - the payload at `A5-11359`, at most 587 bytes
2. Call the game's `ProcessDDPPacket` routine, with registers and flags
   saved around the call.

DDP types:

| Type | Meaning |
|---|---|
| `$E1` | Address exchange, host to joiner |
| `$E2` | Confirmation, joiner to host |
| `$AA` | Game data, numbered with a 32-bit sequence in the first 4 bytes of the payload |

## Internet hardening

All of this happens in the transport layer; the game logic is untouched.

- **Ordering and duplicates.** `$AA` packets are kept sorted by sequence
  number. Packets at or below the last delivered sequence, and repeats
  already in the queue, are dropped.
- **Pacing (`--hold N`, default 8 ticks).** The game double-buffers incoming
  sprite state in two receive slots at `A5-32322`, and overwrites a slot it
  has not consumed yet. A `$AA` packet is therefore delivered only after the
  game has marked the slot it would write into as consumed (word +582 of the
  slot), or once the packet has waited N ticks. At most one game packet is
  delivered per poll. Control packets go through at once.
- **Redundancy.** Each `$AA` datagram is sent twice to absorb packet loss.
  The receiver's sequence check drops the copy. Disable it with
  `--no-redundancy`.
- **Registered-event dedup (`--dedup`, off by default).** The game repeats
  important one-off ("registered") events until the other side sends a
  receipt, and the receiver acts on every copy. This option strips copies it
  has already seen and sends the receipt right away. It is off by default
  because the game's start-of-level handshake relies on seeing the repeats.

`--raw-net` turns off pacing, redundancy and dedup, which gives the original
behaviour.

For testing, `--sim-latency MS` delays every outgoing datagram (with ±20%
jitter) and `--sim-loss PCT` drops that share of them. A two-instance game
on one machine played smoothly at 120 ms round trip with 5% loss.

## Wire format (UDP)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `"BONK"` magic |
| 4 | 1 | protocol version (1) |
| 5 | 1 | kind |
| 6 | 1 | DDP type (kind 0 only) |
| 7 | 1 | sender's node number |
| 8 | n | payload |

Kinds:

| Kind | Meaning |
|---|---|
| 0 | A DDP datagram. The payload is the game's DDP payload. |
| 1 | A probe. A joiner that receives it answers with an announce. Kept for compatibility; the current code doesn't send probes. |
| 2 | An announce, sent by the joiner. The payload is the NBP object and type as two Pascal strings. The object is the player name the host shows in its list. |

Datagrams with the wrong magic or version, and the sender's own broadcasts,
are ignored. The default port is 24731.

## Limitations

- **No NAT traversal or relay.** The host must be reachable on its UDP
  port. A VPN that puts both players on one virtual LAN works as a
  substitute.
- **No disconnect detection.** If one side quits, the other side keeps
  waiting.
- **Two players only.** This matches the original game.
- **No authentication or encryption.** The protocol is the original game's
  and trusts every datagram that carries the right magic.
