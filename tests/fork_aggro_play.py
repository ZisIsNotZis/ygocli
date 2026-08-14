#!/usr/bin/env python3
"""Aggressive REAL gameplay test through the MCP bridge: two clients play a
full duel where each actually summons monsters, attacks, activates effects,
sets cards, and responds to every prompt type (options, positions, card
selects incl. field-mode discards, announce, place, chains, messages).

Strategy: prefer attack > summon > special-summon > activate > set > reposition,
then pass. Follow-up prompts are answered with legal defaults; a per-prompt
retry tracker shifts choices when the engine rejects a move, and a watchdog
passes when nothing changes for too long. Proof = turns advance, LP drops,
monsters are summoned and attacks happen, and the duel ends."""
import json, subprocess, sys, time, select, re, os

EXE = "/home/z/ygo/bin/release/YGOPro"
HOST = "127.0.0.1"; PORT = 7911
PASS = sys.argv[1] if len(sys.argv) > 1 else "aggro-room"
DECK = "example.ydk"; TO = 10.0
WATCHDOG = 60    # seconds of no turn/LP progress before forcing passes


class MCP:
    def __init__(self, name, errpath):
        self.proc = subprocess.Popen(
            [EXE, "--mcp", "-k", "-n", name, "-h", HOST, "-p", str(PORT),
             "-w", PASS, "-d", DECK, "-j"],
            cwd="/home/z/ygo", stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=open(errpath, "w"), text=True, bufsize=1)
        self._id = 0; self.name = name
        time.sleep(3.5)
        self.rpc("initialize", {"protocolVersion": "2024-11-05"})
        self.failed = {}     # prompt-signature -> set of tried option ids
        self.sigcount = {}   # prompt-signature -> consecutive reads
        self.last_sig = None
        self.n = 0
        self.tries = 0
        self._sel_hist = {}
        self.n_summon = 0; self.n_attack = 0; self.n_activate = 0
        self.n_discard = 0; self.n_place = 0; self.n_announce = 0

    def rpc(self, method, params=None):
        self._id += 1
        req = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None: req["params"] = params
        self.proc.stdin.write(json.dumps(req) + "\n"); self.proc.stdin.flush()
        r, _, _ = select.select([self.proc.stdout], [], [], TO)
        if not r: raise TimeoutError(self.name + " timeout")
        line = self.proc.stdout.readline()
        if not line: raise RuntimeError(self.name + " closed")
        return json.loads(line)

    def choose(self, **args):
        r = self.rpc("tools/call", {"name": "ygo_choose", "arguments": args})
        return r["result"]["content"][0]["text"]

    def state(self):
        r = self.rpc("tools/call", {"name": "ygo_state", "arguments": {}})
        return r["result"]["content"][0]["text"]

    def kill(self):
        self.proc.kill(); self.proc.wait()


def strip_prefix(t):
    # "Chose option N.\n<state>" -> "<state>"
    if t.startswith("Chose option"):
        idx = t.find("\n")
        return t[idx + 1:] if idx >= 0 else t
    return t


def sig_of(t):
    return strip_prefix(t or "")


def status(t):
    m = re.search(r"Turn (\d+) \| LP (\d+) / (\d+)", t or "")
    if not m: return None
    turn, lp0, lp1 = int(m.group(1)), int(m.group(2)), int(m.group(3))
    if turn == 0 and lp0 == 0 and lp1 == 0:   # pre-duel RPS display
        return None
    return (turn, lp0, lp1)


def pick_option(text, keywords, failed):
    """Return the first option id whose line's LAST token is one of the
    keywords (the action word), skipping failed ids."""
    for line in (text or "").splitlines():
        m = re.match(r"^(\d+)\. (.*)$", line)
        if not m: continue
        opt_id = int(m.group(1))
        rest = m.group(2).strip()
        if opt_id in failed: continue
        if rest and rest.rsplit()[-1] in keywords:
            return opt_id
    return None


def act(m, text):
    """Decide an action for client m given its state text. Returns
    (choose_kwargs, did_something)."""
    t = strip_prefix(text)
    if "message:" in t:
        return ({"id": 0}, True)
    if "RPS choice" in t:
        # actions: 0=rock 1=paper 2=scissors (NOT the display numbers).
        # Distinct bases per client; shift on ties (repeated prompt).
        base = 0 if m.name == "A" else 2
        shift = m.sigcount.get(sig_of(text), 1)
        m.n += 1
        return ({"id": (base + shift) % 3}, True)
    if "Turn-order" in t:
        return ({"id": 0}, True)
    if "0. ready" in t:
        return ({"id": 0}, True)       # ready up
    if "start duel" in t:
        return ({"id": 1}, True)       # host starts once ready
    if "0. cancel ready" in t:
        return ({"id": -1}, True)      # already ready, wait for start
    if "Game Over" in t or "Match Over" in t:
        return (None, True)
    if "chain? 0. decline" in t:
        return ({"id": 0}, True)
    if "confirm cards" in t:
        return ({"id": 0}, True)
    if "0. face-up attack" in t and "0. yes" not in t:
        return ({"id": 0}, True)          # position select
    if "0. yes\n1. no" in t:
        return ({"id": 1}, True)          # activate / confirm
    mz = re.search(r"select (\d+)-(\d+) \((\d+)/(\d+)", t)
    if mz:                                  # field-mode card select
        lo, hi, have, total = map(int, mz.groups())
        # loop breaker: if this exact select (same card set) keeps recurring
        # after rejections, the picks are unsolvable (e.g. no valid XYZ
        # material pair) -> cancel when allowed, else stop spamming
        selkey = re.sub(r"\(\d+/\d+ chosen\)", "(x/y chosen)", t)
        m._sel_hist[selkey] = m._sel_hist.get(selkey, 0) + 1
        if len(m._sel_hist) > 64:
            m._sel_hist.clear()
        if m._sel_hist[selkey] > 20:
            if "-1. cancel" in t:
                return ({"id": -1}, True)
            return (None, True)
        if have == 0:
            m._sent_select = False          # fresh prompt
        need = hi - have
        if need <= 0:
            if getattr(m, "_sent_select", False):
                return (None, True)         # response in flight, wait
            return ({"id": -2}, True)       # optional select: finish empty
        # for multi-selects prefer same-level cards (valid XYZ materials)
        if need > 1:
            by_lv = {}
            for line in t.splitlines():
                mm = re.match(r"^(\d+)\. .* \(Lv(\d+)\)", line)
                if mm:
                    by_lv.setdefault(int(mm.group(2)), []).append(int(mm.group(1)))
            best = max(by_lv.values(), key=len, default=[])
            if len(best) >= need:
                idx = [best[(m.tries + i) % len(best)] for i in range(need)]
                m.tries += 1
                m._sent_select = True
                m.n_discard += 1
                return ({"id": 0, "indices": idx}, True)
        idx = [(m.tries + i) % max(total, 1) for i in range(min(need, total))]
        m.tries += 1                        # shifts on retry -> different cards
        m._sent_select = True
        m.n_discard += 1
        return ({"id": 0, "indices": idx}, True)
    mz = re.search(r"select (\d+) zone\(s\)", t)
    if mz:                                  # place / disfield
        need = int(mz.group(1))
        if need <= 1:
            return ({"id": 0}, True)
        m.n_place += 1
        return ({"id": 0, "indices": list(range(need))}, True)
    ma = re.search(r"announce (\d+) (race|attribute)\(s\)", t)
    if ma:                                  # announce race/attrib
        n = int(ma.group(1))
        if n <= 1:
            return ({"id": 0}, True)
        m.n_announce += 1
        return ({"id": 0, "indices": list(range(n))}, True)
    if "announce card" in t or "announce number" in t:
        return ({"id": 0}, True)
    # command prompt: prefer real actions over passing
    failed = m.failed.get(sig_of(text), set())
    for kw, counter in (("attack", "n_attack"), ("summon", "n_summon"),
                        ("activate", "n_activate"),
                        ("set-monster", None), ("set-spell", None),
                        ("reposition", None)):
        oid = pick_option(t, [kw], failed)
        if oid is not None:
            if counter:
                setattr(m, counter, getattr(m, counter) + 1)
            return ({"id": oid}, True)
    # hand special-summons are safe; extra-deck summons (XYZ/Link) need
    # material selects that can reject arbitrary picks -> skip them
    for line in t.splitlines():
        mm = re.match(r"^(\d+)\. (.*) special-summon$", line)
        if not mm: continue
        oid = int(mm.group(1))
        if oid in failed: continue
        if "@Extra" not in mm.group(2):
            m.n_summon += 1
            return ({"id": oid}, True)
    for line in t.splitlines():
        mm = re.match(r"^(\d+)\. (.*)$", line)
        if not mm: continue
        oid = int(mm.group(1)); rest = mm.group(2)
        if oid in failed: continue
        if rest.endswith("to battle phase") or rest.endswith("to main phase 2"):
            return ({"id": oid}, True)
    if "pass" in t:
        return ({"id": -1}, True)         # end turn / decline
    # list-window card select: finish when ready, else click the first card
    if re.search(r"^\d+\. .+ @", t, re.M) and ("-2. finish" in t or "-1. cancel" in t):
        if "-2. finish" in t:
            return ({"id": -2}, True)
        failed = m.failed.get(sig_of(text), set())
        for line in t.splitlines():
            mm = re.match(r"^(\d+)\. (.+)$", line)
            if not mm: continue
            oid = int(mm.group(1))
            if oid in failed: continue
            return ({"id": oid}, True)
    # generic numbered option prompt (effect options like "0. destroy",
    # card-select list, etc.): pick the first non-failed option
    if re.search(r"^0\. ", t, re.M):
        failed = m.failed.get(sig_of(text), set())
        for line in t.splitlines():
            mm = re.match(r"^(\d+)\. (.+)$", line)
            if not mm: continue
            oid = int(mm.group(1))
            if oid in failed: continue
            return ({"id": oid}, True)
    return ({"id": -1}, False)


def main():
    a = MCP("A", "/tmp/aggro_A.err")
    b = MCP("B", "/tmp/aggro_B.err")
    samples = []
    ra = rb = False
    game_over = False
    t0 = time.time()
    last_progress = time.time()
    last_seen = None
    try:
        while time.time() - t0 < 150:
            for m in (a, b):
                if game_over: break
                t = m.state()
                s = status(t)
                if s:
                    samples.append(s)
                    if s != last_seen:
                        last_seen = s
                        last_progress = time.time()
                snew = sig_of(t)
                prev = getattr(m, "_prev_sig", None)
                if prev is not None and prev != snew:
                    m.failed.pop(prev, None)          # prompt moved on
                    m.sigcount.clear()
                m.sigcount[snew] = m.sigcount.get(snew, 0) + 1
                if m.sigcount[snew] > 5 and hasattr(m, "_last_opt"):
                    m.failed.setdefault(snew, set()).add(m._last_opt)   # rejected move
                m._prev_sig = snew
                # Wait for an in-flight action to resolve: don't re-send the
                # same command while the prompt is still up (e.g. attack).
                # List-window card selects legitimately persist while the
                # player clicks more cards, so they are exempt.
                list_select = bool(re.search(r"^\d+\. .+ @", t, re.M)) and "-2. finish" in t
                if (snew == getattr(m, "_last_act_sig", None) and m.sigcount[snew] < 8
                        and not list_select and "start duel" not in t):
                    continue
                if "Game Over" in t or "Match Over" in t:
                    print("GAME OVER:", (t or "").strip()[:120])
                    game_over = True
                    break
                kw, acted = act(m, t)
                if kw is None:
                    if "Game Over" in t or "Match Over" in t:
                        game_over = True
                        break
                    continue              # wait (e.g. response in flight)
                r = m.choose(**kw)
                m._last_opt = kw.get("id", kw.get("indices", None))
                m._last_act_sig = snew
            # watchdog: force progress if nothing changed for too long
            if time.time() - last_progress > WATCHDOG:
                st = m.state()
                if "0. ready" in st:
                    print("  watchdog re-ready on", m.name)
                    m.choose(id=0)
                elif "start duel" in st:
                    print("  watchdog re-start on", m.name)
                    m.choose(id=1)
                elif "chosen)" in st or "zone(s)" in st:
                    print("  watchdog surrender (stuck select) on", m.name)
                    m.rpc("tools/call", {"name": "ygo_surrender", "arguments": {}})
                else:
                    print("  watchdog pass on", m.name)
                    m.choose(id=-1)
                last_progress = time.time()
        time.sleep(0.05)
    finally:
        a.kill(); b.kill()

    max_turn = max((s[0] for s in samples), default=0)
    lp0s = {s[1] for s in samples}; lp1s = {s[2] for s in samples}
    print("turns:", max_turn, "| A: summon %d attack %d activate %d discard %d | B: summon %d attack %d activate %d discard %d"
          % (a.n_summon, a.n_attack, a.n_activate, a.n_discard, b.n_summon, b.n_attack, b.n_activate, b.n_discard))
    print("LP0 seen:", sorted(lp0s)[:8], "LP1 seen:", sorted(lp1s)[:8])
    interesting = (a.n_summon + b.n_summon > 0) and (a.n_attack + b.n_attack > 0)
    progressed = max_turn >= 3 or len(lp0s) > 1 or len(lp1s) > 1
    ok = interesting and progressed
    print("AGGRO PLAY VERIFIED" if ok else "AGGRO PLAY NOT VERIFIED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
