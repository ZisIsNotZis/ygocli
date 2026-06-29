#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DECK_GLOB_DIR="${1:-deck}"
MAX_DECKS="${2:-50}"
TIMEOUT_SEC="${3:-20}"
BASE_SEED="${4:-1000}"

if [[ ! -d "$DECK_GLOB_DIR" ]]; then
  echo "Deck directory not found: $DECK_GLOB_DIR" >&2
  exit 2
fi

make -s

mapfile -t all_decks < <(find "$DECK_GLOB_DIR" -type f -name '*.ydk' | sort)
if [[ "${#all_decks[@]}" -eq 0 ]]; then
  echo "No .ydk files found under $DECK_GLOB_DIR" >&2
  exit 2
fi

if [[ "$MAX_DECKS" -lt "${#all_decks[@]}" ]]; then
  mapfile -t decks < <(printf "%s\n" "${all_decks[@]}" | shuf -n "$MAX_DECKS")
else
  decks=("${all_decks[@]}")
fi

total=0
failed=0
retry_fail=0
unknown_fail=0
timeout_fail=0
no_win_fail=0
failed_examples=()

for deck in "${decks[@]}"; do
  seed=$((BASE_SEED + total))
  log_file="$(mktemp /tmp/ygocli_mass_XXXX.log)"
  set +e
  YGOCLI_SEED="$seed" timeout "${TIMEOUT_SEC}s" ./ygocli "$deck" "$deck" --auto >"$log_file" 2>&1
  rc=$?
  set -e
  retry_count=$(grep -c '\[MSG_RETRY\]' "$log_file" || true)
  unknown_count=$(grep -c '\[MSG_UNKNOWN\]' "$log_file" || true)
  win_count=$(grep -c '\[MSG_WIN\]' "$log_file" || true)
  total=$((total + 1))

  case "$rc" in
    0) ;;
    124) timeout_fail=$((timeout_fail + 1));;
    *) failed=$((failed + 1));;
  esac
  if [[ "$retry_count" -ne 0 ]]; then retry_fail=$((retry_fail + 1)); fi
  if [[ "$unknown_count" -ne 0 ]]; then unknown_fail=$((unknown_fail + 1)); fi
  if [[ "$win_count" -lt 1 ]]; then no_win_fail=$((no_win_fail + 1)); fi

  if [[ "$rc" -ne 0 || "$retry_count" -ne 0 || "$unknown_count" -ne 0 || "$win_count" -lt 1 ]]; then
    failed=$((failed + 1))
    if [[ "${#failed_examples[@]}" -lt 8 ]]; then
      failed_examples+=("deck=$deck seed=$seed rc=$rc retry=$retry_count unknown=$unknown_count win=$win_count log=$log_file")
    else
      rm -f "$log_file"
    fi
  else
    rm -f "$log_file"
  fi
done

echo "total=$total failed=$failed timeout=$timeout_fail retry_fail=$retry_fail unknown_fail=$unknown_fail no_win_fail=$no_win_fail"
for ex in "${failed_examples[@]}"; do
  echo "example: $ex"
done

if [[ "$failed" -ne 0 ]]; then
  exit 1
fi

echo "PASS"
