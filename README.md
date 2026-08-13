# ygocli

**English | [中文](README.zh.md)**

**A text-first Yu-Gi-Oh! duel client + server powered by `ocgcore`** — fast, scriptable, and MCP-friendly. Think of ygocli as the next-gen client: it speaks the gframe (ygopro) network protocol, so it can connect to existing ygopro/gframe servers. The reverse (a stock gframe client talking to the ygocli server) is best-effort byte-exact and may not be fully supported yet.

## Build & run

```
make
./ygocli <deck0.ydk> <deck1.ydk> [--auto]   # solo duel in-process (god view)
./ygocli server [--port N] [--bind ip]       # standalone duel server
./ygocli mcp                                 # MCP server (JSON-RPC over stdio)
./ygocli interact                            # interactive mode (same engine as MCP)
./ygocli                                     # usage help
```

Data folders (relative to the binary): `cards.cdb`, `strings.conf`, `script/`, `wiki/`, `single/`, `replay/`.

## MCP tools

| Tool | Purpose |
| --- | --- |
| `ygo_solo` | Solo duel in-process, no server. Both players driven from this session. Respond with `ygo_choose`. |
| `ygo_client` | Connect to a server as a player: join the room, upload your deck, become Ready. The host auto-starts once both players are in. `mode=1` (default) plays a best-of-3 match with side-deck exchange; `mode=0` is a single game. Returns an error and closes the connection on failure (room full, deck rejected, version mismatch, game already started). |
| `ygo_choose` | Choose an option (`id`, or `indices` for multi-select) from the pending prompt. |
| `ygo_card` | Search the card database by filters (AND across params). |
| `ygo_wiki` | Grep the bundled `wiki/*.md` concept files. |
| `ygo_exit` | Close the current network connection (leave the room). |
| `ygo_surrender` | Surrender the current game. Unlike `ygo_exit`, the connection stays up: in a match the opponent wins the game, both players exchange side decks, and the next game starts. |
| `ygo_observe` | Connect as an observer: sees both players' public information, never prompted. |
| `ygo_replay` | Play back a saved `.yrp` replay (narration + final state). Replays are auto-saved by the server to `replay/YYYYMMDD_HHMMSS.yrp`. |
| `ygo_server` / `ygo_server_exit` | Launch / stop a `ygocli server` child process. |
| `ygo_windbot` / `ygo_windbot_exit` | Launch / stop a WindBot AI client (`mono WindBot/WindBot.exe` — not bundled; errors until placed in `WindBot/`). |
| `ygo_puzzle` | Play a puzzle from `single/`: `single/<puzzle>/{deck0.ydk,deck1.ydk,setup.lua}`. The optional `setup.lua` runs in the duel's lua context before the game starts (e.g. `Duel.SetLP(1, 100)`). |

### Network protocol notes

- gframe framing (byte-exact vendored `net/network.h`), version `0x1362`.
- Rooms support an optional password (set by the host on create, must match on join).
- Best-of-3 matches: after a game, the loser picks who goes first, players exchange side decks (`STOC_CHANGE_SIDE` → `CTOS_UPDATE_DECK`), and the next game starts automatically. `ygo_surrender` keeps the connection alive through this flow.
- Observers connect during room formation (seats 2-7), see the public view of all game messages, and are never asked for choices.
- Replays (`.yrp`): ygopro format — magic `ygopro` + version `0x12d0` + seed[8], then `[u32 len][bytes]` chunks of the engine message stream. Auto-saved per game by the server; the gframe GUI can play them back (best-effort).
- Per-player filtering: hands and face-down cards are never sent to the opponent (or observer).

### Compatibility (current)

- **ygocli client → ygopro/gframe server:** the goal; the client implements the gframe join/ready/side flow and the core duel messages. Edge messages may be skipped (best-effort).
- **gframe client → ygocli server:** the server implements the same protocol and is byte-exact where it matters (join, deck, RPS/TP, game messages). Verify per-feature.

## Data folders

- `wiki/*.md` — concept files searched by `ygo_wiki` (phases, chains, combat, mechanics).
- `single/<puzzle>/` — puzzles for `ygo_puzzle` (deck0.ydk, deck1.ydk, optional setup.lua).
- `replay/` — auto-saved `.yrp` replays.

## Development notes

- `YGOCLI_MCP_DEBUG=1` enables message-level debug logs (`srv msg 0x%02x`, client packet hexdumps, loop-exit state).
- Solo regression: `./ygocli <deck0> <deck1> --auto` must reach `MSG_WIN` (no `MSG_RETRY` storm).
- LAN regression: two `ygo_client` sessions vs `ygocli server` must play to a legit winner.

---

## FUTURE (not implemented — placeholders with design notes)

### `ygocli web`

A web UI that follows the exact same logic as MCP: same tools, same engine — just clickable, with the field visualized like ocgcore/gframe. This is still a chat/streamlit-style UI, not a traditional game UI: history scrolls up, card names are replaced with real card images, and the coarse text field is replaced with an HTML/JS drawing. All fundamental logic fully shares with MCP; no traditional YGO UI is planned.

### `ygo_snapshot`

Snapshot the current game state into a `ygo_puzzle` format: export decks, LP, hand/field/grave/removed contents per player, turn/phase, and the win condition so `ygo_puzzle` can reload it. Hard parts: expressing arbitrary state (counters, markers, chain state, xyz materials, pending effects) in a script-based puzzle format. The puzzle loader (`mcp_init_duel_setup`) already runs `setup.lua` in the duel's lua context, so a snapshot can be emitted as a `setup.lua` + two decks. Where the ocgcore lua API lacks the needed helpers, the plan is to fork ocgcore and add new `Duel.*` functions so arbitrary states become expressible. Not exposed in the tool list until implemented.
