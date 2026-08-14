#!/usr/bin/env python3
"""Minimal interact-like driver: two MCP clients (host+joiner), drive both
sides the way ygocli interact would, print every state transition."""
import json, subprocess, sys, os, time, select

EXE = "./ygocli"
DECK = sys.argv[1] if len(sys.argv) > 1 else "example.ydk"
PORT = str(10000 + (os.getpid() % 20000))
TO = 4.0

class MCP:
    def __init__(self, name):
        self.proc = subprocess.Popen([EXE, "mcp"], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=open("/tmp/md_%s.err" % name, "w"),
            text=True, bufsize=1)
        self._id = 0
        self.name = name
        self.rpc("initialize", {"protocolVersion": "2024-11-05"})
    def rpc(self, method, params=None, timeout=TO):
        self._id += 1
        req = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None: req["params"] = params
        self.proc.stdin.write(json.dumps(req) + "\n"); self.proc.stdin.flush()
        r, _, _ = select.select([self.proc.stdout], [], [], timeout)
        if not r: return None  # timeout
        line = self.proc.stdout.readline()
        if not line: raise RuntimeError(self.name + " closed")
        return json.loads(line)
    def kill(self):
        self.proc.kill(); self.proc.wait()

def txt(resp):
    if resp is None: return "<TIMEOUT>"
    try: return resp["result"]["content"][0]["text"]
    except Exception: return json.dumps(resp)[:200]

def main():
    server = subprocess.Popen([EXE, "server", "--port", PORT], stderr=open("/tmp/md_srv.err","w"))
    time.sleep(0.5)
    host = MCP("host"); joiner = MCP("joiner")
    st = {}
    try:
        r = host.rpc("tools/call", {"name": "ygo_client", "arguments": {
            "host": "127.0.0.1", "port": int(PORT), "create_game": True,
            "deck": DECK, "name": "host", "mode": 0}})
        print("HOST first:", txt(r).replace("\n", "\\n")[:400]); st["host"] = txt(r)
        r = joiner.rpc("tools/call", {"name": "ygo_client", "arguments": {
            "host": "127.0.0.1", "port": int(PORT), "create_game": False,
            "deck": DECK, "name": "joiner", "mode": 0}})
        print("JOIN first:", txt(r).replace("\n", "\\n")[:400]); st["joiner"] = txt(r)

        import re
        for step in range(120):
            changed = False
            for side in ("host", "joiner"):
                t = st[side]
                if "Game Over" in t: continue
                # waiting -> poll again
                if "(waiting" in t or t.startswith("<TIMEOUT"):
                    r = (host if side == "host" else joiner).rpc(
                        "tools/call", {"name": "ygo_client", "arguments": {}})
                    nt = txt(r)
                    if nt != t:
                        print(f"[{step}] {side} poll -> {nt.replace(chr(10), '\\n')[:300]}")
                        st[side] = nt; changed = True
                    continue
                # has a prompt -> answer
                opts = re.findall(r"^(\d+): ", t, re.M)
                if not opts:
                    print(f"[{step}] {side} WAKE NO OPTION: {t[:400]}")
                    st[side] = "---"
                    continue
                mm = re.search(r"\(min (\d+), max (\d+)\)", t)
                args = ({"indices": list(range(int(mm.group(1))))}
                        if mm and int(mm.group(1)) >= 2 else {"id": int(opts[0])})
                r = (host if side == "host" else joiner).rpc(
                    "tools/call", {"name": "ygo_choose", "arguments": args}, timeout=TO)
                nt = txt(r)
                print(f"[{step}] {side} choose {args} -> {nt.replace(chr(10), '\\n')[:200]}")
                st[side] = nt; changed = True
            if not changed:
                time.sleep(0.05)
                if step % 10 == 0: print(f"[{step}] idle...")
    finally:
        host.kill(); joiner.kill(); server.kill(); server.wait()

if __name__ == "__main__":
    main()
