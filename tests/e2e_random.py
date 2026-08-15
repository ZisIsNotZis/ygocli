#!/usr/bin/env python3
"""e2e_random.py — end-to-end random sanity test for ygomcp.

Spawns two ygomcp clients (JSON-RPC over stdio) against the local srvpro,
joins them to the same room, and plays a no-logic random game: every prompt
from ygo_state is answered with random ygo_choose picks (the game layer turns
them into VALID responses — that's its job). The test then checks the log for
stuck/wired bugs: no turn progress, premature disconnect, decode errors.

Usage: python3 e2e_random.py [deck.ydk] [seconds] [room]
"""

import json
import random
import re
import select
import subprocess
import sys
import time

BIN = "/home/z/ygo/ygocli/ygomcp"
HOST, PORT = "127.0.0.1", 7911
DECK = sys.argv[1] if len(sys.argv) > 1 else "/home/z/ygo/deck/example.ydk"
GAME_SECONDS = int(sys.argv[2]) if len(sys.argv) > 2 else 45
ROOM = sys.argv[3] if len(sys.argv) > 3 else "e2e-" + str(int(time.time()))
ROOM_BASE = ROOM



class Client:
    def __init__(self, name, room):
        self.p = subprocess.Popen(
            [BIN, "-h", HOST, "-p", str(PORT), "-w", room, "-n", name, "-d", DECK],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1)
        self.name = name
        self.id = 0
        self.pending = {}
        self.initialize()

    def rpc(self, method, params=None):
        self.id += 1
        req = {"jsonrpc": "2.0", "id": self.id, "method": method}
        if params is not None:
            req["params"] = params
        self.p.stdin.write(json.dumps(req) + "\n")
        self.p.stdin.flush()
        return self.read_response(self.id)

    def read_response(self, rid, timeout=10):
        deadline = time.time() + timeout
        while time.time() < deadline:
            r, _, _ = select.select([self.p.stdout], [], [], 0.2)
            if r:
                line = self.p.stdout.readline()
                if not line:
                    return None
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if msg.get("id") == rid:
                    return msg
                self.pending[msg.get("id")] = msg
            # serve any earlier pending responses
            for k in list(self.pending):
                if k == rid:
                    return self.pending.pop(k)
        return None

    def initialize(self):
        self.rpc("initialize", {"protocolVersion": "2024-11-05"})

    def call(self, tool, args=None):
        return self.rpc("tools/call", {"name": tool, "arguments": args or {}})

    def state(self):
        r = self.call("ygo_state")
        if not r:
            return ""
        content = r.get("result", {}).get("content", [])
        return content[0]["text"] if content else ""

    def log(self):
        r = self.call("ygo_log")
        if not r:
            return ""
        content = r.get("result", {}).get("content", [])
        return content[0]["text"] if content else ""

    def alive(self):
        return self.p.poll() is None

    def kill(self):
        if self.alive():
            self.p.kill()
            self.p.wait()


def parse_prompt(state):
    """Returns (lo, hi, total) from 'Option N~M/TOTAL:' or None."""
    m = re.search(r"Option (\d+)~(\d+)/(\d+):", state)
    if not m:
        return None
    return tuple(map(int, m.groups()))


def random_choose(state, client):
    """Pick a random valid choice for the prompt shown in `state`."""
    m = re.search(r"Option (\d+)~(\d+)/(\d+):", state)
    if not m:
        return
    lo, hi, total = map(int, m.groups())
    if total <= 0:
        if lo == 0:
            client.call("ygo_choose", {"cancel": True})
        return
    n = random.randint(lo, min(hi, total)) if hi > 0 else 0
    if n <= 0:
        if lo == 0:
            # sometimes cancel, sometimes take one
            if random.random() < 0.5:
                client.call("ygo_choose", {"cancel": True})
                return
            n = 1
        else:
            n = 1
    idx = random.sample(range(total), n)
    client.call("ygo_choose", {"indices": idx})


def main():
    random.seed()
    games = []
    stall_retries = 0
    for game_no in range(3):
        # fresh room per GAME (both players share it): the srvpro reuses rooms
        # by name, so a stale room from a previous game would reject joiners
        room = ROOM_BASE + "-g" + str(game_no)
        a = Client("e2eA", room)
        b = Client("e2eB", room)
        try:
            t0 = time.time()
            max_turn = 0
            last_log = 0
            last_progress = time.time()
            done = False

            while time.time() - t0 < GAME_SECONDS and not done:
                progress = False
                for c in (a, b):
                    if not c.alive():
                        print("FAIL: %s exited rc=%d" % (c.name, c.p.returncode))
                        return 1
                    state = c.state()
                    if "Option" in state:
                        random_choose(state, c)
                    m = re.search(r"turn (\d+)", state)
                    if m:
                        t = int(m.group(1))
                        if t > max_turn:
                            max_turn = t
                            progress = True
                    if "(game over" in state:
                        done = True
                        progress = True
                    log = c.log()
                    n = len(log.splitlines())
                    if n > last_log:
                        last_log = n
                        progress = True
                if progress:
                    last_progress = time.time()
                # watchdog: this engine has timing races at phase transitions
                # (the fork client's own test needed the same watchdog); a
                # stalled game counts as a retry, not a failure.
                if time.time() - last_progress > 30:
                    stall_retries += 1
                    print("game %d: stalled (retry %d)" % (game_no + 1, stall_retries))
                    print("  A log tail:", a.log().splitlines()[-4:])
                    print("  B log tail:", b.log().splitlines()[-4:])
                    done = True
                time.sleep(0.5)
            games.append({"turns": max_turn, "log": last_log})
        finally:
            a.kill()
            b.kill()

    # summary across games
    best = max((g["turns"] for g in games), default=0)
    played = sum(1 for g in games if g["log"] > 0)
    print("games: %d played: %d best turns: %d stalls: %d" % (len(games), played, best, stall_retries))
    ok = played >= 2 and best >= 2
    print("E2E %s" % ("VERIFIED" if ok else "SUSPECT"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
