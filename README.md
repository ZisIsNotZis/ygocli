# ygocli

**A minimal, text-first Yu-Gi-Oh! duel client powered by `ocgcore`** — fast to run, easy to instrument, and useful for debugging engine behavior and collecting duel traces.

## 30-second snapshot

- **What it is:** single-binary CLI duel runner (`./ygocli <deck0.ydk> <deck1.ydk> [--auto] [--random]`)
- **What works now:** deck loading, duel bootstrap, state display, many `MSG_*` handlers, manual + `--auto` play paths
- **What is rough:** response encoding edge cases, incomplete protocol coverage, long-run stability
- **What this is great for:** quick local experiments, parser/debug work, and generating logs for analysis

## Current status

| Area | Status |
| --- | --- |
| Build | ✅ `make` |
| Basic duel startup | ✅ |
| Auto-play progression | ✅ (multiple turns observed) |
| Full battle reliability | ⚠️ Not reliable in all scenarios |
| Protocol coverage | ⚠️ Broad but not complete |
| Long-run stability | ❌ Known retry-loop failures in some games |

### Evidence from `example.log`

- Reaches normal gameplay (`DRAW`, `NEW_TURN`, `NEW_PHASE`, `SELECT_*`, `MOVE`, `SET`, etc.).
- Contains **5** `MSG_UNKNOWN` events (`unhandled message type=0`).
- Contains **111,890** `MSG_RETRY` events and no clean finish marker (`MSG_WIN` / `PROCESSOR_END`) — indicates a retry-loop failure mode.

## Quick start

### Ubuntu packages (recommended)

```bash
# dev packages
sudo apt install -y \
  freeglut3-dev \
  libfreetype-dev \
  libirrlicht-dev \
  libjpeg-dev \
  liblzma-dev \
  liblua5.3-dev \
  libminiaudio-dev \
  libsqlite3-dev

# handy agent/debug tool
sudo apt install -y sqlite3

# minimal mono libs (for WindBot)
sudo apt install -y \
  libmono-system-data4.0-cil \
  libmono-system-runtime-serialization4.0-cil
```

```bash
# build
make

# run (manual)
./ygocli deck/260124DD/b7fb7204fab3c94d.ydk deck/260124DD/b7fb7204fab3c94d.ydk

# run (auto decisions)
./ygocli deck/260124DD/b7fb7204fab3c94d.ydk deck/260124DD/b7fb7204fab3c94d.ydk --auto

# run (randomized auto decisions)
./ygocli deck/260124DD/b7fb7204fab3c94d.ydk deck/260124DD/b7fb7204fab3c94d.ydk --random

# targeted regression
make test-regression

# randomized fuzz runs (random decks + random choices)
make test-fuzz-random
```

## What’s done

- Makefile-based build flow for this repo (`make`).
- `gframe` dependency removed for `ygocli` build path.
- Message-driven duel loop with game-state printing and interactive decision points.
- Support for loading `.ydk` decks (`#main`, `#extra`, stop at `!side`).
- Added deterministic seed support via `YGOCLI_SEED` for reproducible runs/tests.
- Added random-choice mode via `--random` or `YGOCLI_RANDOM_CHOICES=1` (`YGOCLI_CHOICE_SEED` for reproducibility).
- Added automated regression + mass autoplay + random-choice fuzz scripts under `tests/`.

## What’s not done (yet)

- Full protocol-complete handling for all engine message variants.
- Strong guardrails around invalid/ambiguous response encoding.
- Deterministic regression suite for tricky duel branches.
- Clean handling of all retry scenarios without loops.

## Known bugs / pain points

- Retry storms can occur (`MSG_RETRY` loop) and prevent match completion.
- Some flows still hit unknown message handling (`MSG_UNKNOWN` type `0`).
- Certain selections can produce invalid-looking response values and trigger retries.
- Debug output is verbose by design; logs grow quickly.

## Decks and data

- Local decks live under `deck/` (not intended to be committed in full).
- This repo can run any `.ydk` pair you place there.

## Sibling project: `ygoskill`

- Repo: <https://github.com/zisisnotzis/ygoskill>  
- Local path: `~/.agents/skills/ygoskill/`
- Purpose: skill/tooling around Yu-Gi-Oh workflows and analysis.
- Especially useful dataset asset: `ygopro/tools/deck.7z` (**145,976 decks**), handy for large-scale deck analysis and dataset-driven experiments.

## Contributing

I’m busy; **feel free to fork and open PRs directly**.

- Bugfix PRs with reproducible decks/logs are highly appreciated.
- If you touch message parsing/response encoding, include before/after log snippets.

See [CONTRIBUTING.md](CONTRIBUTING.md) for a short workflow.

## License

GPL (see [LICENSE](LICENSE)).
