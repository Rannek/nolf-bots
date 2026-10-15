#!/usr/bin/env python3
"""Train the NOLF bots on Hydro by self-play evolution.

    python3 -I hydro_evolve.py [--servers 16] [--speed 4] [--round 60] [--bots 12]

Runs N invisible dedicated servers (Xvfb) on Hydro, all at once, faster than
real time.  Every server plays continuous short rounds; for each round this
script assigns two genomes (BotGenome.h: the bots' fighting parameters) to the
two teams.  New challengers are mutations of the current champion and play it
with sides swapped; a challenger that beats the champion convincingly becomes
the new champion, which is installed as NOLF/Bots/genome.txt -- what the bots
use in real games.

Stop:   touch NOLF/Bots/train/STOP      (or send SIGTERM)
Status: NOLF/Bots/train/status.txt, history: NOLF/Bots/train/history.txt
Pauses itself while the game (NOLF.exe / lithtech.exe) is running.
Only server 0 saves what it learns about the map (Bots/07HYDRO_AM.nav).
"""
import argparse
import math
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import time

PREFIX = os.environ.get('NOLF_PREFIX', os.path.expanduser('~/Games/nolf-1'))
GAME = PREFIX + '/drive_c/Games/NOLF'
SRC = PREFIX + '/drive_c/NOLFSource'
TRAIN = os.path.join(GAME, 'Bots', 'train')
MAP = r'Worlds\Multi\AssaultMap\07HYDRO_AM'
XDISPLAY = ':91'

# Must match g_aGeneInfo in BotMgr.cpp: name, default, min, max, binary
GENES = [
    ('react', 1.0, 0.6, 1.6, False), ('aimerr', 1.0, 0.6, 1.6, False), ('aimmove', 1.0, 0.5, 1.6, False),
    ('turn', 1.0, 0.7, 2.0, False), ('strafe', 1.0, 0.4, 2.5, False), ('jump', 1.0, 0.0, 2.0, False),
    ('range', 1.0, 0.5, 2.0, False), ('retreat', 1.0, 0.3, 2.0, False), ('chase', 1.0, 0.2, 2.5, False),
    ('aggro', 1.0, 0.3, 2.0, False), ('burst', 1.0, 0.5, 2.5, False), ('head', 1.0, 0.0, 2.0, False),
    ('effrange', 1.0, 0.6, 2.5, False), ('greed', 1.0, 0.3, 2.5, False), ('heal', 1.0, 0.3, 3.0, False),
    ('role', 0.92, 0.3, 1.0, False), ('cover', 1.0, 0.0, 1.0, False), ('fov', 1.0, 0.6, 1.4, False),
    ('dodge', 1.0, 0.0, 2.5, False), ('lead', 1.0, 0.0, 1.5, False),
    ('waves', 1.0, 0.0, 1.0, True), ('danger', 1.0, 0.0, 1.0, True), ('callouts', 1.0, 0.0, 1.0, True),
    ('discipline', 1.0, 0.0, 1.0, True), ('focus', 1.0, 0.0, 1.0, True), ('recall', 1.0, 0.0, 1.0, True),
]

UMU_ENV = {
    'WINEPREFIX': PREFIX,
    'PROTONPATH': os.path.expanduser('~/.local/share/Steam/compatibilitytools.d/GE-Proton11-7-x86_64'),
    'GAMEID': 'umu-default', 'STORE': 'none',
    'WINEESYNC': '0', 'WINEFSYNC': '0', 'PROTON_NO_ESYNC': '1', 'PROTON_NO_FSYNC': '1',
    'WINEDLLOVERRIDES': 'dmband,dmcompos,dmime,dmloader,dmscript,dmstyle,dmsynth,dmusic,dmusic32,dswave=n;dsound=b',
    'PROTON_VERB': 'run',      # start inside the running Proton session (parallel servers)
}


def session_env(run_verb):
    """Environment for umu-run: invisible display, our Proton."""
    env = dict(os.environ)
    env.update(UMU_ENV)
    if not run_verb:
        env.pop('PROTON_VERB', None)        # the first launch opens the Proton session
    env.pop('WAYLAND_DISPLAY', None)
    env['DISPLAY'] = XDISPLAY
    return env


def log(msg):
    line = time.strftime('%m-%d %H:%M:%S ') + msg
    print(line, flush=True)
    with open(os.path.join(TRAIN, 'history.txt'), 'a') as f:
        f.write(line + '\n')


# ---------------------------------------------------------------- genomes --

def default_genome():
    return {n: d for n, d, lo, hi, b in GENES}


def read_genome(path):
    g = default_genome()
    try:
        for line in open(path):
            p = line.split()
            if len(p) == 2 and p[0] in g:
                g[p[0]] = float(p[1])
    except OSError:
        pass
    return g


def write_genome(g, path):
    tmp = path + '.tmp'
    with open(tmp, 'w') as f:
        for n, d, lo, hi, b in GENES:
            f.write(f'{n} {g[n]:.4f}\n')
    os.replace(tmp, path)       # atomic: the game never reads half a file


def mutate(g, strength):
    c = dict(g)
    for n, d, lo, hi, b in GENES:
        if b:
            if random.random() < 0.06:
                c[n] = 0.0 if c[n] >= 0.5 else 1.0
        elif random.random() < 0.35:
            c[n] = min(hi, max(lo, c[n] + random.gauss(0.0, strength * (hi - lo))))
    return c


class Pool:
    """Genome files on disk (immutable, id = file name)."""
    def __init__(self):
        os.makedirs(os.path.join(TRAIN, 'genomes'), exist_ok=True)
        existing = [int(m.group(1)) for f in os.listdir(os.path.join(TRAIN, 'genomes'))
                    for m in [re.match(r'g(\d+)\.txt$', f)] if m]
        self.next = max(existing, default=0) + 1

    def add(self, g):
        gid = f'g{self.next:06d}.txt'
        self.next += 1
        write_genome(g, os.path.join(TRAIN, 'genomes', gid))
        return gid

    @staticmethod
    def winpath(gid):
        return 'Bots\\train\\genomes\\' + gid


# ---------------------------------------------------------------- servers --

class Server:
    def __init__(self, idx, args):
        self.idx = idx
        self.port = 27999 + idx
        self.dir = os.path.join(TRAIN, f's{idx:02d}')
        os.makedirs(self.dir, exist_ok=True)
        self.windir = f'Bots\\train\\s{idx:02d}'
        self.args = args
        self.pos = 0                # read offset in results.txt
        self.next_round = 1
        self.job = None             # (candidate, challenger_side) for next_round
        self.last_result = time.time()
        self.rounds = 0

    def results_path(self):
        return os.path.join(self.dir, 'results.txt')

    def assign(self, round_no, t1, t2, job):
        with open(os.path.join(self.dir, 'assign.txt.tmp'), 'w') as f:
            f.write(f'round {round_no}\nt1 {Pool.winpath(t1)}\nt2 {Pool.winpath(t2)}\n')
        os.replace(os.path.join(self.dir, 'assign.txt.tmp'), os.path.join(self.dir, 'assign.txt'))
        self.job = job

    def prepare(self):
        """Fresh files and this server's launch lines (its own settings)."""
        os.makedirs(self.dir, exist_ok=True)
        for f in ('results.txt', 'botlog.txt'):
            try:
                os.remove(os.path.join(self.dir, f))
            except OSError:
                pass
        self.pos = 0
        self.next_round = 1
        self.last_result = time.time()
        lines = [
            f'set NOLFBOT_SELFPLAY={self.args.bots}',
            f'set NOLFBOT_TIMESCALE={self.args.speed}',
            f'set NOLFBOT_ROUND={self.args.round}',
            f'set NOLFBOT_TRAIN_DIR={self.windir}',
            f'set NOLFBOT_LOG={self.windir}\\botlog.txt',
            f'set NOLFBOT_NOSAVE={"0" if self.idx == 0 else "1"}',     # one server learns the map
            f'start "HydroTrain{self.idx}" NolfServ.exe -nowiz -NetPort {self.port} -NetSessionName HydroTrain{self.idx}',
        ]
        with open(os.path.join(self.dir, 'launch.bat'), 'w', newline='') as f:
            f.write('@echo off\r\ncd /d C:\\Games\\NOLF\r\n' + '\r\n'.join(lines) + '\r\n')
        return lines

    def alive(self):
        return subprocess.run(['pgrep', '-f', f'NolfServ\\.exe -nowiz -NetPort {self.port} '],
                              stdout=subprocess.DEVNULL).returncode == 0

    def restart_alone(self, give):
        """Relaunch just this server, inside the running Proton session."""
        self.stop()
        time.sleep(2)
        self.prepare()
        give(self)
        out = open(os.path.join(self.dir, 'server.out'), 'w')
        subprocess.Popen(['nice', '-n', '10', 'umu-run', 'cmd.exe', '/c', self.windir + '\\launch.bat'],
                         cwd=GAME, env=session_env(run_verb=True), stdout=out, stderr=subprocess.STDOUT,
                         start_new_session=True)

    def stop(self):
        subprocess.run(['pkill', '-f', f'[N]olfServ\\.exe -nowiz -NetPort {self.port} '], check=False)

    def new_results(self):
        out = []
        try:
            with open(self.results_path()) as f:
                f.seek(self.pos)
                for line in f:
                    if not line.endswith('\n'):
                        break
                    self.pos += len(line)
                    m = re.match(r'R (\d+) t1 (\S+) t2 (\S+) kills (\d+) (\d+) photos (\d+) (\d+)', line)
                    if m:
                        out.append((int(m.group(1)), m.group(2), m.group(3), int(m.group(4)),
                                    int(m.group(5)), int(m.group(6)), int(m.group(7))))
        except OSError:
            pass
        return out


# --------------------------------------------------------------- the files --

def setup_files():
    """startup.txt lists our rez files; NetHost.txt runs Hydro.  Backed up once."""
    bk = os.path.join(TRAIN, 'backup')
    os.makedirs(bk, exist_ok=True)
    for f in ('startup.txt', 'NetHost.txt'):
        src = os.path.join(GAME, f)
        if os.path.exists(src) and not os.path.exists(os.path.join(bk, f)):
            shutil.copy2(src, os.path.join(bk, f))
    orig = os.path.join(GAME, 'startup.txt.orig-2001')
    if not os.path.exists(orig):
        orig = os.path.join(bk, 'startup.txt')
    subprocess.run([sys.executable, '-I', os.path.join(SRC, 'linux', 'selfplay_startup.py'), orig,
                    os.path.join(GAME, 'startup.txt'), 'Custom\\MODERNIZER.REZ', 'Custom\\BOTS.REZ'], check=True)
    nh = os.path.join(GAME, 'NetHost.txt')
    text = open(nh, newline='').read()
    for name, val in (('NetGameType', '1.000000f'), ('NetCANumLevels', '1.000000f'),
                      ('NetCALevel0', f'"{MAP}"'), ('NetEndTime', '60.000000f'), ('NetCAGameEnd', '1.000000f')):
        text = re.sub(rf'^{name} = .*?(\r?)$', lambda m: f'{name} = {val}{m.group(1)}', text, flags=re.M)
    open(nh, 'w', newline='').write(text)


def restore_files():
    bk = os.path.join(TRAIN, 'backup')
    for f in ('startup.txt', 'NetHost.txt'):
        if os.path.exists(os.path.join(bk, f)):
            shutil.copy2(os.path.join(bk, f), os.path.join(GAME, f))


def pack_bots_rez():
    lto = os.path.join(GAME, 'BotsRez', 'Object.lto')
    rez = os.path.join(GAME, 'Custom', 'BOTS.REZ')
    if os.path.exists(rez) and os.path.getmtime(rez) >= os.path.getmtime(lto):
        return
    try:
        os.remove(rez)
    except OSError:
        pass
    env = dict(os.environ)
    env.update(UMU_ENV)
    env.pop('PROTON_VERB', None)
    subprocess.run(['umu-run', os.path.join(SRC, 'TOOLS', 'lithrez.exe'), 'c', 'Custom\\BOTS.REZ', 'BotsRez'],
                   cwd=GAME, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=180)
    if not os.path.exists(rez):
        raise SystemExit("couldn't build Custom/BOTS.REZ")


def game_running():
    r = subprocess.run(['pgrep', '-f', r'[l]ithtech\.exe|[N]OLF\.exe'], stdout=subprocess.DEVNULL)
    return r.returncode == 0


def start_xvfb():
    num = XDISPLAY[1:]
    if subprocess.run(['pgrep', '-f', f'^Xvfb :{num} '], stdout=subprocess.DEVNULL).returncode == 0:
        return None
    # Leftovers of a crash (a reboot clears them anyway)
    for f in (f'/tmp/.X11-unix/X{num}', f'/tmp/.X{num}-lock'):
        try:
            os.remove(f)
        except OSError:
            pass
    p = subprocess.Popen(['Xvfb', XDISPLAY, '-screen', '0', '800x600x24', '-nolisten', 'tcp'],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    time.sleep(2)
    return p


# ------------------------------------------------------------- evolution --

class Candidate:
    def __init__(self, gid, genome, parent):
        self.gid, self.genome, self.parent = gid, genome, parent
        self.diffs = []             # challenger minus champion, per round
        self.sides = [0, 0]         # rounds played as team1 / team2
        self.inflight = 0

    def stats(self):
        n = len(self.diffs)
        mean = sum(self.diffs) / n if n else 0.0
        sd = (sum((d - mean) ** 2 for d in self.diffs) / (n - 1)) ** 0.5 if n > 1 else 0.0
        t = mean / (sd / n ** 0.5) if sd > 0 else (5.0 if mean > 0 and n > 1 else 0.0)
        return n, mean, t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--servers', type=int, default=16)
    ap.add_argument('--speed', type=float, default=4.0)
    ap.add_argument('--round', type=int, default=60, help='game seconds per round')
    ap.add_argument('--bots', type=int, default=12)
    ap.add_argument('--strength', type=float, default=0.12, help='mutation size (fraction of range)')
    args = ap.parse_args()

    os.makedirs(TRAIN, exist_ok=True)
    stop_file = os.path.join(TRAIN, 'STOP')
    if os.path.exists(stop_file):
        os.remove(stop_file)
    stopping = [False]
    signal.signal(signal.SIGTERM, lambda *a: stopping.__setitem__(0, True))
    signal.signal(signal.SIGINT, lambda *a: stopping.__setitem__(0, True))

    pool = Pool()
    champ_file = os.path.join(TRAIN, 'champion.txt')
    if os.path.exists(champ_file):
        champion = read_genome(champ_file)
    elif os.path.exists(os.path.join(GAME, 'Bots', 'genome.txt')):
        champion = read_genome(os.path.join(GAME, 'Bots', 'genome.txt'))
    else:
        champion = default_genome()
    champ_id = pool.add(champion)
    generation = 0
    log(f'start: {args.servers} servers x{args.speed}, {args.round}s rounds, champion {champ_id}')

    candidates = []
    servers = [Server(i, args) for i in range(args.servers)]
    xvfb = None
    running = False
    total_rounds = 0
    last_status = 0

    def new_candidate():
        g = mutate(champion, args.strength)
        c = Candidate(pool.add(g), g, champ_id)
        candidates.append(c)
        return c

    def pick_job():
        # The candidate furthest from a decision, side it has played less
        live = [c for c in candidates]
        while len(live) < max(2, args.servers // 2):
            live.append(new_candidate())
        c = min(live, key=lambda c: len(c.diffs) + c.inflight)
        side = 1 if c.sides[0] <= c.sides[1] else 2
        c.inflight += 1
        return c, side

    def give(srv):
        c, side = pick_job()
        t1, t2 = (c.gid, champ_id) if side == 1 else (champ_id, c.gid)
        srv.assign(srv.next_round, t1, t2, (c, side))

    session = [None]

    def launch_all():
        nonlocal xvfb, running
        pack_bots_rez()
        setup_files()
        xvfb = start_xvfb() or xvfb
        # One Proton session starts every server (one launcher, not 16: memory)
        lines = ['@echo off', 'cd /d C:\\Games\\NOLF']
        for s in servers:
            lines += s.prepare()
            give(s)
            lines.append('ping -n 3 127.0.0.1 >nul')
        lines += [':wait', 'ping -n 30 127.0.0.1 >nul', 'timeout /t 30 /nobreak >nul', 'goto wait']
        with open(os.path.join(TRAIN, 'launch.bat'), 'w', newline='') as f:
            f.write('\r\n'.join(lines) + '\r\n')
        out = open(os.path.join(TRAIN, 'session.out'), 'w')
        session[0] = subprocess.Popen(['nice', '-n', '10', 'umu-run', 'cmd.exe', '/c', 'Bots\\train\\launch.bat'],
                                      cwd=GAME, env=session_env(False), stdout=out, stderr=subprocess.STDOUT,
                                      start_new_session=True)
        running = True
        log('servers launching (one Proton session)')

    def stop_all():
        nonlocal running
        for s in servers:
            if s.job:
                s.job[0].inflight = max(0, s.job[0].inflight - 1)
                s.job = None
        subprocess.run(['pkill', '-f', '[N]olfServ\\.exe -nowiz -NetPort'], check=False)
        if session[0]:
            try:
                os.killpg(session[0].pid, signal.SIGTERM)
            except OSError:
                pass
            session[0] = None
        subprocess.run(['pkill', '-f', 'cmd\\.exe /c Bots\\\\train'], check=False)
        time.sleep(3)
        restore_files()
        running = False

    try:
        while not stopping[0] and not os.path.exists(stop_file):
            if game_running():
                if running:
                    log('game started: pausing training')
                    stop_all()
                time.sleep(10)
                continue
            if not running:
                launch_all()

            for s in servers:
                for (rnd, t1, t2, k1, k2, p1, p2) in s.new_results():
                    s.last_result = time.time()
                    s.next_round = rnd + 1
                    total_rounds += 1
                    s.rounds += 1
                    job = s.job
                    if job and job[0] in candidates and {t1, t2} == {job[0].gid, job[0].parent}:
                        c, side = job
                        c.inflight = max(0, c.inflight - 1)
                        if c.parent == champ_id:
                            kc, kh, pc, ph = (k1, k2, p1, p2) if side == 1 else (k2, k1, p2, p1)
                            c.diffs.append((kc - kh) + 3 * (pc - ph))
                            c.sides[side - 1] += 1
                    elif job:
                        job[0].inflight = max(0, job[0].inflight - 1)
                    give(s)

                # A server that stopped reporting gets restarted
                if time.time() - s.last_result > max(300, 6 * args.round / args.speed):
                    log(f'server {s.idx} silent: restarting it')
                    if s.job:
                        s.job[0].inflight = max(0, s.job[0].inflight - 1)
                        s.job = None
                    s.restart_alone(give)

            # Decisions (sequential: cheap rejects, accept only on clear evidence)
            for c in list(candidates):
                n, mean, t = c.stats()
                verdict = None
                if n >= 8 and mean <= 0:
                    verdict = 'rejected'
                elif n >= 8 and t >= 2.5:
                    verdict = 'ACCEPTED'
                elif n >= 16:
                    verdict = 'ACCEPTED' if (mean > 0 and t >= 1.8) else 'rejected'
                if not verdict:
                    continue
                candidates.remove(c)
                log(f'{c.gid}: {verdict} after {n} rounds, mean diff {mean:+.2f} (t={t:+.1f})')
                if verdict == 'ACCEPTED':
                    generation += 1
                    champion, champ_id = c.genome, c.gid
                    write_genome(champion, champ_file)
                    write_genome(champion, os.path.join(GAME, 'Bots', 'genome.txt'))
                    changed = [f'{n}={champion[n]:.2f}' for n, d, lo, hi, b in GENES if abs(champion[n] - d) > 1e-3]
                    log(f'NEW CHAMPION #{generation} {champ_id}: ' + ' '.join(changed))
                    candidates.clear()      # everyone else was measured against the old one

            if time.time() - last_status > 30:
                last_status = time.time()
                alive = sum(1 for s in servers if s.alive())
                fps = []
                for s in servers:
                    try:
                        tail = open(os.path.join(s.dir, 'botlog.txt'), errors='replace').read()[-20000:]
                        m = re.findall(r'^FRAMES .*?= (\d+) per game second', tail, re.M)
                        if m:
                            fps.append(int(m[-1]))
                    except OSError:
                        pass
                fps_txt = f'{min(fps)}-{max(fps)} (normal play ~60)' if fps else '?'
                lines = [time.strftime('%m-%d %H:%M:%S'),
                         f'servers alive {alive}/{len(servers)}, rounds played {total_rounds}, champions so far {generation}',
                         f'simulation frames per game second: {fps_txt}',
                         f'champion {champ_id}: ' + ' '.join(f'{n}={champion[n]:.2f}' for n, *_ in GENES)]
                for c in sorted(candidates, key=lambda c: -len(c.diffs)):
                    n, mean, t = c.stats()
                    lines.append(f'  candidate {c.gid}: {n} rounds, mean {mean:+.2f}, t {t:+.1f}')
                with open(os.path.join(TRAIN, 'status.txt'), 'w') as f:
                    f.write('\n'.join(lines) + '\n')
            time.sleep(1)
    finally:
        log('stopping')
        stop_all()
        if xvfb:
            xvfb.terminate()
        log(f'stopped after {total_rounds} rounds, champion {champ_id}')


if __name__ == '__main__':
    main()
