#!/bin/sh
# Run the CI stages locally, so a red pipeline is reproducible on the machine
# that caused it.
#
#   tools/ci.sh                  every stage that this machine can run
#   tools/ci.sh lint tests       only those
#   tools/ci.sh --strict         a missing tool is a failure, not a SKIP
#
# Stages: lint tests sanitize docs
#
# .gitlab-ci.yml calls this rather than the tools directly. Two implementations
# of the same check drift, and the one that drifts is always the one nobody runs.
set -eu

STAGES_ALL="lint tests sanitize docs"
STRICT=0
STAGES=""

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO"

B=$(printf '\033[1m'); N=$(printf '\033[0m')
banner() { printf '\n%s── %s %s\n' "$B" "$1" "$N"; }
have() { command -v "$1" >/dev/null 2>&1; }

# A stage that cannot run says so and does not fail the run -- unless --strict,
# which is what CI uses, because a silently skipped check is worth nothing there.
skip() {
  if [ "$STRICT" -eq 1 ]; then
    echo "  FAIL  $1 (missing: $2)"
    return 1
  fi
  echo "  SKIP  $1 (missing: $2)"
  return 0
}

usage() {
  echo "usage: $0 [--strict] [stage ...]"
  echo "stages: $STAGES_ALL"
  exit "${1:-0}"
}

for arg in "$@"; do
  case "$arg" in
    --strict) STRICT=1 ;;
    -h|--help) usage 0 ;;
    -*) echo "unknown option: $arg" >&2; usage 2 ;;
    *) STAGES="$STAGES $arg" ;;
  esac
done
[ -n "$STAGES" ] || STAGES=$STAGES_ALL

# --- lint ---------------------------------------------------------------------

# Every source the library ships plus its tests. The specification submodule is
# not ours and is never touched.
sources() {
  # The vendored core lives flat in src/, so it is excluded by name rather
  # than by directory. Everything this repository actually wrote is the C++.
  git ls-files '*.cpp' '*.h' '*.ino' | grep -v '^src/crsf_'
}

# Whitespace and line endings, checked against the *index* rather than the
# working tree. That is the point: with core.autocrlf=true -- the default on
# Windows, where this is developed -- every file on disk has CRLF even though
# the repository holds LF. A grep over the files on disk would fire on exactly
# the machine this is developed on, and would therefore be useless.
lint_hygiene() {
  rc=0
  bad=$(git ls-files --eol -- '*.c' '*.h' '*.md' '*.yml' '*.py' '*.sh' '*.txt' \
        | grep -v '^i/lf' | grep -v '	src/crsf_' || true)
  if [ -n "$bad" ]; then
    echo "  files stored with the wrong line endings:"
    printf '%s\n' "$bad" | sed 's/^/    /'
    rc=1
  fi

  for f in $(sources); do
    if grep -nP '\t' "$f" >/dev/null 2>&1; then
      echo "  tab character in $f"; rc=1
    fi
    if grep -n ' $' "$f" >/dev/null 2>&1; then
      echo "  trailing whitespace in $f"; rc=1
    fi
  done
  return $rc
}

# The version is written in three places and they must agree. The Doxyfile is
# explicitly one of them: that is where esp_crsf's version sat unnoticed at
# 0.3.0 while its manifest said 1.0.0, because nothing ever read it.
lint_versions() {
  p=$(sed -n 's/^version=//p' library.properties)
  j=$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' library.json)
  d=$(sed -n 's/^PROJECT_NUMBER *= *//p' Doxyfile | tr -d '"[:space:]')
  rc=0
  echo "  library.properties=$p  library.json=$j  Doxyfile=$d"
  [ "$p" = "$j" ] || { echo "  library.json disagrees"; rc=1; }
  [ "$p" = "$d" ] || { echo "  Doxyfile disagrees"; rc=1; }
  return $rc
}

stage_lint() {
  rc=0
  banner "lint: hygiene"
  lint_hygiene || rc=1

  banner "lint: versions"
  lint_versions || rc=1

  banner "lint: clang-format"
  if have clang-format; then
    for f in $(sources); do
      if ! clang-format --dry-run --Werror "$f" >/dev/null 2>&1; then
        echo "  not formatted: $f"; rc=1
      fi
    done
    [ $rc -eq 0 ] && echo "  every file matches .clang-format"
  else
    skip "clang-format" "clang-format" || rc=1
  fi

  banner "lint: clang-tidy"
  if have clang-tidy; then
    # Only this library's own source. The vendored core is c_crsf's code,
    # checked in c_crsf's pipeline.
    clang-tidy --quiet src/CrsfPort.cpp -- -std=c++14 -Isrc -Itests/arduino_stubs >/dev/null || rc=1
    [ $rc -eq 0 ] && echo "  clang-tidy is happy"
  else
    skip "clang-tidy" "clang-tidy" || rc=1
  fi
  return $rc
}

# --- the rest -----------------------------------------------------------------

stage_tests()    { banner "tests";    make -C tests; }
stage_sanitize() {
  banner "sanitize"
  # Cygwin's gcc ships without libasan, which is why this is not part of `tests`.
  if printf 'int main(void){return 0;}' | \
     ${CC:-gcc} -fsanitize=address -x c - -o /dev/null >/dev/null 2>&1; then
    make -C tests sanitize
  else
    skip "sanitize" "libasan"
  fi
}
stage_docs()     {
  banner "docs"
  if have doxygen; then
    doxygen Doxyfile
    echo "  documentation built with no warnings"
  else
    skip "docs" "doxygen"
  fi
}

rc=0
for s in $STAGES; do
  case "$s" in
    lint)     stage_lint     || rc=1 ;;
    tests)    stage_tests    || rc=1 ;;
    sanitize) stage_sanitize || rc=1 ;;
    docs)     stage_docs     || rc=1 ;;
    *) echo "unknown stage: $s" >&2; usage 2 ;;
  esac
done

banner "result"
if [ $rc -eq 0 ]; then
  echo "  everything that could run, passed"
else
  echo "  something failed; see above"
fi
exit $rc
