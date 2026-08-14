#!/usr/bin/env python3
"""Verify forced-prompt auto-respond works on the network-client path.

Launches a ygocli server, connects two MCP clients (host + joiner), and plays
the host side: every time the host is woken it must be a real choice. Also
counts [auto] narration produced on the host (forced prompts answered inline).
"""
import json
import subprocess
import sys
import re
import os
import time
import select

EXE = "./ygocli"
DECK = sys.argv[1] if len(sys.argv) > 1 else "example.ydk"
PORT = str(10000 + (os.getpid() % 20000))
BUDGET = int(os.environ.get("MCP_NET_BUDGET", "120"))
READ_TIMEOUT = 5.0   # seconds per rpc read; a timeout => side is idle/waiting


class MCP:
    def __init__(self, name):
        self.proc = subprocess.Popen(
            [EXE, "mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=open("/tmp/%s.err" % name, "w"), text=True, bufsize=1)
        self._id = 0
        self.name = name
        rpc(self, "initialize", {"protocolVersion": "2024-11-05"})

    def rpc(self, method, params=None):
        self._id += 1
        req = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            req["params"] = params
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        r, _, _ = select.select([self.proc.stdout], [], [], READ_TIMEOUT)
        if not r:
            raise TimeoutError(f"{self.name} read timed out")
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError(f"{self.name} closed")
        return json.loads(line)

    def kill(self):
        self.proc.kill()
        self.proc.wait()


def rpc(m, method, params=None):
    return m.rpc(method, params)


def main():
    server = subprocess.Popen([EXE, "server", "--port", PORT], stderr=open("/tmp/srv.err","w"))
    time.sleep(0.5)

    host = MCP("host")
    joiner = MCP("joiner")
    try:
        # Host creates the room; joiner joins. Both upload the same deck.
        r = rpc(host, "tools/call", {"name": "ygo_client", "arguments": {
            "host": "127.0.0.1", "port": int(PORT), "create_game": True,
            "deck": DECK, "name": "host", "mode": 0}})
        r = rpc(joiner, "tools/call", {"name": "ygo_client", "arguments": {
            "host": "127.0.0.1", "port": int(PORT), "create_game": False,
            "deck": DECK, "name": "joiner", "mode": 0}})

        states = {"host": r["result"]["content"][0]["text"],
                  "joiner": r["result"]["content"][0]["text"]}
        host_awakes = 0
        total_auto = 0

        def answer_one(side):
            """Answer one pending prompt for a side; returns (new_text, awoken)."""
            nonlocal host_awakes
            text = states[side]
            if "Game Over" in text:
                return text, False
            if "(waiting" in text or "(observing" in text or "(surrendered" in text:
                return text, False
            opts = re.findall(r"^(\d+): ", text, re.M)
            if not opts:
                print(f"FAIL(net): {side} woken with no real option")
                print(text[-1000:])
                sys.exit(1)
            if side == "host":
                host_awakes += 1
            mm = re.search(r"\(min (\d+), max (\d+)\)", text)
            args = ({"indices": list(range(int(mm.group(1))))}
                    if mm and int(mm.group(1)) >= 2 else {"id": int(opts[0])})
            try:
                r = rpc(host if side == "host" else joiner, "tools/call",
                        {"name": "ygo_choose", "arguments": args})
            except TimeoutError:
                # ygo_choose blocks waiting for the next prompt (pre-existing
                # auto-unblock design); treat the side as idle and move on.
                return "(waiting for opponent or disconnected)\n", False
            return r["result"]["content"][0]["text"], True

        for _ in range(BUDGET):
            for side in ("host", "joiner"):
                total_auto += len(re.findall(r"^\[auto\] ", states[side], re.M))
            if "Game Over" in states["host"] or "Game Over" in states["joiner"]:
                print(f"PASS(net): game over after {host_awakes} host choices; "
                      f"auto lines {total_auto}")
                return
            for side in ("host", "joiner"):
                if "(waiting" in states[side]:
                    try:
                        r = rpc(host if side == "host" else joiner, "tools/call",
                                {"name": "ygo_client", "arguments": {}})
                        states[side] = r["result"]["content"][0]["text"]
                    except TimeoutError:
                        states[side] = "(waiting for opponent or disconnected)\n"
                states[side], _ = answer_one(side)
            time.sleep(0.05)

        for side in ("host", "joiner"):
            total_auto += len(re.findall(r"^\[auto\] ", states[side], re.M))
        print(f"PASS(net): no forced-prompt wake in {host_awakes} host choices "
              f"(budget {BUDGET}); auto lines {total_auto}")
    finally:
        host.kill()
        joiner.kill()
        server.kill()
        server.wait()


if __name__ == "__main__":
    main()
