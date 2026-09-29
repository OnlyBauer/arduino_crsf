# Changelog

All notable changes to this library, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## Tagging

| Tag | `library.properties` and `library.json` |
| --- | --- |
| `release_X.Y.Z` | `X.Y.Z` |
| `prerelease_X.Y.Z_dev` | `X.Y.Z-dev` |

`library.properties` is the authority, because the Arduino Library Manager reads
that and nothing else. The `version-matches-tag` CI job checks it against the tag
and against `library.json`, which must agree.

## [Unreleased]

### Added

- A wiki: **Getting started**, **Hardware status** and **Vendored core**.
- `tools/wiki_build.py` and `tools/wiki-sync.sh`, for building and publishing
  the wiki by hand.
- A *Related repositories* table in the README.

### Changed

- The wiki is no longer published by CI; `tools/wiki-sync.sh --push` publishes it
  by hand.
- `.clang-format` sets `AllowShortFunctionsOnASingleLine: Inline` and
  `AccessModifierOffset: -4`, the only place this repository's formatting differs
  from its siblings. clang-format has no separate C language — `.c` and `.cpp`
  are both `Cpp` — so there is no language-scoped section to put it in, and this
  repository's own code is entirely C++. With the inherited `None`, each of the
  fifteen trivial accessors in `CrsfPort.h` became a four-line block, which is
  right for C and makes the library's main header hard to read.

### Fixed

- There was no `.clang-tidy` at all, so the `lint` job ran with defaults.
- Three files did not match `.clang-format`.
- clang-tidy analysed the vendored core as C++ and reported
  `misc-const-correctness` in `crsf_codec.h` — a finding that does not even apply
  in C, where that file is compiled everywhere else. The header filter now
  matches only `CrsfPort.h` and `crsf_arduino_conf.h`.
- A missing space in the clang-tidy invocation in `tools/ci.sh`.
- `tools/ci.sh lint` now also covers `tests/test_arduino_port.cpp` and
  `tests/arduino_stubs/arduino_stubs.cpp`, not only `src/CrsfPort.cpp`.
  `.clang-tidy` gains three exclusions that only apply to this repository's own
  C++ test code — `misc-const-correctness` and `misc-use-anonymous-namespace`
  fire on C idioms (the `CHECK_EQ_*` macros from the shared `test_util.h`, and
  file-scope `static`, respectively) that every other repository in this
  project uses identically as plain C; `cert-err58-cpp` fires on a test
  double's trivial constructor in a project that uses no exceptions anywhere.
  Also fixes a stale header comment that described this file's static analysis
  in terms of ESP-IDF and `esp-clang`, left over from being copied from
  esp_crsf and never corrected.
- `build_wiki/` was missing from `.gitignore`, so generated pages were being
  staged. It is ignored in the other three repositories; now here too.
- The `arduino-lint` job failed CI outright: `--library-manager update` checks
  the library against its *existing* entry in the central Library Manager
  index, and this library has never been submitted there — the README says so.
  Rule LP018 has nothing to compare against and refuses rather than passing,
  with `Library name CrsfPort not found in the Library Manager index`.
  `--library-manager false` skips the registry-specific rules instead of
  asserting a submission status this project has not taken. The comment
  claiming `submit` needs public git hosting was untested and, on checking,
  appears to have been wrong: a local run of `--library-manager submit`
  produced no such complaint. `submit` is the better flag once submission is
  actually intended.

## [0.1.0] — 2026-09-28

First tagged version. The Arduino wrapper over
[c_crsf](https://git.bauer.pub/Bauer/c_crsf) `release_1.0.0`.

### Added

- `CrsfPort`, over a `Stream` or a `HardwareSerial`: `begin()`, `loop()`,
  `end()`, about fifteen convenience methods, and an implicit conversion to
  `crsf_handle_t` so the whole ~120-function C API reads naturally in a sketch.
- `src/crsf_arduino_conf.h`, with the read chunk size, the default baud rate, and
  the AVR check.
- `tests/arduino_stubs/`, a mock Arduino runtime, and 43 checks against it.
- `examples/CrsfRxStation` and `examples/CrsfLoopbackSelfTest`.
- `library.properties`, `library.json` and `keywords.txt`.

### Decisions worth recording

**The library is called `CrsfPort`, and so is the class.** "Port" is the word the
whole project uses — `crsf_port_t` in the core, `crsf_stm32_port()` in the STM32
port, `struct crsf_port` in the ESP-IDF one — so a reader moving between the four
repositories meets the same term everywhere.

**About fifteen methods, not one hundred and twenty.** Wrapping the whole C API
would double the documentation, guarantee that a function added upstream silently
does not appear here, and buy nothing at the byte level: Arduino builds with
`-ffunction-sections -Wl,--gc-sections`, so an unreferenced C function costs
exactly as little as an unreferenced inline method. The implicit conversion makes
the rest read the same as it does on every other platform.

**The core is flat in `src/`.** The Arduino 1.5 format compiles `src/`
recursively but puts only `src/` itself on the include path, so a core under
`src/c_crsf/` would force every vendored file to be edited to change its
`#include` lines — exactly the drift vendoring exists to prevent. The metadata
that must not be compiled lives in `extras/`, which is the Arduino convention for
"present in the repository, never a source file".

**AVR is refused with an `#error`.** A port is 3552 bytes of RAM against an
ATmega328P's 2048. `architectures` in `library.properties` is only a hint and the
IDE would otherwise fail inside the linker with a message nobody can act on.

### Two consequences of the flat layout, found by using it

Vendored and hand-written sources sharing one directory costs more than
clutter, and both of these were caught by the drift check rather than by review:

- `tools/sync_core.sh` cleaned up with `rm -f src/crsf_*.h`, which took
  `src/crsf_arduino_conf.h` with it on every sync — this library's own file,
  guilty only of sharing the prefix. The sync now deletes an enumerated set
  rather than a glob.
- The drift check compared a git tree hash of the vendored directory, which for
  a flat layout would have reported every edit to `CrsfPort.cpp` as vendor
  drift. It now hashes the index entries of the vendored files specifically:
  just as immune to `core.autocrlf` as a tree hash, and covering exactly the
  right set. Verified both ways — editing a vendored file fails, editing
  `CrsfPort.cpp` or `crsf_arduino_conf.h` does not.

### Not verified

**Nothing here has been run on a board.** It compiles for four architectures and
passes its host suites against a mock Arduino runtime. No byte has left a real
UART.

Specifically unverified: behaviour on any real `HardwareSerial`, whether a given
board can produce 416666 baud closely enough, and interoperation with any actual
Crossfire or ELRS device. The flash figure quoted in the README is an estimate,
labelled as one, because there is no `avr-gcc` here to measure with. `0.x`, and
staying there, until `examples/CrsfLoopbackSelfTest` runs on a board. See
COMPLIANCE.md §6.
