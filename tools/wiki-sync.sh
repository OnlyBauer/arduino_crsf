#!/usr/bin/env bash
#
# Publishes the generated pages (tools/wiki_build.py) into this project's wiki.
#
#   tools/wiki-sync.sh            build, commit locally, show what would change
#   tools/wiki-sync.sh --push     the same, and push it — what CI runs
#
# wiki-sync.ps1 is the same script for PowerShell; keep the two in step.
#
# GitLab keeps a project wiki in a git repository of its own, right next to the
# project: https://<host>/<namespace>/<project>.wiki.git. Publishing is therefore
# an ordinary clone-commit-push, which is why this needs no API calls.
#
# Credentials. CI_JOB_TOKEN may read repositories but not write to them, so it
# cannot be used here — a push with it fails with 403. Provide instead:
#
#   WIKI_TOKEN        a Project Access Token (role Developer, scope
#                     write_repository), stored as a masked CI variable
#   WIKI_TOKEN_USER   only for a deploy token: its username (default: oauth2)
#   WIKI_URL          overrides the whole URL, for anything unusual
#
# The token never reaches a log line or .git/config: it lives in the remote URL
# only while the push runs, and every message prints a scrubbed URL.
#
# The wiki must be enabled for the project (Settings -> General -> Visibility).
# A wiki that has never had a page has no repository yet and the clone fails;
# this starts an empty one in that case, so the first run also works.
#
# Pages that the build no longer produces are deleted, because the repository is
# the single source: a page renamed at the source would otherwise stay published
# forever under its old name. Nothing is lost — the wiki is a git repository, and
# its history keeps every removed page. --no-prune leaves them alone.

set -u

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$SELF_DIR/.." && pwd)

PAGES="$REPO/build_wiki/pages"
CLONE="$REPO/build_wiki/repo"
PUSH=0
PRUNE=1

if [ -t 1 ]; then
  B=$(printf '\033[1m'); RED=$(printf '\033[31m'); GRN=$(printf '\033[32m')
  YLW=$(printf '\033[33m'); N=$(printf '\033[0m')
else
  B=""; RED=""; GRN=""; YLW=""; N=""
fi

usage() {
  cat <<EOF
Publishes the generated wiki pages into the project's wiki repository.

Usage: tools/wiki-sync.sh [--push] [--no-prune] [--pages DIR]

  --push        push the commit (default: build and commit locally only)
  --no-prune    keep wiki pages that the build no longer produces
  --pages DIR   where to build the pages (default: build_wiki/pages)
  -h, --help    this text

Environment: WIKI_TOKEN, WIKI_TOKEN_USER, WIKI_URL, WIKI_BRANCH — see the
comment at the top of this file.
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --push)     PUSH=1 ;;
    --no-prune) PRUNE=0 ;;
    --pages)    [ $# -ge 2 ] || { echo "--pages needs a value" >&2; exit 2; }
                PAGES="$2"; shift ;;
    --pages=*)  PAGES=${1#--pages=} ;;
    -h|--help)  usage; exit 0 ;;
    *)          echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

die() { printf '%swiki-sync: %s%s\n' "$RED" "$*" "$N" >&2; exit 1; }
note() { printf '%swiki-sync:%s %s\n' "$B" "$N" "$*"; }

# Everything below prints this, never $wiki_url itself.
scrub() { printf '%s' "$1" | sed 's#://[^@/]*@#://#'; }

# --- 1. build the pages -------------------------------------------------------

# Ask each candidate to run something, rather than trusting that it exists:
# Windows ships a python3 on PATH that is only an App Store shortcut and fails
# the moment it is used.
PY=""
for candidate in python3 python; do
  if "$candidate" -c 'import sys' >/dev/null 2>&1; then PY=$candidate; break; fi
done
[ -n "$PY" ] || die "no working python3 on PATH — needed to build the pages"

rm -rf "$PAGES"
"$PY" "$SELF_DIR/wiki_build.py" -o "$PAGES" || die "building the pages failed"

# --- 2. work out where the wiki lives -----------------------------------------

if [ -n "${WIKI_URL:-}" ]; then
  wiki_url="$WIKI_URL"
elif [ -n "${WIKI_TOKEN:-}" ] && [ -n "${CI_SERVER_HOST:-}" ] && [ -n "${CI_PROJECT_PATH:-}" ]; then
  # Same construction as .fetch-sources in .gitlab-ci.yml: host and path from
  # the predefined variables, so nothing is hard-wired to one instance.
  wiki_url="https://${WIKI_TOKEN_USER:-oauth2}:${WIKI_TOKEN}@${CI_SERVER_HOST}/${CI_PROJECT_PATH}.wiki.git"
else
  origin=$(git -C "$REPO" remote get-url origin 2>/dev/null) ||
    die "no WIKI_URL, no WIKI_TOKEN and no origin remote — nowhere to publish to"
  wiki_url=$(printf '%s' "$origin" | sed 's#\.git$##').wiki.git
  if [ "$PUSH" -eq 1 ] && [ -z "${WIKI_TOKEN:-}" ]; then
    note "${YLW}pushing to $(scrub "$wiki_url") with your own git credentials${N}"
  fi
fi
clean_url=$(scrub "$wiki_url")

# --- 3. get the wiki repository -----------------------------------------------

# Never sit at a username prompt: in CI there is nobody to answer it, and a job
# that hangs for an hour is worse than one that fails in a second. An existing
# value is respected, for a local run that means to use a credential helper.
export GIT_TERMINAL_PROMPT=${GIT_TERMINAL_PROMPT:-0}

rm -rf "$CLONE"
mkdir -p "$(dirname "$CLONE")"

# core.autocrlf=false belongs on the clone command, not on the clone afterwards:
# the checkout happens during the clone, so setting it later would leave a
# working tree full of CRLF on a Windows machine (autocrlf=true is the usual
# setting there). Every page the build does not overwrite would then be
# committed again with changed line endings, and CI would flip it straight back.
if git clone --quiet -c core.autocrlf=false -c core.eol=lf "$wiki_url" "$CLONE" 2>/dev/null; then
  branch=$(git -C "$CLONE" symbolic-ref --short HEAD 2>/dev/null || echo "${WIKI_BRANCH:-main}")
  note "cloned $clean_url (branch $branch)"
else
  # Either the wiki has never been created, or the credentials are wrong. The
  # first case is normal on a first run and recoverable; the second shows up as
  # a failed push below, with git's own message.
  branch=${WIKI_BRANCH:-main}
  note "${YLW}$clean_url could not be cloned — starting an empty wiki on $branch${N}"
  git init --quiet "$CLONE" || die "git init failed"
  # Nothing was checked out here, so setting it now is in time.
  git -C "$CLONE" config core.autocrlf false
  git -C "$CLONE" config core.eol lf
  git -C "$CLONE" symbolic-ref HEAD "refs/heads/$branch"
  git -C "$CLONE" remote add origin "$wiki_url"
fi

# --- 4. sync the tree ---------------------------------------------------------

if [ "$PRUNE" -eq 1 ]; then
  find "$CLONE" -maxdepth 1 -name '*.md' -type f | while IFS= read -r existing; do
    if [ ! -f "$PAGES/$(basename "$existing")" ]; then
      note "removing $(basename "$existing") — no longer generated"
      rm -f "$existing"
    fi
  done
fi

cp "$PAGES"/*.md "$CLONE"/ || die "no pages to publish"

# --- 5. commit and push -------------------------------------------------------

source_sha=${CI_COMMIT_SHA:-$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)}
short_sha=$(printf '%s' "$source_sha" | cut -c1-12)

git -C "$CLONE" add -A || die "git add failed"

if git -C "$CLONE" diff --cached --quiet; then
  note "${GRN}already up to date — nothing to publish${N}"
  exit 0
fi

git -C "$CLONE" --no-pager diff --cached --stat

message="Regenerate wiki from $short_sha"
if [ -n "${CI_PIPELINE_URL:-}" ]; then
  message="$message

Generated by $CI_PIPELINE_URL from ${CI_PROJECT_PATH:-this repository}@$source_sha.
Do not edit here: change the source file and let the pipeline republish."
fi

git -C "$CLONE" \
  -c user.name="${GITLAB_USER_NAME:-wiki bot}" \
  -c user.email="${GITLAB_USER_EMAIL:-ci@${CI_SERVER_HOST:-localhost}}" \
  commit --quiet -m "$message" || die "commit failed"

if [ "$PUSH" -ne 1 ]; then
  note "committed in $CLONE but not pushed (add --push)"
  exit 0
fi

if ! git -C "$CLONE" push --quiet origin "HEAD:refs/heads/$branch"; then
  git -C "$CLONE" remote set-url origin "$clean_url"
  die "push to $clean_url failed — is WIKI_TOKEN valid and scoped write_repository?"
fi

# The token has done its job; leave no copy behind in .git/config.
git -C "$CLONE" remote set-url origin "$clean_url"
note "${GRN}published $short_sha to $clean_url ($branch)${N}"
