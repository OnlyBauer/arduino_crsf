#!/usr/bin/env python3
"""Print one version's section of CHANGELOG.md.

The release job uses this to fill in a GitLab Release description. It takes the
version, not the tag, so the caller strips the prefix:

    python3 tools/changelog_section.py 1.0.0

It exits non-zero when the section is missing, and that is the point: a release
cannot then be tagged without its changelog entry having been written first.
Standard library only, so no CI image needs a pip step.
"""

import re
import sys
from pathlib import Path


def section(text: str, version: str) -> str:
    """Return the body of the `## [version]` heading, without the heading."""
    # The heading is `## [1.0.0] — 2026-09-28`, with the date optional and the
    # dash written as an em dash. Only the bracketed version is matched on.
    start = re.compile(r"^## +\[" + re.escape(version) + r"\]", re.MULTILINE)
    m = start.search(text)
    if not m:
        raise SystemExit(
            f"CHANGELOG.md has no '## [{version}]' section.\n"
            f"Write the entry before tagging; the release description comes from it."
        )

    rest = text[m.end():]
    nxt = re.search(r"^## +", rest, re.MULTILINE)
    body = rest[: nxt.start()] if nxt else rest

    # Drop the remainder of the heading line, then trim blank lines.
    body = body.split("\n", 1)[1] if "\n" in body else ""
    return body.strip("\n")


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <version>")

    path = Path(__file__).resolve().parent.parent / "CHANGELOG.md"
    body = section(path.read_text(encoding="utf-8"), sys.argv[1])
    if not body.strip():
        raise SystemExit(f"The '## [{sys.argv[1]}]' section is empty.")
    print(body)


if __name__ == "__main__":
    main()
