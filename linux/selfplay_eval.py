#!/usr/bin/env python3
"""Summarise a bot log (self-play or a normal match) into comparable metrics.

    python3 -I selfplay_eval.py <botlog.txt> [more logs...]

Per log: final team scores, bot photo attempts, nav islands + gap reason,
per-team goal mix, how far attackers get toward the enemy documents, deaths,
stuck counts, fall-back (recall) activity and "no path to objective" counts.
"""
import re
import sys
from collections import Counter, defaultdict

BOT = re.compile(
    r'(?P<name>.{12}) (?P<style>\S+)\s+(?P<role>attack|defend)\s+(?P<taste>\S+)\s+team(?P<team>\d) '
    r'pos\((?P<x>\S+) (?P<y>\S+) (?P<z>\S+)\) state(?P<state>\d) hp(?P<hp>\S+) guns(?P<guns>\d+) '
    r'goal=(?P<goal>[a-z-]+)\(\S+ \S+ \S+\) path(?P<pi>\d+)/(?P<pn>\d+)(?: cells\d+/\d+)? stuck(?P<stuck>\d+)')


def dist2(ax, az, bx, bz):
    return ((ax - bx) ** 2 + (az - bz) ** 2) ** 0.5


def evaluate(path):
    snaps = 0
    score = {}
    photo = 0
    nav = None
    intel = {}            # team -> (x, z) of the documents that team photographs
    team_line = {}
    rows = []
    worlds = []
    why = {}
    armed = {}
    for line in open(path, errors='replace'):
        if line.startswith('World '):
            worlds.append(line.split(',')[0][6:].strip())
        elif line.startswith('---'):
            snaps += 1
        elif line.startswith('SCORE'):
            m = re.match(r'SCORE team1 (\d+) team2 (\d+), bot photo attempts (\d+)', line)
            if m:
                score = {1: int(m.group(1)), 2: int(m.group(2))}
                photo = int(m.group(3))
        elif line.startswith('NAV '):
            nav = line.strip()
        elif line.startswith('INTEL for team'):
            m = re.match(r'INTEL for team(\d) \S+ \S+ pos\((\S+) \S+ (\S+)\)', line)
            if m:
                intel[int(m.group(1))] = (float(m.group(2)), float(m.group(3)))
        elif re.match(r'TEAM\d ', line):
            team_line[int(line[4])] = line.strip()
        else:
            m = BOT.match(line)
            if m:
                rows.append((snaps, m))
                w = re.search(r' why=(\S+) armed(\d)', line)
                if w:
                    why[(snaps, m['name'])] = w.group(1)
                    armed[(snaps, m['name'])] = int(w.group(2))

    print(f"=== {path}")
    print(f"world(s): {', '.join(sorted(set(worlds))) or '?'}   snapshots: {snaps} (~{snaps * 20 // 60} min)")
    print(f"score: team1 {score.get(1, '?')}  team2 {score.get(2, '?')}   bot photo attempts: {photo}")
    if nav:
        print(nav)
    for t in sorted(team_line):
        print(team_line[t])

    alive = [(s, m) for s, m in rows if m['state'] == '1']
    deaths = Counter()
    prev_state = {}
    for s, m in rows:
        key = m['name'].strip()
        if m['state'] != '1' and prev_state.get(key) == '1':
            deaths[int(m['team'])] += 1
        prev_state[key] = m['state']

    for t in (1, 2):
        mine = [m for s, m in alive if int(m['team']) == t]
        if not mine:
            continue
        goals = Counter(m['goal'] for m in mine)
        line = ", ".join(f"{g} {c}" for g, c in goals.most_common())
        print(f"team{t} goals: {line}")
        whys = Counter(why[(s, m['name'])] for s, m in alive
                       if int(m['team']) == t and (s, m['name']) in why)
        if whys:
            print(f"team{t} why: " + ", ".join(f"{w} {c}" for w, c in whys.most_common()))
        unarmed = sum(1 for s, m in alive if int(m['team']) == t and armed.get((s, m['name'])) == 0)
        if armed:
            print(f"team{t} out of ammo (no usable gun): {unarmed}/{len(mine)}")

        # Progress toward the documents this team photographs (0 = at own base end, 1 = at the docs)
        if t in intel and (3 - t) in intel:
            tx, tz = intel[t]            # the target documents
            hx, hz = intel[3 - t]        # our own documents ~ our base
            span = dist2(tx, tz, hx, hz) or 1.0
            atk = [m for m in mine if m['role'] == 'attack']
            prog = [1.0 - dist2(float(m['x']), float(m['z']), tx, tz) / span for m in atk]
            if prog:
                prog.sort()
                far = sum(1 for p in prog if p > 0.75)
                print(f"team{t} attacker progress toward enemy docs: median {prog[len(prog) // 2]:.2f}, "
                      f"best {prog[-1]:.2f}, snapshots past 75%: {far}/{len(prog)}")
            near = [dist2(float(m['x']), float(m['z']), tx, tz) for m in mine]
            if near:
                close = sum(1 for d in near if d < 300)
                print(f"team{t} closest approach to enemy docs: {min(near):.0f}u, snapshots within 300u: {close}")
        stuck = sum(1 for m in mine if int(m['stuck']) >= 2)
        defend = sum(1 for m in mine if m['role'] == 'defend')
        print(f"team{t} deaths seen: {deaths[t]}   stuck>=2: {stuck}/{len(mine)}   defending: {defend}/{len(mine)}")
    print()


def head_to_head(paths):
    """Each log: one team had every feature, the other had one switched off.
    Report (score with) - (score without), per match and overall."""
    diffs = []
    for path in paths:
        feats, score, photos = None, None, 0
        for line in open(path, errors='replace'):
            m = re.search(r'features t1=([0-9a-f]+) t2=([0-9a-f]+)', line)
            if m:
                feats = (int(m.group(1), 16), int(m.group(2), 16))
            m = re.match(r'SCORE team1 (\d+) team2 (\d+), bot photo attempts (\d+)', line)
            if m:
                score = (int(m.group(1)), int(m.group(2)))
                photos = int(m.group(3))
        if not feats or not score:
            print(f"{path}: incomplete")
            continue
        with_t = 1 if feats[0] > feats[1] else 2
        s_with, s_without = score[with_t - 1], score[2 - with_t]
        diffs.append(s_with - s_without)
        print(f"{path}: team{with_t} has the feature: {s_with} vs {s_without} (diff {s_with - s_without:+d}, photo attempts {photos})")
    if diffs:
        n = len(diffs)
        mean = sum(diffs) / n
        wins = sum(1 for d in diffs if d > 0)
        sd = (sum((d - mean) ** 2 for d in diffs) / (n - 1)) ** 0.5 if n > 1 else 0.0
        se = sd / n ** 0.5 if n > 1 else 0.0
        t = mean / se if se > 0 else 0.0
        verdict = ("HELPS" if t > 2 else "HURTS" if t < -2 else "no clear effect (need more matches?)")
        print(f"\nHEAD-TO-HEAD: {n} matches, team WITH the feature won {wins}, "
              f"mean score diff {mean:+.1f} +/- {se:.1f} (t={t:+.1f}) -> {verdict}")


def ab_tally(path):
    """Per-feature verdict over every head-to-head match the trainer ran."""
    per = defaultdict(list)
    for line in open(path, errors='replace'):
        m = re.match(r'AB (\S+) .*has the feature: (\d+) vs (\d+)', line)
        if m:
            per[m.group(1)].append(int(m.group(2)) - int(m.group(3)))
    for feat, diffs in sorted(per.items()):
        n = len(diffs)
        mean = sum(diffs) / n
        sd = (sum((d - mean) ** 2 for d in diffs) / (n - 1)) ** 0.5 if n > 1 else 0.0
        se = sd / n ** 0.5 if n > 1 else 0.0
        t = mean / se if se > 0 else 0.0
        wins = sum(1 for d in diffs if d > 0)
        verdict = "HELPS" if t > 2 else "HURTS" if t < -2 else "unclear"
        print(f"{feat:12s} {n:3d} matches, won {wins:3d}, mean diff {mean:+6.1f} +/- {se:4.1f} (t={t:+.1f}) {verdict}")


if __name__ == '__main__':
    args = sys.argv[1:]
    if args and args[0] == '--ab':
        head_to_head(args[1:])
    elif args and args[0] == '--abfile':
        ab_tally(args[1])
    else:
        for p in args:
            evaluate(p)
