# Contributing to ygocli

Thanks for contributing.

## Preferred workflow

1. Fork the repo.
2. Create a focused branch.
3. Reproduce the issue with concrete decks/logs.
4. Submit a PR with:
   - problem summary,
   - minimal reproduction command,
   - before/after behavior (log snippets preferred).

## What helps most

- Fixes for `MSG_RETRY` loops.
- Better handling for unknown/unparsed messages.
- Safer response encoding in `MSG_SELECT_*` branches.
- Small, targeted PRs rather than broad refactors.

## Local checks

```bash
./build.sh clean
./build.sh
./ygocli <deck0.ydk> <deck1.ydk> [--auto]
```

## Notes

- Deck corpus under `deck/` may be local and large; do not assume all decks are tracked in git.
- Include logs when reporting behavior differences.
