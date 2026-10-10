# Code formats and execution contract

The parser/compiler/runtime implement VitaCheat's publicly documented `.psv` code families. [FinalCheat's official documentation](https://finalcheat.github.io/) is the format reference; it is not bundled with this repository. This document describes VCO's boundaries rather than replacing the full format specification.

## Families

| Family | Operation | Implementation notes |
| --- | --- | --- |
| `0` | Direct 8/16/32-bit writes | Width-specific stores, address and range validation |
| `3` | Pointer chains and writes | Bounded depth; each pointer is checked during execution |
| `4` | Compressed/repeated writes | Stride/value progression; bounded work budget |
| `5` | Direct memory copies | Width/header combinations and transfer range checks |
| `7` | Pointer compressed writes | Chain resolution followed by bounded repeated writes |
| `8` | Pointer memory copies | Source/destination chains and width rules |
| `A` | taiHEN-backed injection/patches | A100/A200 variants, saved original bytes, restore on disable |
| `B` | Module-relative addressing | Module index/segment resolution; can combine with other families |
| `C` | Button conditions | Exact button mask semantics; overlay input does not count as game input |
| `D` | Comparisons and conditional blocks | Width, comparison, skip lengths, and supported combinations |

Compatibility means implementing documented semantics, not executing every possible hexadecimal header. Invalid forms and combinations are rejected. Existing lists may still fail because of a changed game build, an invalid pointer at that moment, unsupported memory permissions, patch interference, or bounded capacity.

## File processing

- The source is `ux0:vitacheat/db/<TITLE_ID>.psv` only. There is no alternate project-specific database directory.
- Automatic decoding accepts valid UTF-8 or supported GBK input. Settings can force UTF-8 or GBK; invalid sequences and conversion overflow report an error.
- File names, headers, entry names, and code lines are parsed into caller-owned bounded buffers. The original file is not rewritten.
- `_V0` and `_V1` populate the entry's source flag. Enabled state belongs to a live session and always begins OFF.
- Where a version declaration is present and checked, a mismatch blocks execution. A missing declaration does not establish that the addresses match the installed game.

## Turning entries off

For data writes, OFF ends future updates. There may be no meaningful original value to restore after a game has continued to change it.

For an A-family patch, OFF releases the patch and restores saved bytes. A patch that uses B-relative addressing is still restored because it is an A patch; the B code supplies the address basis. Patch errors retain diagnostic state and must not be displayed as successful OFF.

## Tests

`tests/cheats/cheat_test.c` exercises grammar, widths, address arithmetic, chains, copies, conditions, patch rollback, and combinations. Higher-level tests cover asynchronous state changes, sessions, ownership, failure paths, and game memory adapters. The device harness generates its own controlled code lists for actual mapping and instruction checks.

The bounded PCSB00031 fixtures under `tests/cheats/fixtures/device/` retain addresses and line positions for specific regressions. They are test inputs, not a complete or endorsed game database. No original VitaCheat executable or firmware oracle is included.
