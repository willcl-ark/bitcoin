# GUI

GUI-only `src/qt` changes normally belong in [bitcoin-core/gui](https://github.com/bitcoin-core/gui); global refactors, build changes, and `src/interfaces` changes belong in the node repository.

## Scope & responsibilities

- Expose new features through RPC first where practical, then build the GUI layer on top.
- Models pass data/events and **do not** manipulate dialogs; views handle interaction and avoid direct core access.
- Do not block the GUI thread.
- Interface functions used **only** by the GUI should be defined and maintained within the GUI code, not added to the core library.

## Translation rules

- Submit all translation changes through Bitcoin Core's [Transifex](https://www.transifex.com/bitcoin/bitcoin/) page, not as GitHub pull requests.
- Only mark strings for translation with `_()` when the translated string will actually be presented to the user. Do not translate logging-only, comparison-only, or discarded strings.
- Only pass **string literals** to `_()`. Raw c-string pointers or runtime-generated strings cannot be extracted by translation platforms.
- Avoid trailing whitespace in translatable strings.
- Do not modify the auto-generated file `src/qt/bitcoinstrings.cpp`.

## Qt UI maintenance

- When removing all items from a row in a Qt `.ui` `gridLayout`, renumber all subsequent rows to maintain a clean, sequential layout definition.

## Startup warnings

- Use `InitWarning` (not `LogWarning`) for important startup warnings that GUI users should see on screen.

## Further reading

- [Bitcoin Core GUI repository](https://github.com/bitcoin-core/gui) — contribution guidelines
- [`doc/translation_process.md`](../doc/translation_process.md) — translation workflow
- [`doc/developer-notes.md`](../doc/developer-notes.md) — general developer notes
