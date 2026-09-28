# Vendored core

The `crsf_*.c` and `crsf_*.h` files in `src/` are a verbatim copy of
[c_crsf](https://git.bauer.pub/Bauer/c_crsf), the protocol layer this component
is built on. They are not a submodule and not a fetched dependency. They are a copy, pinned
to a tag, and checked on every pipeline.

They sit **flat** in `src/`, mixed in with `CrsfPort.cpp` and
`crsf_arduino_conf.h`, and that is forced rather than chosen: the Arduino 1.5
library format compiles `src/` recursively but puts only `src/` *itself* on the
include path. A core under `src/c_crsf/` would therefore need every one of its
`#include` lines edited -- which is precisely the drift this mechanism exists to
prevent.

## Do not edit it

A change made here is gone at the next sync, silently, and the CI job
`vendor-unmodified` will fail before that in any case. If you have found a bug in
the protocol layer:

1. Fix it in [c_crsf](https://git.bauer.pub/Bauer/c_crsf), with a test.
2. Tag a release there.
3. Come back and run `tools/sync_core.sh --tag release_X.Y.Z`.

That is more steps than editing in place, and it is the point: the same bug
affects the STM32 and Arduino ports, and fixing it in one of four copies is how
four copies start to disagree.

## Why a copy rather than a submodule

Four repositories consume this core and one of them is an Arduino library. The
Arduino Library Manager builds its package from a zip of the git tag and does not
fetch submodules, so a submodule would ship broken to the people least equipped
to diagnose it. One mechanism across all four beats a special case in one.

Two things fall out of that, both useful here:

- A source archive of any tag is complete on its own. That is what the release
  page links to, and it needs no `--recurse-submodules`.
- The Library Manager and the IDE's "add .ZIP library" both work. Neither fetches
  submodules, so a submodule here would ship broken to the people least equipped
  to diagnose it. That is the reason the whole project vendors rather than
  submodules, and this repository is the one that forces it.

## The pin

`c_crsf.pin` records which core is here:

```sh
CRSF_CORE_URL=https://git.bauer.pub/Bauer/c_crsf.git
CRSF_CORE_TAG=release_1.0.0
CRSF_CORE_VERSION=1.0.0
CRSF_CORE_SHA=<commit>
CRSF_CORE_TREE=<git tree hash of vendor/c_crsf>
```

`CRSF_CORE_TREE` is what `--check` compares. In the other ports it is a git tree
hash of the vendored directory; here it cannot be, because the directory also
holds `CrsfPort.cpp` and a tree hash would report every edit to it as vendor
drift. So it is a hash over the git **index entries** of the vendored files
specifically.

That keeps the property that matters: git stores those entries normalised, so
`core.autocrlf=true` on a Windows working copy — the default, and what this
project is developed on — cannot make it disagree. A `sha256sum` manifest over
the files on disk would fail on exactly that machine and nowhere else, which is
the worst kind of check.

Both of these were real bugs, found by the check rather than by review: the sync
originally cleaned up with `rm -f src/crsf_*.h`, which took
`src/crsf_arduino_conf.h` — this library's own file — with it every time.

## Refreshing it

```sh
tools/sync_core.sh --tag release_1.0.1
git add vendor/c_crsf c_crsf.pin && git commit
tools/sync_core.sh --pin && git add c_crsf.pin && git commit --amend --no-edit
```

The two-step commit is because the tree hash can only be computed once the tree
is committed. `--from ../c_crsf` clones from a local checkout instead of the
remote, which is what you want while developing both at once.

If the source file list changes, the script says so. The Arduino build needs no
attention — it compiles whatever is in `src/` — but `tests/Makefile` names the
core modules by hand.

## What comes along, and what does not

Copied into `src/`: every `crsf_*.h` and `crsf_*.c` of the core, flattened.
Copied into `extras/`: `LICENSE`, `NOTICE`, `VERSION`, and the golden script and
trace. `extras/` is the Arduino convention for "present in the repository, never
compiled", which is exactly right for metadata that must not become a source
file.

`sources.cmake` is not copied: Arduino compiles everything in `src/` and has no
use for it.

Not copied: the core's own protocol suites. The pin already proves this copy
matches a tag whose pipeline was green, so re-running 6.6 kLOC of them here would
cost time and tell us nothing new.

The golden trace is the deliberate exception. This port has to be able to show
that *it* still puts the right bytes on the wire, and it has to do that against
the same recording the core uses — two copies of a golden file are two files that
eventually disagree, and the disagreement looks exactly like a protocol bug.

## Versioning

**A major version bump in the pinned core forces a major bump here.** `crsf_handle_t` is opaque, so the port struct changing size is invisible,
but the payload structs in `crsf_protocol.h` are supplied by this copy: a caller
who links a stale object file against a new layout gets silent corruption rather
than a link error. A minor release may not cross that boundary. While this library is `0.x` that
rule is academic, but it is the rule from the first `1.0.0`.
