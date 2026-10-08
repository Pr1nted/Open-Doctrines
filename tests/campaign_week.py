#!/usr/bin/env python3
"""A dedicated server hosting a campaign for a WEEK, with made-up players.

    tests/campaign_week.py --build build --state /tmp/week --hours 168
    tests/campaign_week.py --build build --state /tmp/week --leg-hours 5.3   # one CI leg
    tests/campaign_week.py --build build --state /tmp/smoke --hours 0.25 --turn-seconds 30 \\
                           --kill-every 4 --relay-drop-every 3                  # ten-minute rehearsal

WHAT IT IS FOR

The other campaign tests prove particular things in minutes: a restart keeps
seats (campaign_restart_test.sh), a hundred turns do not leak
(campaign_soak_test.sh). What only time proves is that nothing EXPIRES: a
token, a descriptor, a timer measured in hours, a counter that wraps, a log
that grows until the disk is full. This runs the real server for as long as a
tournament does, against players who behave the way people do, and records
every way it could have let them down.

THE PLAYERS

    punctual   online most of the day; submits as soon as a turn opens
    evening    comes online once or twice a day; submits a while into the turn
    flaky      drops and reconnects every few minutes, all day
    absent     claims a country on day one and never comes back (the AI plays it)
    returner   vanishes for most of a day at a time, then comes back
    visitors   strangers who drop in to watch and leave

THE WEATHER

    power cuts   SIGKILL at random intervals, then a pause before it comes back
    relay drops  the relay hangs up on the host (the server must redial)

WHAT FAILS IT -- each is recorded with the time it happened

    the server exits when nobody killed it
    resident memory above --memory-mb (default 256: half a free 512 MB host)
    a turn number repeats, is skipped, or a restart resumes the wrong turn
    a turn resolves later than its deadline + grace + slack while the server was up
    a player is handed a country that is not theirs after reconnecting
    a late joiner's world over 4 MB (half the relay's frame)
    "appendTurn failed" in the log
    a player cannot get back in for more than --rejoin-minutes while the server is up

CHAINING

Everything lives in --state: the world, the server's config, the harness's own
notes. Run it again with the same --state and it carries on where it stopped
-- the stop being, as far as the server knows, one more power cut. That is how
a CI runner with a six-hour limit hosts a seven-day game: one leg at a time
(.github/workflows/week-soak.yml). `--leg-hours` ends this run early; the
total is still --hours from the very first start.

The exit code: 0 the whole run is over and passed, 1 something failed, 3 this
leg passed and more time remains.
"""

import argparse
import json
import os
import random
import re
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def now():
    return time.time()


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def rss_mb(pid):
    try:
        out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True)
        return int(out.stdout.strip() or 0) // 1024
    except Exception:
        return 0


class Week:
    def __init__(self, a):
        self.a = a
        self.state_dir = os.path.abspath(a.state)
        self.data = os.path.join(self.state_dir, "data")
        self.cfg = os.path.join(self.state_dir, "server.json")
        self.notes_path = os.path.join(self.state_dir, "week.json")
        self.metrics_path = os.path.join(self.state_dir, "metrics.csv")
        self.events_path = os.path.join(self.state_dir, "events.log")
        self.srv_bin = os.path.join(a.build, "OpenDoctrinesServer")
        self.cli_bin = os.path.join(a.build, "CampaignClient")
        # Re-entrant: event() takes it, and is called from places that
        # already hold it.
        self.lock = threading.RLock()
        self.stop = threading.Event()
        self.server = None
        self.server_up_since = 0.0
        self.server_down_since = 0.0
        self.killed_on_purpose = False
        self.issuer = None
        self.issuer_proc = None
        self.notes = {}
        self.log_offset = 0

    # ── bookkeeping ──

    def load_notes(self):
        if os.path.exists(self.notes_path):
            with open(self.notes_path) as f:
                self.notes = json.load(f)
        else:
            self.notes = {
                "started": now(), "hours": self.a.hours, "legs": [],
                "countries": {}, "last_turn": 0, "turn_times": [],
                "violations": [], "max_rss_mb": 0, "max_snapshot": 0,
                "kills": 0, "relay_drops": 0, "rejoins": 0, "visitors": 0,
                "turn_seconds": self.a.turn_seconds,
            }

    def save_notes(self):
        tmp = self.notes_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.notes, f, indent=1)
        os.replace(tmp, self.notes_path)

    def event(self, text):
        line = time.strftime("%Y-%m-%d %H:%M:%S") + "  " + text
        print(line, flush=True)
        with self.lock, open(self.events_path, "a") as f:
            f.write(line + "\n")

    def violation(self, text):
        with self.lock:
            self.notes["violations"].append({"at": now(), "what": text})
        self.event("VIOLATION: " + text)

    # ── the world ──

    def setup(self):
        first = not os.path.exists(self.cfg)
        os.makedirs(os.path.join(self.data, "saves"), exist_ok=True)
        for name in os.listdir(os.path.join(ROOT, "data")):
            if name in ("account.json", "config.json", "servers.json", "saves", "tools", "mods.json", "mods"):
                continue
            dst = os.path.join(self.data, name)
            if not os.path.lexists(dst):
                os.symlink(os.path.join(ROOT, "data", name), dst)
        if first:
            subprocess.run([self.srv_bin, "--write-config", "--config", self.cfg, "--data", self.data],
                           capture_output=True)
            self.edit_cfg({
                "map": '"%s"' % self.a.map, "turn-seconds": str(self.a.turn_seconds),
                "resume-grace-seconds": "60", "tunnel": '"off"', "relay": "true",
                # Everyone claims before the start: a player who arrives after
                # it is a spectator, and the week is about players.
                "auto.start-at-players": str(self.a.players),
                "auto.start-min-players": str(self.a.players),
                "late-join": '"spectate"', "max-players": "16",
                "voice-link": '"https://discord.gg/odweektest"',
            })
            self.event("new campaign in %s (%s, %ss turns, %s hours)"
                       % (self.state_dir, self.a.map, self.a.turn_seconds, self.a.hours))

    def edit_cfg(self, kv):
        with open(self.cfg) as f:
            s = f.read()
        for k, v in kv.items():
            s, n = re.subn(r'"%s": [^,\n]+' % re.escape(k), '"%s": %s' % (k, v), s)
            if n != 1:
                raise SystemExit("no setting %s" % k)
        with open(self.cfg, "w") as f:
            f.write(s)

    def start_issuer(self):
        log = open(os.path.join(self.state_dir, "issuer.log"), "a")
        self.issuer_proc = subprocess.Popen(["node", os.path.join(ROOT, "tests", "mock_issuer.mjs"),
                                             "--port", "0"], stdout=subprocess.PIPE, stderr=log, text=True)
        line = self.issuer_proc.stdout.readline()
        m = re.search(r"(http://localhost:\d+)", line)
        if not m:
            raise SystemExit("the stand-in issuer did not start: " + line)
        self.issuer = m.group(1)
        # A new issuer each leg, on a new port: the config and the account
        # follow it, as they would a real service that moved.
        with open(os.path.join(self.data, "config.json"), "w") as f:
            json.dump({"accountIssuer": self.issuer, "accountAgreed": True,
                       "serverCredential": "mock-server-credential"}, f)
        with open(os.path.join(self.data, "account.json"), "w") as f:
            json.dump({"issuer": self.issuer, "token": "dev-host"}, f)

    def issuer_stats(self):
        try:
            with urllib.request.urlopen(self.issuer + "/session-stats", timeout=5) as r:
                return json.loads(r.read())
        except Exception:
            return {}

    def start_server(self):
        log = open(os.path.join(self.state_dir, "server.log"), "a")
        env = dict(os.environ)
        # Renew the session every 20 minutes instead of 12 hours, so a leg
        # sees it happen many times; the cadence is the only thing compressed.
        env["OD_SESSION_RENEW_SECONDS"] = str(self.a.renew_seconds)
        self.server = subprocess.Popen([self.srv_bin, "--config", self.cfg, "--data", self.data],
                                       stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                       env=env)
        self.server_up_since = now()
        self.server_down_since = 0.0
        self.killed_on_purpose = False
        deadline = now() + 180
        while now() < deadline:
            if self.server.poll() is not None:
                self.violation("the server exited while starting (code %s)" % self.server.returncode)
                return False
            if "session open" in self.read_new_log(peek=True):
                return True
            time.sleep(0.5)
        self.violation("the server did not open a session within 3 minutes")
        return False

    def read_new_log(self, peek=False):
        path = os.path.join(self.state_dir, "server.log")
        try:
            with open(path, errors="replace") as f:
                f.seek(self.log_offset)
                text = f.read()
                if not peek:
                    self.log_offset = f.tell()
                return text
        except FileNotFoundError:
            return ""

    def kill_server(self, reason, polite=False):
        if not self.server or self.server.poll() is not None:
            return
        self.killed_on_purpose = True
        self.server.send_signal(signal.SIGTERM if polite else signal.SIGKILL)
        try:
            self.server.wait(timeout=120)
        except subprocess.TimeoutExpired:
            self.server.kill()
            self.violation("the server ignored SIGTERM for two minutes")
        self.server_down_since = now()
        self.event("server %s (%s)" % ("stopped" if polite else "KILLED", reason))

    # ── the log, read as it grows ──

    def watch_log(self):
        text = self.read_new_log()
        for line in text.splitlines():
            m = re.search(r"turn (\d+) resolved", line)
            if m:
                n = int(m.group(1))
                last = self.notes["last_turn"]
                if last and n != last + 1:
                    self.violation("turn %d resolved after turn %d" % (n, last))
                times = self.notes["turn_times"]
                t = now()
                if times and self.server_up_since and times[-1] > self.server_up_since:
                    gap = t - times[-1]
                    limit = self.a.turn_seconds + 60 + self.a.slack
                    if gap > limit:
                        self.violation("turn %d took %.0fs while the server was up (limit %ds)"
                                       % (n, gap, limit))
                times.append(t)
                del times[:-500]
                self.notes["last_turn"] = n
            m = re.search(r"Resumed the campaign at turn (\d+)", line)
            if m:
                k = int(m.group(1))
                if self.notes["last_turn"] and k != self.notes["last_turn"] + 1:
                    self.violation("resumed at turn %d, but turn %d was the last resolved"
                                   % (k, self.notes["last_turn"]))
            if "appendTurn failed" in line:
                self.violation("a turn could not be saved: " + line.strip())

    # ── the players ──

    def client(self, token, extra, seconds):
        args = [self.cli_bin, "--issuer", self.issuer, "--address", "", "--code", "TEST-GAME",
                "--token", token, "--seconds", str(int(seconds))] + extra
        return subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    def player(self, name, kind, index):
        rnd = random.Random(hash((name, self.a.seed)) & 0xFFFFFFFF)
        claimed = name in self.notes["countries"]
        scale = self.a.time_scale
        while not self.stop.is_set():
            # How long this sitting lasts, and how long until the next.
            if kind == "punctual":
                online, away = rnd.uniform(1800, 4 * 3600), rnd.uniform(60, 1800)
            elif kind == "evening":
                online, away = rnd.uniform(900, 3600), rnd.uniform(4 * 3600, 10 * 3600)
            elif kind == "flaky":
                online, away = rnd.uniform(60, 600), rnd.uniform(5, 120)
            elif kind == "returner":
                online, away = rnd.uniform(1800, 3600), rnd.uniform(12 * 3600, 20 * 3600)
            else:  # absent: claim once, then gone for good
                online, away = 120, 10 ** 9
            online, away = max(20, online * scale), max(5, away * scale)

            # ONE SITTING: online until it ends, reconnecting whenever the
            # connection drops. "Cannot get back in" is timed from the first
            # failed attempt in it -- never from when the player last left,
            # which for an evening player is ten hours of choosing not to play.
            sitting_end = now() + online
            trying_since = None
            while not self.stop.is_set() and now() < sitting_end:
                if self.server is None or self.server.poll() is not None:
                    trying_since = None   # nobody can get in while it is down
                    self.stop.wait(5)
                    continue
                extra = ["--marker", name]
                if not claimed:
                    extra += ["--claim-index", str(index)]
                if kind != "absent" or not claimed:
                    delay = rnd.uniform(0, self.a.turn_seconds * 0.6) \
                        if kind in ("evening", "returner") else 0
                    extra += ["--submit-after", "%.0f" % delay]
                started = now()
                p = self.client("dev-" + name, extra, max(10, sitting_end - now()))
                got_in = False
                for line in p.stdout:
                    line = line.strip()
                    if line.startswith("WELCOMED"):
                        got_in = True
                        # Every time back in after the first: a reconnection
                        # through a drop, a power cut or a night away.
                        with self.lock:
                            if name in self.notes["countries"]:
                                self.notes["rejoins"] += 1
                        trying_since = None
                    m = re.match(r"ROSTER me=(\d+)", line)
                    if m and int(m.group(1)) != 0:
                        cid = int(m.group(1))
                        with self.lock:
                            mine = self.notes["countries"].get(name)
                            if mine is None:
                                self.notes["countries"][name] = cid
                                claimed = True
                                self.event("%s (%s) holds country %d" % (name, kind, cid))
                            elif mine != cid:
                                self.violation("%s came back holding %d, not their %d"
                                               % (name, cid, mine))
                    if self.stop.is_set():
                        p.terminate()
                p.wait()
                if not got_in:
                    if trying_since is None:
                        trying_since = started
                    elif now() - trying_since > self.a.rejoin_minutes * 60 and \
                            now() - self.server_up_since > 120:
                        self.violation("%s could not get in for %d minutes with the server up"
                                       % (name, (now() - trying_since) // 60))
                        trying_since = now()
                elif trying_since is None:
                    # In, then dropped (a power cut, a relay drop): the next
                    # attempt starts the clock afresh.
                    pass
                # Back in a few seconds, as a player whose game reconnects.
                self.stop.wait(min(10, max(0, sitting_end - now())))
            if kind == "absent" and claimed:
                return
            self.stop.wait(away)

    def visitor_loop(self):
        n = 0
        while not self.stop.wait(self.a.visit_every * 60):
            if not self.server or self.server.poll() is not None:
                continue
            n += 1
            p = self.client("visitor-%d-%d" % (int(now()), n), ["--until", "SNAPSHOT"], 120)
            out, _ = p.communicate()
            m = re.search(r"SNAPSHOT turn=\d+ bytes=(\d+)", out or "")
            if m:
                size = int(m.group(1))
                with self.lock:
                    self.notes["visitors"] += 1
                    self.notes["max_snapshot"] = max(self.notes["max_snapshot"], size)
                if size > 4 * 1024 * 1024:
                    self.violation("a late joiner's world is %d bytes" % size)

    # ── the run ──

    def run(self):
        self.load_notes()
        self.setup()
        self.start_issuer()
        leg = {"started": now(), "kills": 0, "relay_drops": 0}
        self.notes["legs"].append(leg)
        self.event("leg %d begins, %.1f of %.1f hours done"
                   % (len(self.notes["legs"]), (now() - self.notes["started"]) / 3600, self.notes["hours"]))
        # The previous leg ended in a stop the server did not see coming.
        self.log_offset = os.path.getsize(os.path.join(self.state_dir, "server.log")) \
            if os.path.exists(os.path.join(self.state_dir, "server.log")) else 0
        if not self.start_server():
            self.finish(leg)
            return 1

        kinds = ["punctual", "evening", "flaky", "absent", "returner", "punctual"][:self.a.players]
        threads = [threading.Thread(target=self.player, args=("p%d" % i, k, i), daemon=True)
                   for i, k in enumerate(kinds)]
        threads.append(threading.Thread(target=self.visitor_loop, daemon=True))
        for t in threads:
            t.start()

        end_total = self.notes["started"] + self.notes["hours"] * 3600
        end_leg = now() + self.a.leg_hours * 3600 if self.a.leg_hours else end_total
        next_kill = now() + random.uniform(0.5, 1.5) * self.a.kill_every * 60
        next_drop = now() + random.uniform(0.5, 1.5) * self.a.relay_drop_every * 60
        next_sample = now()
        with open(self.metrics_path, "a") as metrics:
            if metrics.tell() == 0:
                metrics.write("time,turn,rss_mb,save_kb,reopens,refreshes,violations\n")
            while now() < min(end_total, end_leg):
                time.sleep(2)
                self.watch_log()
                if self.server.poll() is not None:
                    if not self.killed_on_purpose:
                        self.violation("the server exited by itself (code %s)" % self.server.returncode)
                    # Back after the "power cut" -- or straight away if it fell over.
                    if now() - self.server_down_since >= self.a.down_seconds or not self.killed_on_purpose:
                        self.event("server starting again")
                        if not self.start_server():
                            break
                elif now() >= next_kill:
                    self.kill_server("a power cut")
                    leg["kills"] += 1
                    self.notes["kills"] += 1
                    next_kill = now() + random.uniform(0.5, 1.5) * self.a.kill_every * 60
                elif now() >= next_drop:
                    try:
                        urllib.request.urlopen(urllib.request.Request(
                            self.issuer + "/relay-drop/TEST-GAME", method="POST"), timeout=5).read()
                        leg["relay_drops"] += 1
                        self.notes["relay_drops"] += 1
                        self.event("the relay hung up on the host")
                    except Exception as e:
                        self.event("could not drop the relay: %s" % e)
                    next_drop = now() + random.uniform(0.5, 1.5) * self.a.relay_drop_every * 60
                if now() >= next_sample and self.server.poll() is None:
                    next_sample = now() + 60
                    rss = rss_mb(self.server.pid)
                    self.notes["max_rss_mb"] = max(self.notes["max_rss_mb"], rss)
                    if rss > self.a.memory_mb:
                        self.violation("the server is using %d MB" % rss)
                    saves = os.path.join(self.data, "saves")
                    size = sum(os.path.getsize(os.path.join(saves, f)) for f in os.listdir(saves)) // 1024
                    st = self.issuer_stats()
                    metrics.write("%d,%d,%d,%d,%s,%s,%d\n" % (
                        now(), self.notes["last_turn"], rss, size, st.get("reopens", ""),
                        st.get("refreshes", ""), len(self.notes["violations"])))
                    metrics.flush()
                    self.save_notes()

        self.stop.set()
        done = now() >= end_total
        # The end of a leg is one more power cut, from the server's point of
        # view; the end of the week is a polite stop.
        self.kill_server("end of the week" if done else "end of this leg", polite=done)
        self.watch_log()
        for t in threads:
            t.join(timeout=10)
        self.finish(leg)
        if self.notes["violations"]:
            return 1
        return 0 if done else 3

    def finish(self, leg):
        st = self.issuer_stats()
        leg["ended"] = now()
        leg["issuer"] = st
        leg_hours = (leg["ended"] - leg["started"]) / 3600
        # Things that must have happened at least once in a long enough leg.
        if leg_hours * 3600 > self.a.renew_seconds * 2 and st.get("reopens", 0) < 1:
            self.violation("the session was never renewed in a %.1f-hour leg" % leg_hours)
        if leg_hours > 1.5 and st.get("refreshes", 0) < 1:
            self.violation("the account token was never refreshed in a %.1f-hour leg" % leg_hours)
        if self.issuer_proc:
            self.issuer_proc.terminate()
        self.save_notes()
        self.write_report()

    def write_report(self):
        n = self.notes
        hours = (now() - n["started"]) / 3600
        lines = [
            "# Week soak: %s" % ("FAILED" if n["violations"]
                                 else "PASSED" if hours >= n["hours"] else "PASSED so far"),
            "",
            "| | |", "|---|---|",
            "| hours played | %.1f of %.1f |" % (hours, n["hours"]),
            "| legs | %d |" % len(n["legs"]),
            "| turns resolved | %d (%ss each) |" % (n["last_turn"], n["turn_seconds"]),
            "| power cuts | %d |" % n["kills"],
            "| relay drops | %d |" % n["relay_drops"],
            "| players reconnecting | %d |" % n["rejoins"],
            "| visitors | %d |" % n["visitors"],
            "| peak memory | %d MB |" % n["max_rss_mb"],
            "| largest late-joiner world | %d KB |" % (n["max_snapshot"] // 1024),
            "| players and their countries | %s |" % ", ".join("%s=%s" % kv for kv in sorted(n["countries"].items())),
            "",
        ]
        if n["violations"]:
            lines.append("## What went wrong")
            lines.append("")
            for v in n["violations"]:
                lines.append("- %s -- %s" % (time.strftime("%Y-%m-%d %H:%M", time.localtime(v["at"])), v["what"]))
        with open(os.path.join(self.state_dir, "report.md"), "w") as f:
            f.write("\n".join(lines) + "\n")
        print("\n".join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    ap.add_argument("--state", required=True)
    ap.add_argument("--hours", type=float, default=168.0, help="the whole run, from its first start")
    ap.add_argument("--leg-hours", type=float, default=0.0, help="stop this run after this long; 0 = never")
    ap.add_argument("--map", default="1914")
    ap.add_argument("--turn-seconds", type=int, default=3600)
    ap.add_argument("--players", type=int, default=6)
    ap.add_argument("--kill-every", type=float, default=180, help="minutes between power cuts, on average")
    ap.add_argument("--down-seconds", type=int, default=45, help="how long a power cut lasts")
    ap.add_argument("--relay-drop-every", type=float, default=120, help="minutes between relay drops")
    ap.add_argument("--visit-every", type=float, default=30, help="minutes between visitors")
    ap.add_argument("--renew-seconds", type=int, default=1200)
    ap.add_argument("--memory-mb", type=int, default=256)
    ap.add_argument("--rejoin-minutes", type=int, default=15)
    ap.add_argument("--slack", type=int, default=300, help="seconds a turn may run past its deadline")
    ap.add_argument("--time-scale", type=float, default=1.0,
                    help="multiply players' online/away times (0.01 for a rehearsal)")
    ap.add_argument("--seed", type=int, default=20261003)
    a = ap.parse_args()
    random.seed(a.seed + int(time.time() // 3600))
    for b in ("OpenDoctrinesServer", "CampaignClient"):
        if not os.access(os.path.join(a.build, b), os.X_OK):
            sys.exit("build %s first (cmake --build %s --target %s)" % (b, a.build, b))
    return Week(a).run()


if __name__ == "__main__":
    sys.exit(main())
