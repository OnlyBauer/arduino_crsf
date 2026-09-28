#!/usr/bin/env python3
"""Builds the GitLab wiki for this component out of the repository itself.

Nothing here is written twice: every page is derived from a file that is
reviewed together with the code, so the wiki cannot drift from it.

    Home.md            README.md
    Compliance.md      COMPLIANCE.md
    Examples.md        the file-level comment of every examples/*/main/main.c
    API-Reference.md   the @brief of every module, plus the Doxygen output
    _sidebar.md        navigation over all of the above
    <name>.md          copied verbatim from wiki/, for pages written by hand

Relative links have to be rewritten on the way in: a wiki page resolves them
against the wiki, not against the repository, so `[x](examples/tx_station)`
would 404. They become absolute links into the project at the commit being
published — permanent, and correct even for readers who never clone.

    python tools/wiki_build.py [-o build_wiki]

Writes a flat page tree and touches nothing else; tools/wiki-sync.sh is what
pushes the result. Standard library only, so the CI image needs no pip step.
"""

import argparse
import os
import re
import subprocess
import sys
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Everything repository-specific lives in wiki/wiki.toml, so this script is the
# same file in all four repositories and can be vendored with the core rather
# than hand-maintained four times.
#
# There is deliberately no fallback. A missing or incomplete config is a loud
# failure, not a silent default: quietly generating another project's sidebar
# is exactly the kind of drift the rest of this project refuses.
CONFIG_PATH = REPO / "wiki" / "wiki.toml"


def load_config():
    """Read wiki/wiki.toml, or explain what is missing and stop."""
    if not CONFIG_PATH.is_file():
        sys.exit(f"wiki_build: {CONFIG_PATH} is missing. Every repository that "
                 f"builds a wiki needs one; see another repository's copy.")
    with open(CONFIG_PATH, "rb") as handle:
        cfg = tomllib.load(handle)

    for key in ("title", "pages", "sidebar", "examples", "api"):
        if key not in cfg:
            sys.exit(f"wiki_build: {CONFIG_PATH} has no [{key}]")
    return cfg


CFG = load_config()

# Pages that come from a repository file, and the wiki page each becomes. Also
# the link map: a link to COMPLIANCE.md must point at the wiki page, not at the
# file, or the reader leaves the wiki mid-sentence.
FROM_FILE = dict(CFG["pages"])

# Sidebar order. Without this the hand-written pages are simply appended after
# the generated ones, which puts "Getting started" below the API reference — the
# alphabet is not a reading order. Anything not named here follows, sorted.
SIDEBAR_ORDER = list(CFG["sidebar"]["order"])

# --- repository facts ---------------------------------------------------------


def git(*args, default=""):
    """git output, or `default` if git or the repository is unavailable."""
    try:
        done = subprocess.run(
            ["git", "-C", str(REPO), *args],
            capture_output=True, text=True, check=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return default
    return done.stdout.strip()


def project_url():
    """Base URL of the project, for links out of the wiki into the code."""
    url = os.environ.get("CI_PROJECT_URL", "").strip()
    if url:
        return url.rstrip("/")
    remote = git("remote", "get-url", "origin")
    if not remote:
        return ""
    remote = re.sub(r"\.git$", "", remote)
    remote = re.sub(r"^git@([^:]+):", r"https://\1/", remote)
    # A remote may carry a token; it must not end up in a published page.
    remote = re.sub(r"^(https?://)[^/@]+@", r"\1", remote)
    return remote.rstrip("/")


def source_ref():
    """The commit the pages are generated from — pinned, so links keep working."""
    for env in ("CI_COMMIT_SHA", "CI_COMMIT_REF_NAME"):
        value = os.environ.get(env, "").strip()
        if value:
            return value
    return git("rev-parse", "HEAD", default="main")


# --- markdown plumbing --------------------------------------------------------

# [label](target "optional title") — also matches the tail of an ![image](...),
# which is what we want: only the target is rewritten.
LINK = re.compile(r"\[([^\]]*)\]\(([^)\s]+)(\s+\"[^\"]*\")?\)")


def rewrite_links(text, url, ref):
    def repl(match):
        label, target, title = match.group(1), match.group(2), match.group(3) or ""
        if re.match(r"^(https?:|mailto:|#|/)", target):
            return match.group(0)
        path, sep, frag = target.partition("#")
        anchor = sep + frag if sep else ""
        if path in FROM_FILE:
            return f"[{label}]({FROM_FILE[path]}{anchor}{title})"
        if not url:
            return match.group(0)
        kind = "blob" if (REPO / path).is_file() else "tree"
        return f"[{label}]({url}/-/{kind}/{ref}/{path.rstrip('/')}{anchor}{title})"

    return LINK.sub(repl, text)


def generated_from(source, url, ref):
    """The line every generated page carries, so nobody edits the wrong copy.

    `source` is linked only when it is one actual file: the pages built from a
    glob or from a property of many files would otherwise advertise a blob URL
    that 404s.
    """
    if url and (REPO / source).is_file():
        where = f"[`{source}`]({url}/-/blob/{ref}/{source})"
    else:
        where = f"`{source}`"
    return (f"*Generated from {where}. Edit the source, not this page — the next "
            f"pipeline run overwrites it.*")


DOC_BLOCK = re.compile(r"/\*\*(.*?)\*/", re.S)


def file_comment(path):
    """(@brief, remaining prose) from the file-level /** ... */ block."""
    text = path.read_text(encoding="utf-8", errors="replace")
    found = DOC_BLOCK.search(text)
    if not found:
        return "", ""

    brief, body = "", []
    for line in found.group(1).splitlines():
        line = re.sub(r"^\s*\*\s?", "", line).rstrip()
        stripped = line.strip()
        if stripped.startswith("@file"):
            continue
        if stripped.startswith("@brief"):
            brief = stripped[len("@brief"):].strip()
            continue
        body.append(line)

    while body and not body[0].strip():
        body.pop(0)
    while body and not body[-1].strip():
        body.pop()
    return brief, "\n".join(body)


# --- pages --------------------------------------------------------------------


def page_from_file(source, url, ref):
    text = (REPO / source).read_text(encoding="utf-8")
    return generated_from(source, url, ref) + "\n\n" + rewrite_links(text, url, ref)


def page_examples(url, ref):
    """One section per example, from the file comment its entry point already has.

    The glob comes from the config because the four repositories disagree about
    where an example's entry point lives: ESP-IDF wants examples/<name>/main/main.c,
    a plain CMake project examples/<name>/main.c, and Arduino insists on
    examples/<Name>/<Name>.ino. The name is always the directory directly under
    examples/, whatever the file beneath it is called.
    """
    ex_cfg = CFG["examples"]
    out = [
        "# Examples",
        "",
        generated_from(ex_cfg["glob"], url, ref),
        "",
        ex_cfg["intro"],
        "",
        "```",
        ex_cfg["build"].strip(),
        "```",
        "",
    ]

    root = REPO / "examples"
    if not root.is_dir():
        out += ["*This repository ships no examples yet.*", ""]
        return "\n".join(out)

    seen = set()
    for entry in sorted(REPO.glob(ex_cfg["glob"])):
        name = entry.relative_to(root).parts[0]
        if name in seen:
            continue
        seen.add(name)
        brief, body = file_comment(entry)
        if url:
            out.append(f"## [`{name}`]({url}/-/tree/{ref}/examples/{name})")
        else:
            out.append(f"## `{name}`")
        out.append("")
        if brief:
            out += [brief, ""]
        if body:
            out += [body, ""]
    return "\n".join(out)


def page_api(url, ref):
    """The module table, from each module's own @brief."""
    out = [
        "# API reference",
        "",
        generated_from("the @brief of every module", url, ref),
        "",
    ]

    pages_url = os.environ.get("CI_PAGES_URL", "").strip().rstrip("/")
    if pages_url:
        out += [
            f"The full Doxygen reference is published from the default branch: "
            f"<{pages_url}/index.html>.",
            "",
        ]
    else:
        out += [
            "The full Doxygen reference is published by the `pages` job from the "
            "default branch; locally, `doxygen` writes it to `docs/html/index.html`.",
            "",
        ]

    out += [
        CFG["api"]["note"],
        "",
        "| Module | Role |",
        "| --- | --- |",
    ]
    # The vendored core is listed too where there is one -- a reader of this
    # reference needs crsf_publish_battery() documented, and it is documented
    # there. The globs come from the config so each repository decides what is
    # its own code and what is shared, and in which order they read.
    modules = []
    for pattern in CFG["api"]["modules"]:
        for path in sorted(REPO.glob(pattern)):
            if path not in modules:
                modules.append(path)
    for path in modules:
        rel = path.relative_to(REPO).as_posix()
        brief, _ = file_comment(path)
        name = f"[`{rel}`]({url}/-/blob/{ref}/{rel})" if url else f"`{rel}`"
        out.append(f"| {name} | {brief} |")
    out.append("")
    return "\n".join(out)


def page_sidebar(generated, handwritten):
    """Navigation over every page, in SIDEBAR_ORDER and then alphabetically.

    Takes the generated page names rather than hard-coding them, so adding a
    page above cannot leave it missing from the sidebar.
    """
    def rank(page):
        if page in SIDEBAR_ORDER:
            return (0, SIDEBAR_ORDER.index(page), page)
        return (1, 0, page)

    out = [f"### {CFG['title']}", ""]
    for page in sorted(set(generated) | set(handwritten), key=rank):
        out.append(f"- [{page.replace('-', ' ')}]({page})")
    out += ["", "*This wiki is generated from the repository.*", ""]
    return "\n".join(out)


# --- build --------------------------------------------------------------------


def build(out_dir):
    url, ref = project_url(), source_ref()
    if not url:
        print("wiki_build: no CI_PROJECT_URL and no origin remote — links into "
              "the code are left as they are", file=sys.stderr)

    hand_dir = REPO / "wiki"
    handwritten = sorted(p.stem for p in hand_dir.glob("*.md")) if hand_dir.is_dir() else []

    pages = {
        "Home.md": page_from_file("README.md", url, ref),
        "Compliance.md": page_from_file("COMPLIANCE.md", url, ref),
        "Changelog.md": page_from_file("CHANGELOG.md", url, ref),
        "Examples.md": page_examples(url, ref),
        "API-Reference.md": page_api(url, ref),
    }
    pages["_sidebar.md"] = page_sidebar(
        [name[:-3] for name in pages], handwritten)
    for page in handwritten:
        pages[f"{page}.md"] = (hand_dir / f"{page}.md").read_text(encoding="utf-8")

    out_dir.mkdir(parents=True, exist_ok=True)
    # The build directory is ours: clear out pages from an earlier run so a
    # renamed page cannot linger and get published forever.
    for stale in out_dir.glob("*.md"):
        stale.unlink()

    for name, text in sorted(pages.items()):
        if not text.endswith("\n"):
            text += "\n"
        (out_dir / name).write_text(text, encoding="utf-8", newline="\n")
        print(f"  {name}")

    print(f"wiki_build: {len(pages)} pages in {out_dir} (from {ref[:12]})")
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Build the wiki page tree from this repository.")
    parser.add_argument("-o", "--out", default=str(REPO / "build_wiki"),
                        help="output directory (default: build_wiki/)")
    args = parser.parse_args()
    return build(Path(args.out))


if __name__ == "__main__":
    sys.exit(main())
