#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DECK_GLOB_DIR="${1:-/home/z/ygo/deck}"
RUNS="${2:-200}"
TIMEOUT_SEC="${3:-15}"
BASE_SEED="${4:-12000}"

if [[ ! -d "$DECK_GLOB_DIR" ]]; then
  echo "Deck directory not found: $DECK_GLOB_DIR" >&2
  exit 2
fi

make -s

mapfile -t decks < <(find "$DECK_GLOB_DIR" -type f -name '*.ydk' | sort)
if [[ "${#decks[@]}" -lt 2 ]]; then
  echo "Need at least two .ydk decks under $DECK_GLOB_DIR" >&2
  exit 2
fi

total=0
failed=0
retry_fail=0
unknown_fail=0
timeout_fail=0
no_win_fail=0
failed_examples=()

for ((i = 0; i < RUNS; ++i)); do
  duel_seed=$((BASE_SEED + i))
  choice_seed=$((BASE_SEED * 17 + i))
  deck_a="$(printf "%s\n" "${decks[@]}" | shuf -n 1)"
  deck_b="$(printf "%s\n" "${decks[@]}" | shuf -n 1)"
  log_file="$(mktemp /tmp/ygocli_fuzz_XXXX.log)"

  set +e
  YGOCLI_SEED="$duel_seed" \
  YGOCLI_RANDOM_CHOICES=1 \
  YGOCLI_CHOICE_SEED="$choice_seed" \
  timeout "${TIMEOUT_SEC}s" ./ygocli "$deck_a" "$deck_b" --auto >"$log_file" 2>&1
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
    if [[ "${#failed_examples[@]}" -lt 12 ]]; then
      failed_examples+=("deck_a=$deck_a deck_b=$deck_b duel_seed=$duel_seed choice_seed=$choice_seed rc=$rc retry=$retry_count unknown=$unknown_count win=$win_count log=$log_file")
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
