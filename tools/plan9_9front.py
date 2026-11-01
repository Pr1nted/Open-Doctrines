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


def describe(path):
    """What a file is, for the log: size, first bytes, file(1), ISO/MBR marks."""
    with open(path, "rb") as f:
        head = f.read(64)
        f.seek(0x8000)
        pvd = f.read(8)
        f.seek(510)
        mbr = f.read(2)
    size = os.path.getsize(path)
    kind = subprocess.run(["file", "-b", path], capture_output=True, text=True).stdout.strip() \
        if shutil.which("file") else "?"
    print(f"{path}: {size} bytes; file: {kind}", flush=True)
    print(f"  first bytes: {head[:32].hex()}", flush=True)
    gz, iso, boot = head[:2] == b"\x1f\x8b", pvd[1:6] == b"CD001", mbr == b"\x55\xaa"
    print(f"  gzip magic: {gz}; ISO9660 PVD at 0x8000: {iso}; MBR 55aa: {boot}", flush=True)
    return head, iso, boot


def gunzip(src, work):
    """However many layers of gzip there are: an .iso.gz served gzipped again
    (or a .gz that was not) must not reach the readers still compressed."""
    cur, n = src, 0
    while True:
        with open(cur, "rb") as f:
            if f.read(2) != b"\x1f\x8b":
                return cur
        n += 1
        dst = os.path.join(work, f"9front.{n}.iso")
        with gzip.open(cur, "rb") as a, open(dst, "wb") as b:
            shutil.copyfileobj(a, b, 1 << 20)
        print(f"gunzipped {cur} -> {dst}", flush=True)
        cur = dst


def iso9660_read(iso, path):
    """One file out of an ISO9660 image, by walking its directories (no Rock
    Ridge or Joliet needed: names compare case-blind, without ';1')."""
    with open(iso, "rb") as f:
        lb = 2048
        sector = 16
        root = None
        while sector < 64:
            f.seek(sector * 2048)
            vd = f.read(2048)
            if vd[1:6] != b"CD001":
                break
            if vd[0] == 1:
                lb = int.from_bytes(vd[128:130], "little")
                root = vd[156:156 + 34]
                break
            if vd[0] == 255:
                break
            sector += 1
        if root is None:
            raise ValueError("no ISO9660 primary volume descriptor")
        ext, size = int.from_bytes(root[2:6], "little"), int.from_bytes(root[10:14], "little")
        parts = path.split("/")
        for i, want in enumerate(parts):
            f.seek(ext * lb)
            data = f.read(size)
            off, found = 0, None
            while off < len(data):
                ln = data[off]
                if ln == 0:
                    off = (off // 2048 + 1) * 2048
                    continue
                rec = data[off:off + ln]
                nl = rec[32]
                name = rec[33:33 + nl].decode("latin-1").split(";")[0].rstrip(".").lower()
                if name == want.lower():
                    found = rec
                    break
                off += ln
            if found is None:
                raise FileNotFoundError(f"{path} not in {iso}")
            ext, size = int.from_bytes(found[2:6], "little"), int.from_bytes(found[10:14], "little")
        f.seek(ext * lb)
        return f.read(size)


def extract(iso, work):
    """amd64/9pc64 and cfg/plan9.ini out of the ISO, as data, by whichever
    reader works. Returns (image, kernel, plan9.ini, is_iso9660); kernel and
    plan9.ini are None when nothing could read them -- the caller then boots
    the image itself and talks to 9boot instead."""
    describe(iso)
    iso = gunzip(iso, work)
    _, is_iso, _ = describe(iso)
    out = os.path.join(work, "iso")
    os.makedirs(out, exist_ok=True)
    want = ["amd64/9pc64", "cfg/plan9.ini"]
    paths = [os.path.join(out, w) for w in want]

    def have():
        return all(os.path.isfile(p) and os.path.getsize(p) > 0 for p in paths)

    readers = []
    if shutil.which("bsdtar"):
        readers.append(("bsdtar", ["bsdtar", "-xf", iso, "-C", out] + want))
    if shutil.which("7z"):
        readers.append(("7z", ["7z", "x", "-y", "-o" + out, iso] + want))
    for name, cmd in readers:
        r = subprocess.run(cmd, capture_output=True, text=True)
        print(f"{name}: exit {r.returncode} {(r.stderr or '').strip()[:300]}", flush=True)
        if r.returncode == 0 and have():
            return iso, paths[0], paths[1], is_iso
    if shutil.which("isoinfo"):
        ok = True
        for w, p in zip(want, paths):
            os.makedirs(os.path.dirname(p), exist_ok=True)
            for args in (["-J", "-x", "/" + w], ["-R", "-x", "/" + w], ["-x", "/" + w.upper() + ";1"]):
                with open(p, "wb") as fo:
                    r = subprocess.run(["isoinfo", "-i", iso] + args, stdout=fo, stderr=subprocess.DEVNULL)
                if r.returncode == 0 and os.path.getsize(p) > 0:
                    break
            else:
                ok = False
        print(f"isoinfo: {'ok' if ok else 'failed'}", flush=True)
        if ok and have():
            return iso, paths[0], paths[1], is_iso
    try:
        for w, p in zip(want, paths):
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as fo:
                fo.write(iso9660_read(iso, w))
        print("built-in ISO9660 reader: ok", flush=True)
        return iso, paths[0], paths[1], is_iso
    except (ValueError, OSError) as e:
        print(f"built-in ISO9660 reader: {e}", flush=True)
    print("could not read the kernel out of the image: booting it through 9boot instead", flush=True)
    return iso, None, None, is_iso


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


def boot9(g, deadline):
    """Drive 9boot, 9front's bootloader, over the serial line.

    Used when the kernel could not be read out of the image. SeaBIOS's serial
    console (fw_cfg etc/sercon-port) carries the BIOS screen and keyboard to
    COM1, and 9boot speaks through the BIOS, so its plan9.ini listing and its
    "> " prompt arrive here. Keys are pressed after "Booting from" to stop any
    countdown; at the prompt console=0 is added, so the kernel's console is
    this serial line too, and then "boot"."""
    since = 0
    poked = 0.0
    seen_boot = None
    while time.time() < deadline:
        # sercon draws the BIOS screen with ANSI sequences: drop them
        text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "\n",
                      g.out[since:].decode("latin-1")).replace("\r", "\n")
        if seen_boot is None and "Booting from" in text:
            seen_boot = time.time()
        if re.search(r"\n\s*>\s*$", text):
            print("9boot prompt: console=0, boot", flush=True)
            g.send("\b" * 8 + "console=0\r")
            settle = time.time() + 2
            while time.time() < settle:
                g.read(0.2)
            g.send("boot\r")
            return
        if seen_boot and time.time() - seen_boot < 30 and time.time() - poked > 1.0:
            g.send(" ")
            poked = time.time()
        g.read(0.5)
    raise TimeoutError("no 9boot prompt on the serial line")


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
    iso, kernel, ini, is_iso = extract(os.path.abspath(a.iso), a.work)
    tree = os.path.join(a.work, "src")
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "plan9_package.py"),
                           "--tree", tree], stdout=subprocess.DEVNULL)
    img = os.path.join(a.work, "src.img")
    fat_image(tree, img)

    kvm = os.access("/dev/kvm", os.R_OK | os.W_OK)

    def qemu(direct):
        cmd = ["qemu-system-x86_64", "-m", a.memory, "-display", "none", "-monitor", "none",
               "-serial", "stdio", "-no-reboot", "-net", "none",
               "-drive", f"file={img},format=raw,if=ide,index=1"]
        if kvm:
            cmd[1:1] = ["-enable-kvm", "-cpu", "host"]
        medium = (["-drive", f"file={iso},media=cdrom,if=ide,index=2,readonly=on"] if is_iso
                  else ["-drive", f"file={iso},format=raw,if=ide,index=0,snapshot=on"])
        if direct:
            return cmd + ["-kernel", kernel, "-append", kernel_args(ini)] + medium
        # SeaBIOS's serial console, at COM1 (0x3f8, little-endian), for 9boot.
        sercon = os.path.join(a.work, "sercon-port")
        with open(sercon, "wb") as f:
            f.write((0x3F8).to_bytes(2, "little"))
        return cmd + ["-fw_cfg", f"name=etc/sercon-port,file={sercon}"] + medium + \
            ["-boot", "d" if is_iso else "c"]

    deadline = time.time() + a.timeout
    log = os.path.join(a.work, "serial.log")
    raw = None
    g = None
    try:
        direct = kernel is not None
        if direct:
            # The kernel straight from QEMU (multiboot). If QEMU refuses it, or
            # it says nothing on the serial line, go round by 9boot instead.
            cmd = qemu(True)
            print("+ " + " ".join(cmd), flush=True)
            g = Guest(cmd, log)
            try:
                g.wait(r"\S", min(deadline, time.time() + (90 if kvm else 240)), 0)
            except (TimeoutError, EOFError) as e:
                print(f"direct kernel boot: {e}; output: {g.out[-500:]!r}", flush=True)
                g.kill()
                g, direct = None, False
        if not direct:
            cmd = qemu(False)
            print("+ " + " ".join(cmd), flush=True)
            g = Guest(cmd, log + ".9boot" if kernel else log)
            boot9(g, deadline)
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
        tail = (g.out if g else b"").decode("latin-1").replace("\r", "").split("\n")[-40:]
        print("last of the serial line:\n  " + "\n  ".join(tail), flush=True)
    finally:
        if g:
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
