# ocgcore limitations for large decks

Observed from code review and a synthetic run with 1000 main + 1000 extra cards per side:

- **Sequence IDs are 8-bit**
  - `card_state.sequence` is `uint8_t`.
  - Card movement/indexing into deck, hand, grave, remove, and extra uses 0-255 style sequences.
  - Past 255 cards, sequence values wrap and become ambiguous.

- **Deck/extra counts are serialized as 8-bit values**
  - `field.cpp` writes `list_main.size()` and `list_extra.size()` as `uint8_t` in field-state messages.
  - UI/client state will truncate counts above 255.

- **API/card creation uses 8-bit sequence parameters**
  - `new_card(...)` and `new_tag_card(...)` take `uint8_t sequence`.
  - Large decks can be loaded, but placement/indexing semantics are not designed for thousands of cards.

- **Selection code still assumes 256-card buffers**
  - `processor.cpp` contains fixed scratch buffers like `card* tc[256]`.
  - Some selection/ordering paths will not scale beyond that without refactoring.

- **CLI can run, but state becomes unreliable**
  - The synthetic mega-deck run did not crash immediately.
  - However, state output showed wrapped sequence numbers (e.g. grave indexes far below the true pile size), so correctness is not preserved.

## Conclusion

`ygocli` + `ocgcore` can *start* with huge decks, but they do **not** fully support thousands of mains/extras in a correct way. The hard blockers are mostly 8-bit indices/counts and fixed 256-slot selection assumptions.
