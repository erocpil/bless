#!/usr/bin/env python3
"""
CI documentation link checker.

Verifies that all local Markdown links (relative paths to .md files)
in docs/ resolve to existing files.  Exit 0 = clean, exit 1 = broken links.

Usage:
    python3 tools/ci_doc_links.py
"""

import re
import os
import sys
from urllib.parse import unquote, urlsplit

DOCS_DIR = os.path.join(os.path.dirname(__file__), "..", "docs")
LINK_PATTERN = re.compile(r'(?<!!)\[[^\]]*\]\(([^)\n]+)\)')


def markdown_target(raw_target):
    """Return a local .md path, without query/fragment, or None."""
    target = raw_target.strip()
    if target.startswith("<"):
        closing = target.find(">")
        if closing < 0:
            return None
        target = target[1:closing]
    else:
        # Drop an optional Markdown link title: (path.md "title").
        target = target.split(maxsplit=1)[0]

    parsed = urlsplit(target)
    if parsed.scheme or parsed.netloc:
        return None

    path = unquote(parsed.path)
    return path if path.lower().endswith(".md") else None


def check_file(filepath):
    """Return list of broken (source_file, target) tuples."""
    broken = []
    with open(filepath) as f:
        for lineno, line in enumerate(f, 1):
            for match in LINK_PATTERN.finditer(line):
                target = markdown_target(match.group(1))
                if target is None:
                    continue
                # Resolve relative to the source file's directory
                src_dir = os.path.dirname(filepath)
                resolved = os.path.normpath(
                    os.path.join(src_dir, target))
                if not os.path.exists(resolved):
                    broken.append(
                        (os.path.relpath(filepath, DOCS_DIR),
                         lineno, target))
    return broken


def main():
    # Gather the index of all existing .md files under docs/
    md_files = []
    for root, _dirs, files in os.walk(DOCS_DIR):
        for fname in files:
            if fname.endswith(".md"):
                md_files.append(os.path.join(root, fname))

    md_files.sort()
    total_links = 0
    all_broken = []

    for path in md_files:
        broken = check_file(path)
        for src, lineno, target in broken:
            all_broken.append((src, lineno, target))
        with open(path) as f:
            total_links += sum(
                markdown_target(match.group(1)) is not None
                for match in LINK_PATTERN.finditer(f.read()))

    if all_broken:
        print(f"FAIL: {len(all_broken)} broken link(s) found:")
        for src, lineno, target in sorted(all_broken):
            print(f"  {src}:{lineno} -> {target}")
        sys.exit(1)
    else:
        print(f"PASS: {len(md_files)} files, {total_links} links verified")
        sys.exit(0)


if __name__ == "__main__":
    main()
