#!/usr/bin/env python3
"""Global concurrency gate for OpenDoctrines game processes.

Every instance spikes to ~2 GB while the map loads, then settles near
50 MB. Sizing anything on the settled figure is what repeatedly froze a
16 GB machine: the pool's worker, its periodic bench, a stray eval and a
recording ran at once, and four 2 GB spikes do not fit alongside a
browser. train_parallel's --max-rss-gb could not prevent it -- it was set
above the spike, so it never fired, and it watched only workers, not the
bench and eval subprocesses the pool itself launches.

The gate is a counting semaphore held across exec, so it covers every
launcher (pool worker, bench, eval, recording driver) without any of them
having to cooperate with each other:

    python3 tools/odlock.py -- ./build/.../OpenDoctrines --eval-ai ...

The lock lives on an inherited fd, so it is released by the kernel when
the process dies -- including SIGKILL, which is how these get stopped.
"""
import argparse, fcntl, os, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCKDIR = os.path.join(ROOT, "data", "ai", ".odlocks")

# Two concurrent instances ~= 4 GB of peak, which leaves room for the
# user's own applications on a 16 GB machine. Raise only with headroom
# measured, not assumed.
DEFAULT_SLOTS = int(os.environ.get("OD_MAX_GAMES", "2"))


def acquire(slots, timeout, label):
    os.makedirs(LOCKDIR, exist_ok=True)
    waited, warned = 0.0, False
    while True:
        for i in range(slots):
            fd = os.open(os.path.join(LOCKDIR, f"slot{i}.lock"),
                         os.O_CREAT | os.O_RDWR, 0o644)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                os.close(fd)
                continue
            # Survive the exec below: flock is held per open-file-
            # description, so the child keeps it as long as the fd stays
            # open and is not close-on-exec.
            flags = fcntl.fcntl(fd, fcntl.F_GETFD)
            fcntl.fcntl(fd, fcntl.F_SETFD, flags & ~fcntl.FD_CLOEXEC)
            os.set_inheritable(fd, True)
            os.write(fd, f"{os.getpid()} {label}\n".encode())
            return fd
        if timeout and waited >= timeout:
            sys.stderr.write(f"odlock: no slot after {timeout:.0f}s ({slots} busy)\n")
            sys.exit(75)
        if not warned:
            sys.stderr.write(f"odlock: all {slots} slots busy, waiting...\n")
            warned = True
        time.sleep(2.0)
        waited += 2.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slots", type=int, default=DEFAULT_SLOTS)
    ap.add_argument("--timeout", type=float, default=0.0,
                    help="0 waits forever")
    ap.add_argument("--label", default="")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.cmd[1:] if a.cmd and a.cmd[0] == "--" else a.cmd
    if not cmd:
        ap.error("no command given")
    acquire(max(1, a.slots), a.timeout, a.label or os.path.basename(cmd[0]))
    os.execvp(cmd[0], cmd)


if __name__ == "__main__":
    main()
