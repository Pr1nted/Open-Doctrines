#!/usr/bin/env bash
# Run a DOS command in DOSBox-X, headless, and come back with what it printed.
#
#   tools/dos_run.sh [options] <folder> <command line...>
#
#   <folder>     mounted as C: and made the current drive
#   options      --memsize MB     emulated memory (default 1024)
#                --timeout SECS   kill DOSBox-X after this long (default 1800)
#                --grab FILE      also screenshot the DOSBox-X window into FILE
#                --grab-after S   ...this many seconds in (default 60)
#                --grab-every S   ...and again every S seconds while it runs,
#                                 as FILE-01.png, FILE-02.png, ...
#
# The command's standard output lands in <folder>/OUT.TXT and is printed; its
# exit status (0 or not) in <folder>/RC.TXT, and this script exits with it.
# DOSBox-X itself never returns a program's status, so it is carried out
# through a file by RUN.BAT.
#
# The machine is an emulated Pentium with an S3 card (VESA 2.0, linear
# framebuffer) and long file names on, which is what the game needs (see
# packaging/dos/DOS.TXT). Needs dosbox-x and Xvfb; --grab needs ImageMagick.
set -euo pipefail

mem=1024; limit=1800; grab=""; grab_after=60; grab_every=0
while [ $# -gt 0 ]; do
  case "$1" in
    --memsize) mem="$2"; shift 2 ;;
    --timeout) limit="$2"; shift 2 ;;
    --grab) grab="$2"; shift 2 ;;
    --grab-after) grab_after="$2"; shift 2 ;;
    --grab-every) grab_every="$2"; shift 2 ;;
    *) break ;;
  esac
done
dir="$(cd "${1:?usage: dos_run.sh [options] <folder> <command...>}" && pwd)"; shift
cmdline="$*"
[ -n "$cmdline" ] || { echo "no command given" >&2; exit 2; }

rm -f "$dir/OUT.TXT" "$dir/RC.TXT"
printf '@ECHO OFF\r\nSET OD_DOS_TRACE=1\r\n%s > OUT.TXT\r\nIF ERRORLEVEL 1 GOTO BAD\r\nECHO 0 > RC.TXT\r\nGOTO DONE\r\n:BAD\r\nECHO 1 > RC.TXT\r\n:DONE\r\n' "$cmdline" > "$dir/RUN.BAT"

conf="$(mktemp --suffix=.conf)"
cat > "$conf" <<CONF
[sdl]
output=surface
fullscreen=false
autolock=false
[log]
logfile=
[dosbox]
machine=svga_s3
memsize=$mem
vmemsize=8
quit warning=false
[cpu]
core=dynamic
cputype=pentium_mmx
cycles=max
[dos]
ver=7.1
lfn=true
xms=true
ems=false
[mixer]
nosound=true
[speaker]
pcspeaker=false
[sblaster]
sbtype=none
[midi]
mpu401=none
[autoexec]
mount c "$dir"
c:
call RUN.BAT
exit
CONF

display=":$((90 + RANDOM % 9))"
Xvfb "$display" -screen 0 1600x1200x24 -nolisten tcp >/dev/null 2>&1 &
xvfb=$!
trap 'kill $xvfb 2>/dev/null || true; rm -f "$conf"' EXIT
sleep 2

start=$(date +%s)
DISPLAY="$display" SDL_AUDIODRIVER=dummy timeout "$limit" \
  dosbox-x -conf "$conf" -fastlaunch -nogui -nomenu -exit >"$dir/DOSBOX.LOG" 2>&1 &
box=$!

if [ -n "$grab" ]; then
  ( sleep "$grab_after"
    n=1
    while kill -0 $box 2>/dev/null; do
      if [ "$grab_every" -gt 0 ]; then out="${grab%.png}-$(printf %02d $n).png"; else out="$grab"; fi
      DISPLAY="$display" import -window root "$out" 2>/dev/null && echo "grabbed the screen into $out"
      [ "$grab_every" -gt 0 ] || break
      n=$((n + 1)); sleep "$grab_every"
    done ) &
  grabber=$!
fi

set +e
wait $box; boxrc=$?
set -e
if [ -n "$grab" ]; then kill "$grabber" 2>/dev/null || true; wait "$grabber" 2>/dev/null || true; fi
echo "DOSBox-X finished in $(( $(date +%s) - start ))s (status $boxrc)"
[ "$boxrc" = 124 ] && echo "timed out after ${limit}s"

[ -f "$dir/OUT.TXT" ] && tr -d '\r' < "$dir/OUT.TXT" || echo "(no OUT.TXT: the command never ran)"
rc=""; [ -f "$dir/RC.TXT" ] && rc="$(tr -dc '0-9' < "$dir/RC.TXT")"
echo "DOS exit status: ${rc:-unknown}"
[ "$rc" = 0 ]
