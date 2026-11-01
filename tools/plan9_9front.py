#!/usr/bin/env python3
"""Boot 9front in QEMU, build the Plan 9 edition there with mk, and play it.

    tools/plan9_9front.py --iso 9front.amd64.iso[.gz] --work build/plan9/guest \\
        --transcript build/plan9/9front.txt [--bin build/plan9/bin]

Headless and scripted, with nothing typed by hand and no VNC:

  1. The kernel (amd64/9pc64) and cfg/plan9.ini are read out of the ISO with
     bsdtar or 7z. Nothing from the ISO is run on this machine.
  2. QEMU boots that kernel directly (-kernel: 9front kernels carry a
     multiboot header) with plan9.ini's lines on the command line, plus
     console=0, so the kernel's console -- and rc on it -- is the first
     serial port, which is this script's pipe. Root is the ISO, as on a live
     CD boot; questions the boot asks are answered with their defaults.
  3. The source tree (tools/plan9_package.py --tree) goes in on a FAT disk
     image made with mkfs.vfat and mtools; 9front's dossrv mounts it.
  4. At the rc prompt: mk in plan9/, then 6.opendoctrines < play.txt, its
     output printed between markers. That is normalised exactly as the other
     editions' transcripts are (tools/freertos_play.py --raw-in).
  5. --bin copies 6.opendoctrines and 6.odmap back off the FAT disk, for the
     release zip.

Exit status 0 when a transcript came back (the caller diffs it), 1 if the
guest did not get that far; build/plan9/guest/serial.log has everything the
guest printed. --timeout bounds the whole run (default 900 s: without KVM,
QEMU emulates the CPU, and 9front still boots in a minute or two).
"""
import argparse
import gzip
import os
import re
import select
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BEGIN, END = "OD-TRANSCRIPT-BEGIN", "OD-TRANSCRIPT-END"


def extract(iso, work):
    """amd64/9pc64 and cfg/plan9.ini out of the ISO, as data."""
    if iso.endswith(".gz"):
        plain = os.path.join(work, "9front.iso")
        if not os.path.exists(plain):
            with gzip.open(iso, "rb") as src, open(plain, "wb") as dst:
                shutil.copyfileobj(src, dst)
        iso = plain
    out = os.path.join(work, "iso")
    os.makedirs(out, exist_ok=True)
    want = ["amd64/9pc64", "cfg/plan9.ini"]
    if shutil.which("bsdtar"):
        subprocess.check_call(["bsdtar", "-xf", iso, "-C", out] + want)
    elif shutil.which("7z"):
        subprocess.check_call(["7z", "x", "-y", "-o" + out, iso] + want, stdout=subprocess.DEVNULL)
    else:
        sys.exit("need bsdtar (libarchive-tools) or 7z (p7zip-full) to read the ISO")
    return iso, os.path.join(out, "amd64", "9pc64"), os.path.join(out, "cfg", "plan9.ini")


def kernel_args(ini):
    """plan9.ini's settings, less anything that would stop and ask."""
    conf = {}
    for line in open(ini, encoding="latin-1"):
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("[") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        if v == "ask" or " " in v:
            continue
        conf[k] = v
    conf.update({
        "console": "0",
        "mouseport": "ps2",
        "monitor": "vesa",
        "vgasize": "text",       # no rio: rc on the console
        "user": "glenda",
        "nobootprompt": "local!/dev/sdD0/data",
        "*nomp": "1",
    })
    conf.pop("bootfile", None)
    return " ".join(f"{k}={v}" for k, v in conf.items())


def fat_image(tree, img):
    if os.path.exists(img):
        os.remove(img)
    subprocess.check_call(["mkfs.vfat", "-C", "-n", "ODSRC", img, str(64 * 1024)],
                          stdout=subprocess.DEVNULL)
    for name in sorted(os.listdir(tree)):
        subprocess.check_call(["mcopy", "-s", "-i", img, os.path.join(tree, name), "::/"])


class Guest:
    def __init__(self, cmd, log):
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
        self.out = b""
        self.log = open(log, "wb")

    def read(self, timeout):
        r, _, _ = select.select([self.p.stdout], [], [], timeout)
        if r:
            chunk = os.read(self.p.stdout.fileno(), 65536)
            if not chunk:
                raise EOFError("QEMU exited")
            self.out += chunk
            self.log.write(chunk)
            self.log.flush()
        elif self.p.poll() is not None:
            raise EOFError("QEMU exited")

    def send(self, s):
        # A serial console takes what it is given slowly; a character at a time
        # with a breath between keeps the UART's FIFO from overrunning.
        for ch in s.encode():
            self.p.stdin.write(bytes([ch]))
            self.p.stdin.flush()
            time.sleep(0.005)

    def wait(self, pattern, deadline, since):
        rx = re.compile(pattern)
        while time.time() < deadline:
            text = self.out[since:].decode("latin-1")
            m = rx.search(text)
            if m:
                return since + m.end()
            self.read(0.5)
        raise TimeoutError(f"no {pattern!r} from the guest")

    def kill(self):
        if self.p.poll() is None:
            self.p.kill()
        self.p.wait()


PROMPT = r"(term|cpu|[a-z]+)% $"
ASKS = r"(\[[^\]\n]*\]: ?$|\? ?$|bootargs is.*\n?.*: ?$)"


def boot(g, deadline):
    """Answer the boot's questions with their defaults until rc's prompt."""
    since = 0
    while True:
        text = g.out[since:].decode("latin-1").replace("\r", "")
        if re.search(PROMPT, text):
            return len(g.out)
        tail = text.rstrip("\n").split("\n")[-1] if text else ""
        if re.search(ASKS, tail) and not re.search(PROMPT, tail):
            g.send("\n")
            since = len(g.out)
        if time.time() > deadline:
            raise TimeoutError("9front did not reach an rc prompt")
        g.read(1.0)


def run(g, cmd, deadline):
    since = len(g.out)
    g.send(cmd + "\n")
    return g.wait(PROMPT, deadline, since)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "plan9", "guest"))
    ap.add_argument("--transcript", required=True)
    ap.add_argument("--bin", help="copy the built binaries here")
    ap.add_argument("--timeout", type=float, default=900)
    ap.add_argument("--memory", default="1024")
    a = ap.parse_args()

    os.makedirs(a.work, exist_ok=True)
    iso, kernel, ini = extract(os.path.abspath(a.iso), a.work)
    tree = os.path.join(a.work, "src")
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "plan9_package.py"),
                           "--tree", tree], stdout=subprocess.DEVNULL)
    img = os.path.join(a.work, "src.img")
    fat_image(tree, img)

    cmd = ["qemu-system-x86_64", "-m", a.memory, "-display", "none", "-monitor", "none",
           "-serial", "stdio", "-no-reboot", "-net", "none",
           "-kernel", kernel, "-append", kernel_args(ini),
           "-drive", f"file={img},format=raw,if=ide,index=1",
           "-drive", f"file={iso},media=cdrom,if=ide,index=2,readonly=on"]
    if os.access("/dev/kvm", os.R_OK | os.W_OK):
        cmd[1:1] = ["-enable-kvm", "-cpu", "host"]
    print("+ " + " ".join(cmd), flush=True)

    deadline = time.time() + a.timeout
    g = Guest(cmd, os.path.join(a.work, "serial.log"))
    raw = None
    try:
        boot(g, deadline)
        print("9front is up", flush=True)
        for c in ["dossrv >[2]/dev/null; sleep 1",
                  "mount -c /srv/dos /n/src /dev/sdC1/data",
                  "cd /n/src/plan9",
                  "mk >../build.log >[2=1]; echo OD-MK-^$status; cat ../build.log",
                  "6.opendoctrines < ../play.txt > ../transcript.txt; echo OD-RUN-^$status",
                  "echo " + BEGIN[:3] + "^" + BEGIN[3:] + "; cat ../transcript.txt; echo "
                  + END[:3] + "^" + END[3:],
                  "cd /; unmount /n/src"]:
            run(g, c, deadline)
        text = g.out.decode("latin-1").replace("\r", "")
        # The game's last words are a "> " with no newline, so the end marker
        # follows on the same line.
        m = re.search(r"\n" + BEGIN + r"\n(.*?)" + END + r"\n", text, re.S)
        if m:
            raw = m.group(1)
        mk = re.findall(r"\nOD-MK-(\S*)", text)
        print(f"mk: {mk[-1] if mk else 'no status'}", flush=True)
    except (TimeoutError, EOFError) as e:
        print(f"guest: {e}", flush=True)
    finally:
        g.kill()

    if a.bin:
        os.makedirs(a.bin, exist_ok=True)
        for b in ("6.opendoctrines", "6.odmap"):
            subprocess.call(["mcopy", "-n", "-o", "-i", img, "::/plan9/" + b, os.path.join(a.bin, b)])
    if raw is None:
        print("no transcript came back", flush=True)
        return 1
    rawf = os.path.join(a.work, "transcript.raw")
    with open(rawf, "w", encoding="latin-1") as f:
        f.write(raw)
    with open(a.transcript, "w") as f:
        subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "freertos_play.py"),
                               "--raw-in", rawf, os.path.join(ROOT, "tests", "freertos", "play.txt")],
                              stdout=f)
    return 0


if __name__ == "__main__":
    sys.exit(main())
