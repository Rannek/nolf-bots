#!/usr/bin/env python3
"""Emit a clang VFS overlay that makes the source tree case-insensitive.

The original code was written on Windows and includes headers with
inconsistent case (StdAfx.h vs stdafx.h, ..\\lt2 vs LT2).  clang reads this
overlay via -ivfsoverlay and resolves those lookups to the real files.
"""
import json
import os
import sys

SKIP_DIRS = {".git", "build", "build-linux"}


def walk(path):
    entries = []
    for name in sorted(os.listdir(path)):
        full = os.path.join(path, name)
        if os.path.isdir(full) and not os.path.islink(full):
            if name in SKIP_DIRS:
                continue
            entries.append({"type": "directory", "name": name, "contents": walk(full)})
        elif os.path.isfile(full):
            entries.append({"type": "file", "name": name, "external-contents": full})
    return entries


def main():
    root, out = os.path.abspath(sys.argv[1]), sys.argv[2]
    overlay = {
        "version": 0,
        "case-sensitive": "false",
        "fallthrough": "true",
        "roots": [{"type": "directory", "name": root, "contents": walk(root)}],
    }
    with open(out, "w") as f:
        json.dump(overlay, f)


if __name__ == "__main__":
    main()
