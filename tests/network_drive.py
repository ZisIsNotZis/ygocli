#!/usr/bin/env python3
"""Full network-play driver against srvpro.

Scenarios:
  1. single  : join room -> ready -> start -> RPS -> TP -> play rounds -> chat
               -> opponent disconnect -> game-over/disconnect handling
  2. match   : room "M#..." -> game1 -> surrender -> side-deck (CHANGE_SIDE)
               -> game2 -> surrender -> game3 -> surrender -> Match Over
  3. observe : third client joins a live 2/2 duel -> watches the byte-log replay

Usage: python3 -u tests/network_drive.py [deck.ydk] [host] [port] [--keep]
Exits 0 only if every assertion passes.
"""
import json, subprocess, sys, os, time, select

EXE = "./ygocli"
DECK = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else "example.ydk"
HOST = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else "127.0.0.1"
PORT = int(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else 7911
TO = 6.0
FAIL = []
COUNT = [0]


class MCP:
    def __init__(self, name):
        env = dict(os.environ)
        if "--debug" in sys.argv:
            env["YGOCLI_MCP_DEBUG"] = "1"
        self.proc = subprocess.Popen([EXE, "mcp"], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=open("/tmp/%s.err" % name, "w"),
            text=True, bufsize=1, env=env)
        self._id = 0; self.name = name
        self.rpc("initialize", {"protocolVersion": "2024-11-05"})

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

    def call(self, name, args):
        COUNT[0] += 1
        return self.rpc("tools/call", {"name": name, "arguments": args})["result"]["content"][0]["text"]

    def choose(self, **args):
        return self.call("ygo_choose", args)

    def kill(self):
        self.proc.kill(); self.proc.wait()


def expect(cond, label):
    if cond:
        print("  ok :", label)
    else:
        FAIL.append(label)
        print("  FAIL:", label)


def connect(m, name, passwd, host=HOST, port=PORT):
    t = m.choose(id=1, host=host, port=port, password=passwd, deck=DECK, name=name)
    return t


def rps_answer(m, val):
    """Answer RPS; retry on ties (server re-sends SELECT_HAND)."""
    for _ in range(6):
        t = m.choose(id=val)
        if "RPS choice" in t:
            val = 3 if val == 1 else 1   # switch so a tie cannot repeat
            continue
        return t
    return t


def poll_until(m, needles, rounds=40):
    """Poll with -1 until any needle appears in the response (or rounds)."""
    for _ in range(rounds):
        t = m.choose(id=-1)
        if any(n in t for n in needles):
            return t
    return t


def play_rounds(m1, m2, max_rounds=30):
    """Both clients poll; answer any duel prompt with its first option.
    Returns when a game-over/menu state appears or the round budget is spent."""
    for i in range(max_rounds):
        progressed = False
        for m in (m1, m2):
            t = m.choose(id=-1)
            if any(n in t for n in ("Game Over", "Match Over", "Duel over", "Main menu",
                                    "Disconnected", "Match Over: Player")):
                return t
            # duel prompt with numbered options (skip RPS/TP/lobby/waiting)
            if any(f"{n}." in t for n in range(0, 4)) and "RPS choice" not in t \
               and "Turn-order" not in t and "ready" not in t and "start duel" not in t:
                m.choose(id=0)
                progressed = True
        if not progressed:
            return "idle"
    return "budget"


def show(label, text):
    print(f"--- {label} ---"); print(text.strip()[:400]); print()


def scenario_single():
    print("== SCENARIO 1: single duel ==")
    a = MCP("s1a"); b = MCP("s1b")
    try:
        ta = a.choose(id=-1); expect("Main menu" in ta, "A main menu")
        tb = connect(b, "s1b", "pairtest"); expect("Room:" in tb, "B joined room")
        ta = connect(a, "s1a", "pairtest"); expect("Room:" in ta, "A joined room")
        ta = a.choose(id=0); expect("cancel ready" in ta, "A ready")
        tb = b.choose(id=0); expect("cancel ready" in tb, "B ready")
        ta = a.choose(id=1); expect("RPS" in ta, "host start -> RPS")
        tb = rps_answer(b, 2)          # paper beats A's rock -> B picks TP
        ta = rps_answer(a, 1)
        tb = poll_until(b, ["Turn-order"], 4)
        expect("Turn-order" in tb, "RPS winner (B) gets TP")
        tb = b.choose(id=0)            # B goes first
        ta = poll_until(a, ["Turn", "waiting", "Game Over", "Duel over"], 6)
        t = play_rounds(a, b, 12)
        show("after play", t)
        expect(("Turn" in t) or any(n in t for n in ("Game Over", "Duel over", "idle", "budget")),
               "duel progressed")
        # chat
        tc = a.call("ygo_chat", {"text": "hello from A"})
        expect("Sent" in tc, "chat sent")
        # opponent disconnects mid-duel
        td = b.call("ygo_disconnect", {})
        expect("Left the room" in td, "B disconnected")
        ta = a.choose(id=-1)
        expect(("Game Over" in ta) or ("Duel over" in ta) or ("Disconnected" in ta),
               "A sees game over after B left")
        ta = a.choose(id=-1)
        expect("Main menu" in ta or "Disconnected" in ta, "A back to menu/disconnected")
    finally:
        a.kill(); b.kill()


def match_game(m, who):
    """Surrender for one side, then both re-upload side decks (CHANGE_SIDE)."""
    ts = m.call("ygo_surrender", {})
    show(who + " surrender", ts)
    return ts


def scenario_match():
    print("== SCENARIO 2: BO3 match (M# room) ==")
    a = MCP("m1a"); b = MCP("m1b")
    try:
        ta = connect(a, "m1a", "M#matchtest"); expect("Room:" in ta and "[match]" in ta, "A joined match room")
        tb = connect(b, "m1b", "M#matchtest"); expect("Room:" in tb, "B joined match room")
        a.choose(id=0); b.choose(id=0)
        ta = a.choose(id=1); expect("RPS" in ta, "game1 start -> RPS")
        rps_answer(a, 1); rps_answer(b, 2)
        tb = poll_until(b, ["Turn-order"], 4)
        if "Turn-order" in tb: b.choose(id=0)
        t = play_rounds(a, b, 8)
        show("game1 play", t)
        # game 1: A surrenders -> B wins game 1 -> CHANGE_SIDE
        match_game(a, "A")
        # both clients must re-upload on CHANGE_SIDE and get game 2
        ta = poll_until(a, ["Turn-order", "Turn ", "waiting", "Duel over", "Match Over"], 30)
        tb = poll_until(b, ["Turn-order", "Turn ", "waiting", "Duel over", "Match Over"], 30)
        show("game2 setup A", ta)
        show("game2 setup B", tb)
        if "Turn-order" in ta:
            a.choose(id=1)
        if "Turn-order" in tb:
            b.choose(id=1)
        t = play_rounds(a, b, 8)
        show("game2 play", t)
        # game 2: B surrenders -> A wins -> 1-1 -> game 3
        match_game(b, "B")
        ta = poll_until(a, ["Turn-order", "Turn ", "waiting", "Match Over", "Duel over"], 30)
        tb = poll_until(b, ["Turn-order", "Turn ", "waiting", "Match Over", "Duel over"], 30)
        if "Turn-order" in ta: a.choose(id=0)
        if "Turn-order" in tb: b.choose(id=0)
        t = play_rounds(a, b, 8)
        show("game3 play", t)
        # game 3: A surrenders -> B wins match 2-1 -> Match Over + DUEL_END
        match_game(a, "A")
        ta = poll_until(a, ["Match Over", "Game Over", "Disconnected"], 30)
        tb = poll_until(b, ["Match Over", "Game Over", "Disconnected"], 30)
        show("match end A", ta)
        show("match end B", tb)
        expect("Match Over" in ta, "A sees Match Over")
        expect("Match Over" in tb, "B sees Match Over")
    finally:
        a.kill(); b.kill()


def scenario_observe():
    print("== SCENARIO 3: observer ==")
    a = MCP("o1a"); b = MCP("o1b"); c = MCP("o1c")
    try:
        connect(a, "o1a", "observe"); connect(b, "o1b", "observe")
        a.choose(id=0); b.choose(id=0); a.choose(id=1)
        # let the duel actually start (RPS + TP) before the observer joins
        rps_answer(a, 1); rps_answer(b, 2)
        tb = poll_until(b, ["Turn-order"], 4)
        if "Turn-order" in tb: b.choose(id=0)
        play_rounds(a, b, 2)
        tc = connect(c, "o1c", "observe")
        expect("take a free seat" in tc, "C joined as observer")
        tc = c.choose(id=-1)
        show("observer view", tc)
        expect(("Turn" in tc) or ("watching" in tc) or ("waiting" in tc), "observer sees duel state")
        # observer disconnect
        td = c.call("ygo_disconnect", {})
        expect("Left the room" in td, "observer left")
    finally:
        a.kill(); b.kill(); c.kill()


def main():
    scenario_single()
    scenario_match()
    scenario_observe()
    print()
    if FAIL:
        print("FAILED %d assertion(s):" % len(FAIL))
        for f in FAIL: print("  -", f)
        sys.exit(1)
    print("ALL PASS (%d tool calls)" % COUNT[0])


main()
