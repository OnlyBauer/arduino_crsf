#!/bin/sh
# Refresh the vendored copy of c_crsf, and check that nobody has edited it.
#
#   tools/sync_core.sh --tag release_1.0.1     pull that tag into vendor/c_crsf
#   tools/sync_core.sh --tag release_1.0.1 --from ../c_crsf
#                                              ...from a local clone instead
#   tools/sync_core.sh --pin                   record the tree hash after committing
#   tools/sync_core.sh --check                 verify the copy, offline
#
# Why a copy and not a submodule. Four repositories consume this core, and one
# of them is an Arduino library: the Arduino Library Manager packs a zip from
# the git tag and does not fetch submodules, so a submodule would ship broken to
# the people least able to diagnose it. One mechanism for all four beats a
# special case, and a vendored copy also means a source tarball of any tag is
# complete on its own.
#
# What stops it rotting. The pin below records the git *tree* hash of
# vendor/c_crsf, and --check compares it. A tree hash is used rather than a
# sha256 manifest for a specific reason: git computes it from the blobs as
# stored, so core.autocrlf=true on a Windows working copy -- the default, and
# what this project is developed on -- cannot make it disagree. A checksum over
# the files on disk would fail on exactly that machine and nowhere else.
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO"

PIN="c_crsf.pin"
DEST="src"
META="extras/vendor"
MODE=""
TAG=""
FROM=""

usage() {
  echo "usage: $0 --tag <tag> [--from <path>] | --pin | --check"
  exit "${1:-0}"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --tag)   MODE=tag; TAG=${2:?--tag needs a tag}; shift 2 ;;
    --from)  FROM=${2:?--from needs a path}; shift 2 ;;
    --pin)   MODE=pin; shift ;;
    --check) MODE=check; shift ;;
    -h|--help) usage 0 ;;
    *) echo "unknown argument: $1" >&2; usage 2 ;;
  esac
done
[ -n "$MODE" ] || usage 2

# shellcheck disable=SC1090
[ -f "$PIN" ] && . "./$PIN"
CRSF_CORE_URL=${CRSF_CORE_URL:-https://git.bauer.pub/Bauer/c_crsf.git}

# --- what a sync copies -------------------------------------------------------
#
# Deliberately not everything. The core's own protocol suites do not come along:
# the pin already proves the copy matches a tag whose pipeline was green, so
# re-running 6.6 kLOC of them in every consumer is weight without information.
# Flat, and that is not a stylistic choice.
#
# The Arduino 1.5 library format compiles src/ recursively but puts only src/
# itself on the include path. A core under src/c_crsf/ would therefore force
# every vendored file to be edited to say #include "c_crsf/crsf_codec.h" -- which
# is exactly the drift this whole mechanism exists to prevent. Flattening costs a
# cluttered src/ and buys zero edits.
#
# The metadata that must not be compiled goes to extras/, which is the Arduino
# convention for "in the repository, never a source file".
# Exactly the files a sync owns.
#
# The flat layout is forced on us by the Arduino 1.5 format, and it means
# vendored and hand-written sources share one directory. So the set cannot be
# inferred from the directory: crsf_arduino_conf.h is this library's own file and
# only shares the prefix. Deleting it on every sync was a real bug, found by the
# drift check reporting it as missing.
vendored_files() {
  ls "$DEST"/crsf_*.c "$DEST"/crsf_*.h 2>/dev/null |
    grep -v 'crsf_arduino_conf[.]h$' || true
}

# The pin, for a layout where a directory tree hash would be wrong.
#
# Hashing the *index* entries of the vendored files is as CRLF-proof as a tree
# hash -- git stores them normalised, so core.autocrlf on a Windows working copy
# cannot upset it -- and it covers exactly the right set, where a tree hash over
# src/ would report every edit to CRSFv3.cpp as vendor drift.
vendored_hash() {
  # The paths are all crsf_*.c and crsf_*.h, so word splitting is safe here.
  # shellcheck disable=SC2046
  git ls-files -s -- $(vendored_files | sort) | git hash-object --stdin

}

copy_tree() {
  src=$1
  for f in $(vendored_files); do
    rm -f "$f"
  done
  mkdir -p "$DEST" "$META"
  cp "$src"/include/*.h "$DEST/"
  cp "$src"/src/*.c "$DEST/"
  for f in LICENSE NOTICE VERSION; do
    cp "$src/$f" "$META/$f"
  done

  # The wire trace and the script that produces it do come along, and they are
  # the exception that proves the rule above. A port has to be able to show that
  # *it* still puts the same bytes on the wire, and it has to do that against the
  # same recording the core uses -- two copies of a golden file are two files
  # that eventually disagree, and the disagreement looks like a protocol bug.
  cp "$src/tests/golden_script.h" "$META/"
  cp "$src/tests/golden_trace.txt" "$META/"
}

case "$MODE" in
  tag)
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT

    if [ -n "$FROM" ]; then
      echo "cloning $TAG from $FROM"
      git clone --quiet --no-local --depth 1 --branch "$TAG" "$FROM" "$work/c" 2>/dev/null \
        || git clone --quiet --branch "$TAG" "$FROM" "$work/c"
    else
      echo "cloning $TAG from $CRSF_CORE_URL"
      git clone --quiet --depth 1 --branch "$TAG" "$CRSF_CORE_URL" "$work/c"
    fi

    # An annotated tag, not a lightweight one: a release is a signed statement
    # about a commit, and a lightweight tag can be moved without trace.
    if [ "$(git -C "$work/c" cat-file -t "$TAG" 2>/dev/null || echo none)" != "tag" ]; then
      echo "warning: $TAG is not an annotated tag" >&2
    fi

    sha=$(git -C "$work/c" rev-parse "$TAG^{commit}")
    ver=$(tr -d '[:space:]' < "$work/c/VERSION")

    before=$(ls "$DEST"/crsf_*.c 2>/dev/null | sort || true)
    copy_tree "$work/c"
    after=$(ls "$DEST"/crsf_*.c | sort)
    if [ -n "$before" ] && [ "$before" != "$after" ]; then
      echo
      echo "NOTE: the source file list changed. Arduino compiles everything in"
      echo "      src/ so the build needs no update, but tests/Makefile names"
      echo "      modules by hand -- check it."
    fi

    cat > "$PIN" <<PINEOF
# Which c_crsf $DEST holds. Written by tools/sync_core.sh; do not edit by hand.
CRSF_CORE_URL=$CRSF_CORE_URL
CRSF_CORE_TAG=$TAG
CRSF_CORE_VERSION=$ver
CRSF_CORE_SHA=$sha
CRSF_CORE_TREE=
PINEOF
    echo "synced c_crsf $ver ($TAG) into $DEST"
    echo
    echo "Now commit, then record the tree hash:"
    echo "    git add $DEST $PIN && git commit"
    echo "    tools/sync_core.sh --pin && git add $PIN && git commit --amend --no-edit"
    ;;

  pin)
    tree=$(vendored_hash)
    sed "s|^CRSF_CORE_TREE=.*|CRSF_CORE_TREE=$tree|" "$PIN" > "$PIN.tmp"
    mv "$PIN.tmp" "$PIN"
    echo "recorded tree $tree"
    ;;

  check)
    [ -f "$PIN" ] || { echo "no $PIN"; exit 1; }
    : "${CRSF_CORE_TREE:?$PIN has no CRSF_CORE_TREE; run --pin}"

    dirty=$(vendored_files | sort | xargs git status --porcelain -- 2>/dev/null || true)
    if [ -n "$dirty" ]; then
      echo "There are uncommitted changes to the vendored files:"
      printf '%s\n' "$dirty" | sed 's/^/  /'
      echo
      echo "The vendored core is a copy of c_crsf $CRSF_CORE_TAG and is not edited"
      echo "here. Fix it in c_crsf, tag, and run: tools/sync_core.sh --tag <tag>"
      exit 1
    fi

    have=$(vendored_hash)
    if [ "$have" = "$CRSF_CORE_TREE" ]; then
      echo "the vendored core matches the pin ($CRSF_CORE_TAG)"
      exit 0
    fi

    echo "the vendored core has drifted from the pin."
    echo "  pinned: $CRSF_CORE_TREE  ($CRSF_CORE_TAG)"
    echo "  found:  $have"
    echo
    echo "Run: tools/sync_core.sh --tag $CRSF_CORE_TAG"
    exit 1
    ;;
esac
