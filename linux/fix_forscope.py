#!/usr/bin/env python3
"""Fix MSVC6 for-loop scoping: hoist `for (T x = ...` declarations when the
compiler reports `x` used after the loop.  Reads clang-cl output on stdin."""
import re
import sys
from collections import defaultdict

err = re.compile(r"^(.*?)\((\d+),\d+\): error: use of undeclared identifier '(\w+)'")
todo = defaultdict(set)
for line in sys.stdin:
    m = err.match(line)
    if m:
        todo[m.group(1)].add((int(m.group(2)), m.group(3)))

for path, items in todo.items():
    raw = open(path, "rb").read().decode("latin-1")
    nl = "\r\n" if "\r\n" in raw else "\n"
    lines = raw.split(nl)
    done = set()
    for lineno, ident in sorted(items):
        pat = re.compile(r"for\s*\(\s*((?:unsigned\s+|const\s+)?[A-Za-z_][\w:]*\s*\**)\s*\b" + ident + r"\b\s*=")
        for i in range(lineno - 1, -1, -1):
            m = pat.search(lines[i])
            if m:
                if i in done:
                    break
                indent = re.match(r"\s*", lines[i]).group(0)
                typ = m.group(1).strip()
                lines[i] = lines[i][: m.start()] + "for (" + ident + " =" + lines[i][m.end():]
                lines.insert(i, indent + typ + " " + ident + ";")
                done = {d + 1 if d >= i else d for d in done} | {i + 1}
                break
        else:
            print(f"could not fix {path}:{lineno} {ident}", file=sys.stderr)
    open(path, "wb").write(nl.join(lines).encode("latin-1"))
    print(f"fixed {path}")
