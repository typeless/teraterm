# Host unit-test lane (Lane A)

Fast, pre-merge unit tests that build and run with the **system compiler on
Linux/macOS** — no Windows, no cross toolchain. This is the signal wired into
`.github/workflows/putup.yml` (`unit-tests-host`).

```sh
make            # build + run everything under ASan/UBSan
make check      # same
make clean
```

## What runs here

- **`test_agent_{jsonrpc,mcp,shmem}`** — the pre-existing C tests for the agent
  feature, run verbatim (their own `CHECK()` harness).
- **`test_codeconv`** — Catch2 tests for the pure Unicode scalar converters in
  `common/codeconv.cpp` (UTF-8/UTF-16/UTF-32 round-trips, malformed-input
  handling). Catch2 v3 is vendored at `libs/catch2/`.

## Scope and the `winhost/` shim

Only **pure / protocol** logic belongs here. Core translation units that pull in
`<windows.h>` compile against the minimal shim in `winhost/`, which supplies just
the types, constants, and codepage entry points those files reference. The
codepage functions are **stubs that abort if called** — anything whose behaviour
depends on real `MultiByteToWideChar`/`WideCharToMultiByte`/`GetACP` must be a
`[win]`-tagged test that runs on Lane B (below), not here. `make check` excludes
`[win]` cases automatically. The shim grows only as new host-tested TUs need it.

## Lane B — native Windows (`ttunittest.exe`)

`Tupfile` builds the same Catch2 tests into `ttunittest.exe` via clang-cl+xwin,
against the **real** `<windows.h>` (no shim), so `[win]`-tagged tests exercise
the genuine codepage conversions. It is built by the normal `putup -B build-win`,
run on the `windows-latest` CI lane (job `test-windows`), and locally under Wine:

```sh
wine build-win/tests/unit/ttunittest.exe          # all cases
wine build-win/tests/unit/ttunittest.exe "[win]"  # just the native-only cases
```

## Adding a test

1. New Catch2 file `test_<unit>.cpp` using `#include "catch_amalgamated.hpp"`.
2. Add `<unit>` to `CATCH_TESTS` in the `Makefile`.
3. If the unit under test includes `<windows.h>`, add any newly-referenced
   symbol to `winhost/` (a real type/constant, or a loud stub).

Tests should characterise **actual** current behaviour so the incremental C→C++
rewrite stays faithful; where a test documents a latent bug (e.g. the lax
over-range acceptance in `test_codeconv.cpp`), say so in a comment and keep it
green until the behaviour is deliberately changed with its own RED test.
