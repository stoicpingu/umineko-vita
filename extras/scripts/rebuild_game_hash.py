#!/usr/bin/env python3
"""Rebuild assets/game/game.hash for the current asset tree.

The hash file is an INI with two sections:

    [info]
    "game"="..."
    "hash"="size"      <- the hash IS the file size in bytes
    "ver"="..."
    "apiver"="..."
    "date"="..."
    [data]
    "<relative-path>"="<size-in-bytes>"
    ...

Lines are CRLF. Only the seven top-level dirs that the engine treats as game
data are tracked: backgrounds, fonts, graphics, legacy, sound, sprites, video.
Everything else at the root (`save/`, `onscripter-ru-osx.app/`, `*.cfg`,
`*.sav`, `*.file`, `*.hash`) is intentionally excluded.

Header keys are preserved verbatim from the existing file so the engine sees
the same `ver`/`apiver`. Pass --info-from <file> to import them from another
hash instead.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

TRACKED_DIRS = (
    "backgrounds",
    "fonts",
    "graphics",
    "legacy",
    "sound",
    "sprites",
    "video",
)

DEFAULT_INFO = [
    ('game', 'UminekoPS3fication*'),
    ('hash', 'size'),
    ('ver', '20190109-ru'),
    ('apiver', '2.2.0'),
    ('date', 'ignore'),
]

INFO_LINE_RE = re.compile(r'^"([^"]+)"="([^"]*)"\s*$')


def parse_info(path: Path):
    info = []
    seen = set()
    in_info = False
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if line == "[info]":
                in_info = True
                continue
            if line == "[data]":
                break
            if not in_info or not line:
                continue
            m = INFO_LINE_RE.match(line)
            if not m:
                continue
            key, value = m.group(1), m.group(2)
            if key in seen:
                continue
            seen.add(key)
            info.append((key, value))
    return info or list(DEFAULT_INFO)


def walk_tracked(root: Path):
    for top in TRACKED_DIRS:
        top_dir = root / top
        if not top_dir.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(top_dir):
            dirnames.sort()
            for name in sorted(filenames):
                if name == ".DS_Store" or name == "Thumbs.db" or name.startswith("._"):
                    continue
                full = Path(dirpath) / name
                rel = full.relative_to(root)
                try:
                    size = full.stat().st_size
                except OSError as e:
                    print(f"warn: cannot stat {rel}: {e}", file=sys.stderr)
                    continue
                yield rel.as_posix(), size


def main() -> int:
    parser = argparse.ArgumentParser(description="Rebuild game.hash for assets/game.")
    parser.add_argument("--root", type=Path, required=True,
                        help="Path to assets/game (the optimized asset root).")
    parser.add_argument("--out", type=Path,
                        help="Path to write game.hash (defaults to <root>/game.hash).")
    parser.add_argument("--info-from", type=Path,
                        help="Read [info] header from this file instead of <root>/game.hash.")
    args = parser.parse_args()

    root = args.root.resolve()
    if not root.is_dir():
        print(f"--root is not a directory: {root}", file=sys.stderr)
        return 2

    out_path = args.out or (root / "game.hash")
    info_src = args.info_from or (root / "game.hash")

    info = parse_info(info_src) if info_src.exists() else list(DEFAULT_INFO)

    entries = list(walk_tracked(root))
    entries.sort(key=lambda kv: kv[0])

    lines = ["[info]"]
    for k, v in info:
        lines.append(f'"{k}"="{v}"')
    lines.append("[data]")
    for rel, size in entries:
        lines.append(f'"{rel}"="{size}"')

    body = "\r\n".join(lines) + "\r\n"
    tmp = out_path.with_suffix(out_path.suffix + ".tmp")
    tmp.write_bytes(body.encode("utf-8"))
    tmp.replace(out_path)

    print(f"Wrote {out_path}")
    print(f"  info keys: {len(info)}")
    print(f"  data entries: {len(entries)}")
    print(f"  total bytes tracked: {sum(s for _, s in entries):,}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
