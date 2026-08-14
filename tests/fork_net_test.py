#!/usr/bin/env python3
"""Drive the forked ygopro client (--mcp) through a full network duel against
srvpro: launch two clients, join the same room, ready, start, RPS, TP, play."""
import json, subprocess, sys, os, time, select

EXE = "/home/z/ygo/bin/release/YGOPro"
HOST = "127.0.0.1"
PORT = 7911
PASS = sys.argv[1] if len(sys.argv) > 1 else "mcp-room"
DECK = "example.ydk"
TO = 10.0
FAIL = []


class MCP:
    def __init__(self, name, n):
        self.proc = subprocess.Popen(
            [EXE, "--mcp", "-k", "-n", name, "-h", HOST, "-p", str(PORT),
             "-w", PASS, "-d", DECK, "-j"],
            cwd="/home/z/ygo",
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=open("/tmp/ygo_%s.err" % name, "w"), text=True, bufsize=1)
        self._id = 0
        self.name = name
        self.n = n
        time.sleep(3.5)   # let the GUI initialize and join the room
        self.rpc("initialize", {"protocolVersion": "2024-11-05"})

    def rpc(self, method, params=None):
        self._id += 1
        req = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            req["params"] = params
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        r, _, _ = select.select([self.proc.stdout], [], [], TO)
        if not r:
            raise TimeoutError(self.name + " timeout")
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError(self.name + " closed")
        return json.loads(line)

    def call(self, name, args):
        resp = self.rpc("tools/call", {"name": name, "arguments": args})
        try:
            return resp["result"]["content"][0]["text"]
        except Exception:
            return "RPC-ERROR: " + json.dumps(resp)

    def choose(self, **args):
        return self.call("ygo_choose", args)

    def kill(self):
        try:
            self.proc.kill()
            self.proc.wait()
        except Exception:
            pass


def expect(cond, label):
    if cond:
        print("  ok :", label)
    else:
        FAIL.append(label)
        print("  FAIL:", label)


def show(label, text):
    print("--- %s ---" % label)
    print((text or "").strip()[:250])
    print()


def main():
    a = MCP("A", 0)
    b = MCP("B", 1)
    try:
        ta = a.choose(id=-1)
        expect("ready" in ta, "A in lobby with ready option")
        tb = b.choose(id=-1)
        expect("ready" in tb, "B in lobby with ready option")

        a.choose(id=0)   # A ready
        b.choose(id=0)   # B ready
        time.sleep(1)

        # host starts (option 1 = start for host, else wait)
        ta = a.choose(id=1)
        if "start" in ta:
            a.choose(id=1)
        tb = b.choose(id=1)
        if "start" in tb:
            b.choose(id=1)

        # adaptive pre-duel + play loop
        turns = 0
        rps_answered = {0: False, 1: False}
        for i in range(200):
            acted = False
            for m, rpsval in ((a, 1), (b, 2)):
                t = m.choose(id=-1)
                if "Game Over" in t or "Match Over" in t or "Main menu" in t or "Disconnected" in t:
                    show("end %s" % m.name, t)
                    print("DRIVER DONE (game over)")
                    return
                if "RPS choice" in t and not rps_answered[m.n]:
                    m.choose(id=rpsval)
                    rps_answered[m.n] = True
                    acted = True
                    continue
                if "Turn-order" in t:
                    m.choose(id=0)
                    acted = True
                    continue
                # duel prompt with numbered options (or pass)
                if any(f"{n}." in t for n in range(0, 6)) or "pass" in t:
                    if "ready" not in t and "leave room" not in t and "wait" not in t:
                        m.choose(id=0)
                        acted = True
                        continue
            if acted:
                turns += 1
            if i and i % 20 == 0:
                print("  round", i)
        print("DRIVER DONE (200 rounds)")
    finally:
        a.kill()
        b.kill()
    if FAIL:
        print("FAILED:", FAIL)
        sys.exit(1)
    print("ALL PASS")


main()
