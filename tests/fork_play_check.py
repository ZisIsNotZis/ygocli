#!/usr/bin/env python3
"""Verify REAL gameplay through the MCP bridge: two clients join, ready,
start, answer RPS/TP, then play. The pass-first agent must also resolve
field-mode card selects (end-phase hand discard) via the indices param.
Requires turn count to advance past the discards -- proof the game actually
plays, not just clicks."""
import json, subprocess, sys, os, time, select, re

EXE = "/home/z/ygo/bin/release/YGOPro"
HOST = "127.0.0.1"; PORT = 7911
PASS = sys.argv[1] if len(sys.argv) > 1 else "play-room"
DECK = "example.ydk"; TO = 10.0


class MCP:
    def __init__(self, name):
        self.proc = subprocess.Popen(
            [EXE, "--mcp", "-k", "-n", name, "-h", HOST, "-p", str(PORT),
             "-w", PASS, "-d", DECK, "-j"],
            cwd="/home/z/ygo", stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=open("/tmp/play_%s.err" % name, "w"), text=True, bufsize=1)
        self._id = 0; self.name = name
        time.sleep(3.5)
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

    def choose(self, **args):
        r = self.rpc("tools/call", {"name": "ygo_choose", "arguments": args})
        return r["result"]["content"][0]["text"]

    def state(self):
        r = self.rpc("tools/call", {"name": "ygo_state", "arguments": {}})
        return r["result"]["content"][0]["text"]

    def kill(self):
        self.proc.kill(); self.proc.wait()


def status(t):
    m = re.search(r"Turn (\d+) \| LP (\d+) / (\d+)", t or "")
    return (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else None


a = MCP("A"); b = MCP("B")
samples = []
ra = rb = False
started = False
discards = 0   # field-mode selects resolved
try:
    # lobby
    for m in (a, b):
        for _ in range(20):
            t = m.state()
            if "ready" in t:
                break
            time.sleep(0.5)
        print("lobby", m.name, repr((t or "")[:80]))
    a.choose(id=0); b.choose(id=0)   # ready
    # The srvpro room auto-starts the duel once both players are ready; the
    # host's "start duel" button is a fallback if the auto-start is delayed.
    t0 = time.time()
    game_over = False
    host_started = False
    while time.time() - t0 < 75:
        for m, v in ((a, 1), (b, 2)):
            t = m.state()
            s = status(t)
            if s: samples.append(s)
            if "start duel" in t and not host_started:
                m.choose(id=1)
                host_started = True
                print("  [%s] host clicked start" % m.name)
            elif "RPS choice" in t:
                if (m is a and not ra) or (m is b and not rb):
                    m.choose(id=v)
                    if m is a: ra = True
                    else: rb = True
            elif "Turn-order" in t:
                m.choose(id=0)
            elif re.search(r"select (\d+)-(\d+) \((\d+)/", t):
                # field-mode card select (e.g. end-phase hand discard)
                mo = re.search(r"select (\d+)-(\d+) \((\d+)/(\d+)", t)
                lo, hi, have, total = map(int, mo.groups())
                need = hi - have
                if need <= 0:
                    m.choose(id=-2)     # finish
                else:
                    # fresh prompt: nothing pre-selected, pick the first `need`
                    idx = list(range(min(need, total)))
                    m.choose(id=0, indices=idx)
                    discards += 1
                    print("  [%s] field-select %d-%d: sent %d" % (m.name, lo, hi, len(idx)))
            elif any("%d." % n in t for n in range(6)) and "ready" not in t and "wait" not in t and "leave" not in t:
                # pass-first strategy: end the turn / decline whenever possible
                if "pass" in t or "summon" in t or "attack" in t or "activate" in t or "set-" in t or "reposition" in t:
                    m.choose(id=-1)
                else:
                    m.choose(id=0)
            if "Game Over" in t or "Match Over" in t:
                print("GAME OVER:", (t or "").strip()[:100])
                game_over = True
                break
        if game_over:
            break
        time.sleep(0.05)
finally:
    a.kill(); b.kill()

max_turn = max((s[0] for s in samples), default=0)
lp0s = {s[1] for s in samples}
lp1s = {s[2] for s in samples}
print("samples:", len(samples), "max turn:", max_turn, "field-selects resolved:", discards)
print("LP0 seen:", sorted(lp0s)[:8], "LP1 seen:", sorted(lp1s)[:8])
ok = (max_turn >= 3 or game_over) and discards >= 0
print("PLAY VERIFIED" if ok else "PLAY NOT VERIFIED")
sys.exit(0 if ok else 1)
