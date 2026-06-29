#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DECK="${1:-deck/260124DD/b7fb7204fab3c94d.ydk}"
SEED="${2:-1}"
TIMEOUT_SEC="${3:-30}"
LOG_FILE="$(mktemp /tmp/ygocli_regression_XXXX.log)"
trap 'rm -f "$LOG_FILE"' EXIT

if [[ ! -f "$DECK" ]]; then
  echo "Deck not found: $DECK" >&2
  exit 2
fi

make -s

set +e
YGOCLI_SEED="$SEED" timeout "${TIMEOUT_SEC}s" ./ygocli "$DECK" "$DECK" --auto >"$LOG_FILE" 2>&1
run_rc=$?
set -e

retry_count=$(grep -c '\[MSG_RETRY\]' "$LOG_FILE" || true)
unknown_count=$(grep -c '\[MSG_UNKNOWN\]' "$LOG_FILE" || true)
win_count=$(grep -c '\[MSG_WIN\]' "$LOG_FILE" || true)

echo "rc=$run_rc retry=$retry_count unknown=$unknown_count win=$win_count log=$LOG_FILE"

if [[ "$run_rc" -ne 0 ]]; then
  echo "FAIL: process exited non-zero (or timeout)" >&2
  tail -n 80 "$LOG_FILE" >&2
  exit 1
fi
if [[ "$retry_count" -ne 0 ]]; then
  echo "FAIL: saw MSG_RETRY" >&2
  tail -n 80 "$LOG_FILE" >&2
  exit 1
fi
if [[ "$unknown_count" -ne 0 ]]; then
  echo "FAIL: saw MSG_UNKNOWN" >&2
  tail -n 80 "$LOG_FILE" >&2
  exit 1
fi
if [[ "$win_count" -lt 1 ]]; then
  echo "FAIL: no winner produced" >&2
  tail -n 80 "$LOG_FILE" >&2
  exit 1
fi

echo "PASS"
