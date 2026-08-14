#!/usr/bin/env python3
"""Drive the ygocli MCP server and verify forced prompts auto-resolve.

The invariant under test: the agent is only ever woken for a prompt that offers
at least one real numbered option. Prompts with no real decision (pass-only
chains, end-phase-only idle, etc.) must be answered internally and never surface.

Also counts [auto] lines to show the feature is active, and keeps playing until
the game ends or the call budget runs out (games may legitimately not finish
with dumb picks; the invariant check is what matters).
"""
import json
import subprocess
import sys
import re
import os

EXE = "./ygocli"
DECK = sys.argv[1] if len(sys.argv) > 1 else "example.ydk"
BUDGET = int(os.environ.get("MCP_TEST_BUDGET", "150"))

proc = subprocess.Popen(
    [EXE, "mcp", DECK, DECK],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=None,
    text=True, bufsize=1,
    env={**os.environ, "YGOCLI_SEED": "12345"},
)


def rpc(method, params=None, _id=1):
    req = {"jsonrpc": "2.0", "id": _id, "method": method}
    if params is not None:
        req["params"] = params
    proc.stdin.write(json.dumps(req) + "\n")
    proc.stdin.flush()
    line = proc.stdout.readline()
    if not line:
        raise RuntimeError("MCP server closed")
    return json.loads(line)


def choice_key(result):
    """Stable-ish fingerprint of the pending decision: choices + field state."""
    return result


def main():
    init = rpc("initialize", {"protocolVersion": "2024-11-05"}, 1)
    assert init.get("result", {}).get("serverInfo", {}).get("name") == "ygocli", init

    resp = rpc("tools/call", {"name": "ygo_solo",
                              "arguments": {"deck0": DECK, "deck1": DECK}}, 2)
    result = resp["result"]["content"][0]["text"]

    seen = {}          # result fingerprint -> set of tried pick descriptions
    auto_lines = 0
    awakes = 0
    for call in range(BUDGET):
        auto_lines += len(re.findall(r"^\[auto\] ", result, re.M))
        if "Game Over" in result:
            print(f"PASS: game over after {awakes} agent choices; "
                  f"auto-respond lines: {auto_lines}")
            proc.kill()
            proc.wait()
            return

        real_options = re.findall(r"^(\d+): ", result, re.M)
        if not real_options:
            print("FAIL: agent woken with no real option offered")
            print(result[-1200:])
            proc.kill()
            sys.exit(1)

        awakes += 1
        # Build the list of candidate picks: for multi-select (min >= 2) try
        # index combos; otherwise single index. Cycle through untried picks so a
        # dumb pick that loops the state gets replaced by a different one.
        mm = re.search(r"\(min (\d+), max (\d+)\)", result)
        if mm and int(mm.group(1)) >= 2:
            mn = int(mm.group(1))
            mx = int(mm.group(2))
            candidates = []
            n = len(real_options)
            for start in range(n):
                combo = [(start + i) % n for i in range(mn)]
                if mx >= mn and len(set(combo)) == mn:
                    candidates.append(("indices", combo))
            if not candidates:
                candidates = [("indices", list(range(mn)))]
        else:
            candidates = [("id", int(i)) for i in real_options]

        key = choice_key(result)
        tried = seen.setdefault(key, set())
        pick = None
        for cand in candidates:
            if str(cand) not in tried:
                pick = cand
                break
        if pick is None:
            # Every candidate for this state was tried without progress; give up
            # on this state by re-picking the first candidate (avoids infinite
            # alternation within the same state).
            pick = candidates[0]
        tried.add(str(pick))

        kind, val = pick
        args = {kind: val}
        resp = rpc("tools/call", {"name": "ygo_choose", "arguments": args}, 3)
        result = resp["result"]["content"][0]["text"]

    auto_lines += len(re.findall(r"^\[auto\] ", result, re.M))
    print(f"PASS: no forced-prompt wake in {awakes} agent choices "
          f"(game still running after budget {BUDGET}); auto-respond lines: {auto_lines}")
    proc.kill()
    proc.wait()


if __name__ == "__main__":
    main()
