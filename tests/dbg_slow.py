#!/usr/bin/env python3
"""Slow instrumented run: two clients, full flow, printing every state."""
import json, subprocess, sys, os, time, select

EXE = "/home/z/ygo/bin/release/YGOPro"
HOST = "127.0.0.1"; PORT = 7911; PASS = sys.argv[1] if len(sys.argv) > 1 else "dbg-room"
DECK = "example.ydk"; TO = 10.0


class MCP:
    def __init__(self, name):
        env = dict(os.environ); env["YGOCLI_MCP_DEBUG"] = "1"
        self.proc = subprocess.Popen(
            [EXE, "--mcp", "-k", "-n", name, "-h", HOST, "-p", str(PORT),
             "-w", PASS, "-d", DECK, "-j"],
            cwd="/home/z/ygo", stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=open("/tmp/dbg_%s.err" % name, "w"), text=True, bufsize=1, env=env)
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
        return json.loads(self.proc.stdout.readline())

    def choose(self, **args):
        r = self.rpc("tools/call", {"name": "ygo_choose", "arguments": args})
        return r["result"]["content"][0]["text"]

    def kill(self):
        self.proc.kill(); self.proc.wait()


def show(m, label, t):
    print("[%s] %s: %s" % (m.name, label, (t or "").strip()[:150]))
    print()


a = MCP("A"); b = MCP("B")
try:
    show(a, "lobby", a.choose(id=-1))
    show(b, "lobby", b.choose(id=-1))
    show(a, "ready", a.choose(id=0))
    time.sleep(2)
    show(b, "ready", b.choose(id=0))
    time.sleep(2)
    show(a, "start", a.choose(id=1))
    time.sleep(3)
    for i in range(10):
        ta = a.choose(id=-1); tb = b.choose(id=-1)
        print("[poll %d] A: %s" % (i, (ta or "").strip()[:100]))
        print("[poll %d] B: %s" % (i, (tb or "").strip()[:100]))
        if "RPS choice" in ta or "RPS choice" in tb:
            print("RPS visible; A answering 1, B answering 2")
            show(a, "A rps ans", a.choose(id=1))
            time.sleep(1)
            show(b, "B rps ans", b.choose(id=2))
            time.sleep(2)
        if "Turn-order" in tb or "Turn-order" in ta:
            print("TP visible; B answering 0")
            show(b, "B tp ans", b.choose(id=0))
            time.sleep(2)
        if any(n in ta for n in ("summon", "activate", "attack", "pass")) or \
           any(n in tb for n in ("summon", "activate", "attack", "pass")):
            print("DUEL ACTIVE")
            break
    time.sleep(3)
    # final states
    show(a, "final", a.choose(id=-1))
    show(b, "final", b.choose(id=-1))
finally:
    a.kill(); b.kill()
