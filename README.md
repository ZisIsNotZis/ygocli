# ygocli

**English | [中文](README.zh.md)**

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

## UI glance

```text
============================================================
GAME STATE - Turn: 1 - Phase: Main1
============================================================
=== Player 0 - LP: 8000
  Deck: 36 cards
  Extra: 15 cards
  Hand:
    [0] 幽鬼兔 (怪兽|效果|调整 3 0/1800 这个卡名的效果1回合只能使用1次。 ①：场上的怪兽的效果发动时或者场上的已是表侧表示存在的魔法·陷阱卡的效果发动时，把手卡·场上的这张卡送去墓地才能发动。场上的那张卡破坏。)
    [1] 幽鬼兔 (怪兽|效果|调整 3 0/1800 这个卡名的效果1回合只能使用1次。 ①：场上的怪兽的效果发动时或者场上的已是表侧表示存在的魔法·陷阱卡的效果发动时，把手卡·场上的这张卡送去墓地才能发动。场上的那张卡破坏。)
    [2] 救祓少女连祷 (陷阱 0 0/0 这个卡名的卡在1回合只能发动1张。 ①：自己场上的怪兽只有「救祓少女」怪兽的场合，支付800基本分，以对方的场上·墓地1张卡为对象才能发动。那张卡除外。那之后，可以从以下效果选1个适用。 ●进行1只「救祓少女」超量怪兽的超量召唤。 ●这个回合自己是已把怪兽超量召唤的场合，对方场上1张卡除外。)
  Grave:
    [0] 欢聚友伴·茸茸长尾山雀 (怪兽|效果 4 100/600 这张卡的效果发动的回合，自己只能有1次把这张卡以外的「欢聚友伴」怪兽的效果发动。 ①：自己·对方回合，自己场上没有卡存在的场合，把这张卡从手卡丢弃才能发动。这个回合中，以下效果适用。 ●每次对方从卡组·额外卡组把怪兽特殊召唤，自己抽1张。 ●结束阶段，自己手卡比对方场上的卡数量＋6张要多的场合，那个相差数量的自己手卡随机回到卡组。)
  Removed:
  MZone:
    [0] 荒魂 (怪兽|效果|灵魂 4 800/1800 这张卡不能特殊召唤。 ①：这张卡召唤·反转时才能发动。从卡组把「荒魂」以外的1只灵魂怪兽加入手卡。 ②：这张卡召唤·反转的回合的结束阶段发动。这张卡回到手卡。) FA
    [1] 幸魂 (怪兽|效果|灵魂 4 400/900 这张卡不能特殊召唤。这个卡名的①③的效果1回合各能使用1次。 ①：把手卡的这张卡给对方观看才能发动。进行手卡1只灵魂怪兽的召唤。 ②：这张卡召唤·反转的回合的结束阶段发动。这张卡回到手卡。 ③：这张卡被解放的场合，以自己墓地1只灵魂怪兽为对象发动。那只怪兽加入手卡。) FA
  SZone:
=== Player 1 - LP: 8000
  Deck: 37 cards
  Extra: 15 cards
  Hand:
    [0] 抹杀之指名者 (魔法|速攻 0 0/0 这个卡名的卡在1回合只能发动1张。 ①：宣言1个卡名才能发动。宣言的1张卡从卡组除外。这个回合中，这个效果除外的卡以及原本卡名和那张卡相同的卡的效果无效化。)
    [1] 救祓少女圣母悼歌 (魔法|永续 0 0/0 这个卡名的①②③的效果1回合各能使用1次。 ①：作为这张卡的发动时的效果处理，以下效果适用。 ●从卡组把2只「救祓少女」怪兽加入手卡。那之后，选自己1张手卡丢弃。 ②：以自己场上1只表侧表示怪兽为对象才能发动。进行1只在那只怪兽有卡名记述的「救祓少女」怪兽的召唤。 ③：怪兽被送去对方墓地的场合才能发动（伤害步骤也能发动）。对方把那之内的1只除外。)
    [2] 屋敷童 (怪兽|效果|调整 3 0/1800 这个卡名的效果1回合只能使用1次。 ①：包含以下其中任意种效果的魔法·陷阱·怪兽的效果发动时，把这张卡从手卡丢弃才能发动。那个发动无效。 ●从墓地把卡加入手卡·卡组·额外卡组的效果 ●从墓地把怪兽特殊召唤的效果 ●从墓地把卡除外的效果)
  Grave:
    [0] 欢聚友伴·茸茸长尾山雀 (怪兽|效果 4 100/600 这张卡的效果发动的回合，自己只能有1次把这张卡以外的「欢聚友伴」怪兽的效果发动。 ①：自己·对方回合，自己场上没有卡存在的场合，把这张卡从手卡丢弃才能发动。这个回合中，以下效果适用。 ●每次对方从卡组·额外卡组把怪兽特殊召唤，自己抽1张。 ●结束阶段，自己手卡比对方场上的卡数量＋6张要多的场合，那个相差数量的自己手卡随机回到卡组。)
    [1] 小丑与锁鸟 (怪兽|效果 1 0/0 ①：自己·对方回合，对方在抽卡阶段以外从卡组把卡加入手卡的场合，把这张卡从手卡送去墓地才能发动。这个回合，双方不能从卡组把卡加入手卡。)
  Removed:
  MZone:
  SZone:
============================================================

  [0] SP Summon 救祓少女·卡斯皮特尔
  [1] SP Summon 大薰风骑士 翠玉
  [2] SP Summon 救祓少女·卡斯皮特尔
  [3] SP Summon 救祓少女·阿索菲勒
  [4] SP Summon 救祓少女·米迦埃莉丝
  [5] SP Summon 救祓少女·米迦埃莉丝
  [6] SP Summon 救祓少女·米迦埃莉丝
  [7] SP Summon 励辉士 入魔蝇王
  [8] SP Summon 救祓少女·吉卜利娜
  [9] SSet 救祓少女连祷
  [10] Go to End Phase

Your choice (0-10):
```

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
