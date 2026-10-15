#!/usr/bin/env python3
"""Write the dedicated server's startup.txt for a self-play run.

    python3 -I selfplay_startup.py <original startup.txt> <output startup.txt> <extra rez>...

NolfServ -nowiz ignores the wizard's custom rez list; its resources come only
from startup.txt ([Rez] Rez0..Rez31, later entries override earlier ones).
Keep the stock list and append the extras (Modernizer, then our bots).
"""
import re
import sys

src, dst, extras = sys.argv[1], sys.argv[2], sys.argv[3:]
text = open(src, newline='').read().replace('\r\n', '\n')
rez = re.findall(r'^Rez(\d+)\s*=\s*"([^"]*)"', text, re.M)
names = [v for k, v in sorted(rez, key=lambda kv: int(kv[0]))] + extras
if len(names) > 32:
    sys.exit("too many rez entries for NolfServ (max 32)")
out = '[Rez]\r\n' + ''.join(f'Rez{i} = "{n}"\r\n' for i, n in enumerate(names)) + '\r\n'
open(dst, 'w', newline='').write(out)
print(f"{len(names)} rez entries, last: {names[-1]}")
