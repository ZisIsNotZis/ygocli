#!/usr/bin/env python3
"""Drive two real `ygocli interact` processes like a human: send tool names +
args lines, read until the next ygocli> prompt or a non-waiting result."""
import subprocess, sys, os, time, select, re

EXE = "./ygocli"
DECK = sys.argv[1] if len(sys.argv) > 1 else "example.ydk"
PORT = str(10000 + (os.getpid() % 20000))
TO = 5.0

class Interact:
    def __init__(self, name):
        self.proc = subprocess.Popen([EXE, "interact"], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=open("/tmp/ix_%s.err" % name, "w"),
            text=True, bufsize=1)
        self.name = name
        self.buf = ""
        # consume banner + tools list
        self._read_until_prompt()
    def _read_until_prompt(self, timeout=TO):
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([self.proc.stdout], [], [], end - time.time())
            if not r:
                return None  # timed out mid-output
            chunk = self.proc.stdout.read(1)
            if not chunk:
                raise RuntimeError(self.name + " closed")
            self.buf += chunk
            if self.buf.endswith("ygocli> "):
                out = self.buf[:-len("ygocli> ")]
                self.buf = ""
                return out
        return None
    def send(self, line):
        self.proc.stdin.write(line + "\n"); self.proc.stdin.flush()
    def wait(self, timeout=TO):
        """Read until next prompt. Returns output since last prompt (may be None on timeout)."""
        return self._read_until_prompt(timeout)
    def kill(self):
        self.proc.kill(); self.proc.wait()

def main():
    server = subprocess.Popen([EXE, "server", "--port", PORT], stderr=open("/tmp/ix_srv.err","w"))
    time.sleep(0.5)
    host = Interact("host"); joiner = Interact("joiner")

    def call(side, tool, args_lines, label):
        """Send tool + args; read until prompt; return output."""
        side.send(tool)
        for a in args_lines: side.send(a)
        out = side.wait()
        if out is None: out = side.wait()  # retry once
        print(f"--- {label} ---")
        print((out or "<NO OUTPUT>")[:600].replace("\n", "\\n"))
        return out or ""

    try:
        call(host, "ygo_client", ["y", "127.0.0.1", PORT, "y", DECK, "host", "", "", "", "", "", "0"],
             "HOST create")
        call(joiner, "ygo_client", ["", "127.0.0.1", PORT, "", DECK, "joiner", "", "", "", "", "", "0"],
             "JOINER join")
        # drive alternately until game over or budget
        import time as _t
        for step in range(60):
            moved = False
            for side, lbl in ((host, "HOST"), (joiner, "JOIN")):
                out = side.wait(timeout=3.0)
                if out is None:
                    continue
                print(f"[{step}] {lbl}: {out.replace(chr(10), '\\n')[:300]}")
                if "Game Over" in out:
                    print("GAME OVER"); return
                opts = re.findall(r"^(\d+): ", out, re.M)
                if opts:
                    side.send("ygo_choose")
                    side.send(opts[0])
                    side.send("")  # indices empty
                    moved = True
                # else: waiting text -> poll again next step
            if not moved:
                _t.sleep(0.2)
    finally:
        host.kill(); joiner.kill(); server.kill(); server.wait()

if __name__ == "__main__":
    main()
