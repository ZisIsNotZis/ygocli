#!/usr/bin/env python3
"""Quick manual driver: two ygocli MCP clients join the same srvpro room by
password, ready, start, and play until Game Over or a prompt with choices."""
import json, subprocess, sys, os, time, select

EXE = "./ygocli"
DECK = sys.argv[1] if len(sys.argv) > 1 else "example.ydk"
PASS = sys.argv[2] if len(sys.argv) > 2 else "roomtest"
HOST = sys.argv[3] if len(sys.argv) > 3 else "127.0.0.1"
PORT = int(sys.argv[4]) if len(sys.argv) > 4 else 7911
TO = 5.0

class MCP:
    def __init__(self, name):
        self.proc = subprocess.Popen([EXE, "mcp"], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=open("/tmp/%s.err" % name, "w"),
            text=True, bufsize=1)
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
        return self.rpc("tools/call", {"name": name, "arguments": args})["result"]["content"][0]["text"]
    def kill(self): self.proc.kill(); self.proc.wait()

def show(side, label, text):
    print(f"--- {label} ---"); print(text); print()

def main():
    a = MCP("a"); b = MCP("b")
    try:
        # main menu
        ta = a.call("ygo_choose", {"id": -1}); show("a", "menu", ta)
        # connect both with same pass
        ta = a.call("ygo_choose", {"id": 1, "host": HOST, "port": PORT, "password": PASS, "deck": DECK, "name": "A"})
        show("a", "connect A", ta)
        tb = b.call("ygo_choose", {"id": 1, "host": HOST, "port": PORT, "password": PASS, "deck": DECK, "name": "B"})
        show("b", "connect B", tb)
        # ready both (choice 0)
        ta = a.call("ygo_choose", {"id": 0}); show("a", "ready A", ta)
        tb = b.call("ygo_choose", {"id": 0}); show("b", "ready B", tb)
        # host starts (choice 1) - A is likely host
        ta = a.call("ygo_choose", {"id": 1}); show("a", "start A", ta)
        tb = b.call("ygo_choose", {"id": -1}); show("b", "poll B", tb)
        # RPS (1/2/3) then TP (0/1), then loop duel
        # NOTE: -1 is NOT valid while RPS/TP is pending; only answer with a real choice.
        for side, m in (("a", a), ("b", b)):
            t = ""
            for _ in range(8):
                if "RPS" in t:
                    show(side, "RPS", t)
                    t = m.call("ygo_choose", {"id": 1})   # rock
                    continue
                if "Turn-order" in t:
                    show(side, "TP", t)
                    t = m.call("ygo_choose", {"id": 0})   # first
                    continue
                if "Main menu" in t or "Game Over" in t or "waiting" in t or "Lobby" in t or "Room" in t or "cancel ready" in t:
                    break
                t = m.call("ygo_choose", {"id": -1})      # poll only when no pending choice
            show(side, "post-start", t)
        # play a few rounds: answer prompts with first option, -1 to pass
        for i in range(40):
            done = True
            for side, m in (("a", a), ("b", b)):
                t = m.call("ygo_choose", {"id": -1})
                if "Game Over" in t:
                    show(side, "end", t); return
                if "Main menu" in t:
                    show(side, "back-to-menu", t); return
                if any(f"{n}." in t for n in range(0, 4)) and "RPS" not in t and "Turn-order" not in t:
                    done = False
                    show(side, f"round {i}", t)
                    m.call("ygo_choose", {"id": 0})
            if done:
                show("x", "idle all", "both idle (waiting)")
                break
        print("DRIVER DONE")
    finally:
        a.kill(); b.kill()

main()
