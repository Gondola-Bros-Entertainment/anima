#!/usr/bin/env python3
"""Check maintained documentation links without build or graphics dependencies."""

from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]


def check(root=ROOT):
    errors = []
    paths = sorted(root.glob("*.md"))
    for directory in ("docs", "third_party", "tests/ui/assets"):
        paths.extend((root / directory).rglob("*.md"))
    for path in paths:
        content = re.sub(r"```.*?```", "", path.read_text(encoding="utf-8"), flags=re.DOTALL)
        for link in re.findall(r"\]\(([^)]+)\)", content):
            target = urlsplit(link.strip("<>"))
            if target.scheme or target.netloc or not target.path:
                continue
            if not (path.parent / unquote(target.path)).exists():
                errors.append(f"{path.relative_to(root)}: missing link target {link}")
    return errors


if __name__ == "__main__":
    failures = check()
    if failures:
        print("\n".join(failures), file=sys.stderr)
    else:
        print("PASS documentation links")
    sys.exit(bool(failures))
