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

### Changed

- **Renamed the library and the class from `CrsfPort` to `CRSFv3`**, including
  `src/CrsfPort.h`/`.cpp` → `src/CRSFv3.h`/`.cpp`, `library.properties`,
  `library.json`, `keywords.txt`, the examples, the wiki and this README. `0.1.0`
  called the library `CrsfPort` because "port" was the term shared with
  `crsf_port_t` in the core and `crsf_stm32_port()` in the STM32 port; `CRSFv3`
  instead matches the branding already used across the other three repositories
  ("esp_crsf — CRSFv3 for ESP-IDF", "stm_crsf — CRSFv3 for STM32"), at the cost of
  that one shared word. This is a breaking change against the tagged `0.1.0`
  release: a sketch must change both its `#include` and its class name.

### Added

- Four examples porting the remaining esp_crsf demos that `0.1.0` shipped
  without: `CrsfTxStation` (the handset end, pairing with `CrsfRxStation`),
  `CrsfParameterHost` (discovers devices and walks their parameter tree),
  `CrsfParameterDevice` (serves a parameter tree of every usable type), and
  `CrsfCommandsAndTunnel` (0x32 Direct Commands, MAVLink and MSP tunnels).
  `library.properties`'s `architectures=esp32,stm32,rp2040,samd` and the
  `build-examples` CI matrix apply to these the same as to the first two;
  verified against esp32:esp32:esp32 with `arduino-cli compile`.
- A wiki: **Getting started**, **Hardware status** and **Vendored core**.
- `tools/wiki_build.py` and `tools/wiki-sync.sh`, for building and publishing
  the wiki by hand.
- A *Related repositories* table in the README.
- `tools/junit_from_tests.sh`, so the `unit-tests` job populates GitLab's
  Tests tab instead of leaving it empty. The suites are deliberately
  framework-free (`tests/test_util.h`'s `printf`-based `PASS`/`FAIL` lines,
  not a test framework with structured output), so this converts the
  existing plain-text summary into one JUnit `<testcase>` per suite rather
  than teaching every suite anything about JUnit: each suite's own failure
  detail (the `file:line: ...` lines `CHECK*` already prints) is captured
  from the lines directly preceding its summary line and attached as that
  testcase's `<failure>` body. Coarser than one `<testcase>` per assertion,
  but nothing before this showed up in the Tests tab at all.

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

- The `build-examples` CI job failed for every example on
  `STMicroelectronics:stm32:Nucleo_64:pnum=NUCLEO_F411RE`, including the two
  that shipped in `0.1.0`: `undefined reference to 'Serial1'`. Never caught
  before because the CI matrix builds every `examples/*/` directory in one
  `set -e` shell loop in alphabetical order, and no example before this one
  in that order had ever reached the STM32 link step far enough to hit it.
  The STM32 core's `Serial.h` forward-declares `Serial1` unconditionally
  whenever `USART1` exists (`extern Uart Serial1;` — `Uart` is the core's
  concrete `HardwareSerial` subclass), but only *defines* it when the
  board's one "generic Serial" slot happens to be assigned to USART1; on a
  Nucleo64 board that slot is USART2 (the ST-Link VCP) instead, so `Serial1`
  is a declaration with nothing behind it. A first attempt at this fix
  declared `HardwareSerial Serial1(PA10, PA9);` in each sketch, which
  replaced one failure with another (`conflicting declaration`) by fighting
  that forward declaration instead of working around it. All six examples
  now instead declare their own `Uart CrsfSerial(PA10, PA9);` under
  `#if defined(ARDUINO_ARCH_STM32)` and construct `CRSFv3` from that instead
  of `Serial1` on STM32 only; PA9/PA10 are USART1, wired to the D1/D0 pins
  on a Nucleo64 board's Arduino header. Verified for `esp32:esp32:esp32`
  (the `#if` makes the STM32 branch inert there) and against the installed
  core's actual `Serial.h`/`WSerial.h` source, which is where the exact
  mechanism above was confirmed. Full local compilation for the STM32
  target itself could not be completed: the toolchain arduino-cli bundles
  could not be made to spawn its `cc1plus` as a child process on the
  machine this was fixed on, and manually invoking a second, working
  `arm-none-eabi-g++` against the same real STM32 core sources got past
  `Arduino.h`, `wiring.h` and `clock.h` — the actual chain that matters for
  this fix — before hitting an unrelated path-resolution problem with a
  vendored CMSIS header, native to that machine's scratch directory rather
  than to the code.
- The generated **Examples** wiki page never showed a description for any
  Arduino sketch: `tools/wiki_build.py` only recognises a Doxygen-style
  `/** ... */` file header, and both example `.ino` files used a plain `/*
  ... */` block instead, matching the esp_crsf examples' style everywhere
  except that leading marker. All six example headers (the two existing ones
  plus the four new) now use `/* SPDX-License-Identifier: ... */` followed by
  a `/** @brief ... */` block, exactly as esp_crsf's `main.c` files do, which
  also fixes it there.
- The `build-examples` CI job failed on every board: `examples/CrsfRxStation`
  called nine functions it never defined — `applyControls()`,
  `enterFailsafe()`, four battery readings and three attitude readings —
  meant as stand-ins for a real application's own sensor and control code, but
  never actually given bodies, so the sketch could not compile on any board.
  It now defines each as a trivial stub returning `0` (or doing nothing),
  clearly commented as a placeholder to replace with a real read. Caught by
  `arduino-cli compile` for `esp32:esp32:esp32`; pre-existing since the
  `0.1.0` tag.
- `make -C tests sanitize` failed to build: it compiled the vendored core's `.c`
  files with `$(CXX)` directly. `g++` compiles by C++ rules regardless of file
  extension (unlike `gcc`, which detects the language from it), so under
  `-Werror` this rejected plain-C idioms the core relies on — a `bool`-to-`uint8_t`
  narrowing conversion, a partial aggregate initializer, and an enum/`int`
  ternary. `make` (the non-sanitized target) never hit this because its pattern
  rule already compiles `.c` objects with `$(CC)`. `sanitize` now does the same:
  the core is compiled to `.san.o` objects with `$(CC)` and the sanitizer flags,
  then linked with `$(CXX)`, matching how the library is actually built.
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
