#!/usr/bin/env bash
# Build the Linux packages inside a real Linux guest, install them, run them,
# and take them away again.
#
#   tools/linux_vm_test.sh <host> [tree]
#
#   host   ssh destination of a guest from tools/linux_vm_create.sh, e.g. odlinux
#   tree   a packaged game tree (tools/package.py output). Omitted, a STUB
#          binary is used: the packaging is what is under test, not the game.
#
# WHAT THIS ANSWERS THAT CI CANNOT
#
# The release workflow proves the packages BUILD, on the machine that built the
# game -- where every library is already installed, which is exactly the
# condition a packaging bug hides in. It cannot prove one INSTALLS on a machine
# that has none of them, that its declared dependencies are real package names,
# that the launcher finds its own prefix, or that removing it leaves nothing
# behind and takes nothing of the player's with it.
#
# The Windows installer went untested for that reason until a real Windows
# machine ran it and found two bugs in an afternoon. This is the same argument
# for Linux.
#
# THE STUB IS NOT A SHORTCUT. With no tree given, the "game" is a shell script
# that prints $OD_DATA_DIR. Everything this checks -- file placement, the
# MANAGED marker, dependency resolution, the launcher's prefix derivation, the
# writable-copy seeding, clean removal -- is true of the packaging and
# independent of what the executable does. One thing it CANNOT check is rpm's
# automatic dependency generation, which reads an ELF: pass a real tree for
# that.
set -uo pipefail

HOST="${1:-$OD_SSH_HOST}"
TREE="${2:-}"
[ -n "$HOST" ] || { echo "usage: $0 <ssh-host> [packaged-tree]"; exit 1; }

# A guest from tools/linux_vm_create.sh is an ~/.ssh/config alias and needs no
# options. One from tools/qemu_guest.sh is odtest@127.0.0.1 on a forwarded port
# with its own key, and needs several -- so it supplies them in the
# environment, via `eval "$(tools/qemu_guest.sh sshenv <name>)"`. Empty here is
# the alias case, unchanged.
#
# Two variables rather than one because ssh spells the port -p and scp -P.
SSH_OPTS="${OD_SSH_OPTS:-}"
SCP_OPTS="${OD_SCP_OPTS:-}"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PKG="$ROOT/packaging/linux"
APPID=io.github.Pr1nted.OpenDoctrines
VERSION="${OD_TEST_VERSION:-1.2.3a}"

checks=0; failed=0
ok()  { checks=$((checks+1)); printf '  ok    %s\n' "$1"; }
bad() { checks=$((checks+1)); failed=$((failed+1)); printf '  FAIL  %s%s\n' "$1" "${2:+  [$2]}"; }
try() { if [ "$1" = 0 ]; then ok "$2"; else bad "$2" "${3:-}"; fi; }
sec() { printf '\n== %s ==\n' "$1"; }

# shellcheck disable=SC2086 -- splitting SSH_OPTS into arguments is the point
r() { ssh $SSH_OPTS "$HOST" "$@"; }

echo "host: $HOST"
arch=$(r 'dpkg --print-architecture' 2>/dev/null) || { echo "cannot reach $HOST"; exit 1; }
distro=$(r '. /etc/os-release; echo "$PRETTY_NAME"')
glibc=$(r 'ldd --version | head -1 | grep -oE "[0-9]+\.[0-9]+$"')
echo "guest: $distro  ($arch, glibc $glibc)"

sec "getting the tools and the sources there"
r 'rm -rf ~/od && mkdir -p ~/od/packaging/linux ~/od/tools ~/od/tree/data/Icon'
scp $SCP_OPTS -q "$ROOT/tools/make_linux_packages.sh" "$HOST:od/tools/"
scp $SCP_OPTS -q "$PKG/opendoctrines-launcher" "$PKG/$APPID.desktop" "$PKG/$APPID.metainfo.xml" \
       "$HOST:od/packaging/linux/"
if [ -n "$TREE" ]; then
    echo "  pushing $TREE"
    scp $SCP_OPTS -qr "$TREE/." "$HOST:od/tree/"
else
    echo "  no tree given: using a stub that prints OD_DATA_DIR"
    scp $SCP_OPTS -q "$ROOT/data/Icon/icon.png" "$HOST:od/tree/data/Icon/"
    r 'printf "#!/bin/sh\nprintf \"GAME OK data=%%s\\n\" \"\$OD_DATA_DIR\"\n" > ~/od/tree/OpenDoctrines
       chmod +x ~/od/tree/OpenDoctrines
       echo tips > ~/od/tree/data/tips.json'
fi
r 'chmod +x ~/od/tools/make_linux_packages.sh'
# The tools the packaging needs. Installed here rather than baked into the
# image so a rebuilt guest is a clean one.
r 'command -v fpm >/dev/null || {
     sudo apt-get update -qq
     sudo apt-get install -y -qq ruby ruby-dev build-essential rpm file desktop-file-utils >/dev/null 2>&1
     sudo gem install --no-document fpm >/dev/null 2>&1; }
   command -v appimagetool >/dev/null || {
     a=$(dpkg --print-architecture); [ "$a" = amd64 ] && a=x86_64 || a=aarch64
     curl -sSL -o /tmp/appimagetool "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$a.AppImage"
     chmod +x /tmp/appimagetool
     printf "#!/bin/sh\nexec /tmp/appimagetool --appimage-extract-and-run \"\$@\"\n" | sudo tee /usr/local/bin/appimagetool >/dev/null
     sudo chmod +x /usr/local/bin/appimagetool; }' >/dev/null 2>&1
try $? "the packaging tools are present"

sec "building the packages, on Linux"
# KEEP THE OUTPUT. This was >/dev/null 2>&1, so when the build failed the
# report said "make_linux_packages.sh runs to completion -- FAIL" and threw
# away the only thing that could say why; the next eight checks then failed
# as consequences, and the cause was gone. A gate that discards the reason
# makes you reproduce the failure by hand to find out what it already knew.
pkglog=$(mktemp)
r "cd ~/od && tools/make_linux_packages.sh tree $VERSION $arch out" > "$pkglog" 2>&1
pkgrc=$?
try $pkgrc "make_linux_packages.sh runs to completion"
if [ "$pkgrc" != 0 ]; then
    echo "        --- what it said ---"
    tail -15 "$pkglog" | sed 's/^/        /'
fi
rm -f "$pkglog"
for k in deb rpm AppImage; do
    r "ls ~/od/out/*.$k" >/dev/null 2>&1
    try $? "it produced a .$k"
done

deb=$(r 'ls ~/od/out/*.deb')

sec "installing it where none of its libraries are"
# Intentionally NOT pre-installed in the guest: whether the package pulls its
# own X11 runtime in is one of the things being tested.
r "sudo dpkg -i $deb" >/dev/null 2>&1
r "sudo apt-get -f install -y" >/dev/null 2>&1
state=$(r "dpkg -l opendoctrines 2>/dev/null | tail -1 | cut -c1-2")
[ "$state" = "ii" ] && ok "it installs and configures" \
                    || bad "it installs and configures" "dpkg state '$state'"

# A dependency NAME that does not exist in the archive cannot be fixed by
# apt, and the package is uninstallable on every machine rather than this one.
#
# MATCHED AGAINST A LIST, NOT ASKED ONE AT A TIME. This was `dpkg -s "$d"`,
# which answers
#
#   dpkg-query: error: --status needs a valid package name but 'libc6' is not:
#               ambiguous package name 'libc6' with more than one installed
#               instance
#
# the moment the guest has a foreign architecture registered -- ours now has
# i386 and armhf, so libc6 is installed three times over. dpkg exits non-zero,
# the loop files it as unresolved, and the stage reports that libc6 is not a
# real package while the .deb it came from is installed and configured. Every
# multi-arch dependency failed and only the single-arch ones passed, which is
# the shape of the bug rather than the shape of a packaging mistake.
#
# Comparing against the set of installed names, with the :arch suffix stripped,
# cannot be ambiguous. Version constraints are stripped too: `libc6 (>= 2.34)`
# became `libc6(>=2.34)` under the old sed and would have been reported as a
# missing package the day one was added.
unresolved=$(r "installed=\$(dpkg-query -W -f='\${binary:Package}\n' 2>/dev/null \
                             | sed 's/:.*//' | sort -u)
                for d in \$(dpkg-deb -f $deb Depends | tr ',' '\n' \
                             | sed 's/(.*//; s/ //g' | grep -v '^\$'); do
                  printf '%s\n' \"\$installed\" | grep -qx \"\$d\" || echo \"\$d\"
                done")
[ -z "$unresolved" ] && ok "every declared dependency is a real package" \
                     || bad "every declared dependency is a real package" "$unresolved"

sec "does it run"
out=$(r 'opendoctrines' 2>&1)
case "$out" in
    *"$HOME"*|*data=*) ok "the launcher starts the game against a WRITABLE data dir" ;;
    *) bad "the launcher starts the game against a writable data dir" "$out" ;;
esac
r 'test -f /usr/lib/opendoctrines/MANAGED'
try $? "MANAGED sits beside the executable, so the updater stands down"
r 'test -f ~/.local/share/opendoctrines/data/.installed-version'
try $? "the shipped tree was seeded into the player's home"

sec "the AppImage, with nothing installed at all"
# AN APPIMAGE NEEDS FUSERMOUNT, AND DEBIAN 12 DOES NOT SHIP IT.
#
# The first version of this check ran the file and failed, which read as "the
# AppImage is broken". It is not: a stock Debian 12 has libfuse2 and libfuse3
# and /dev/fuse, and no `fusermount` BINARY, because that lives in the `fuse`
# package, which nothing pulls in. The AppImage then says
#
#   Error: No suitable fusermount binary found on the $PATH
#
# which is an accurate message about the machine and a baffling one to a
# player who just wanted to double-click a game.
#
# So this asks the two questions separately. Extracted, it must ALWAYS work:
# that is the package. Mounted, it works once fuse is installed: that is the
# documented dependency, and the README has to say so, which is checked
# further down rather than assumed.
out=$(r "cd ~/od && ./out/*.AppImage --appimage-extract-and-run 2>&1" | tail -1)
case "$out" in
    *data=*) ok "extracted, it runs with nothing installed at all" ;;
    *) bad "extracted, it runs with nothing installed at all" "$out" ;;
esac

if r 'command -v fusermount >/dev/null || command -v fusermount3 >/dev/null'; then
    out=$(r "cd ~/od && ./out/*.AppImage 2>&1" | tail -1)
    case "$out" in
        *data=*) ok "and mounted, on a machine that has fusermount" ;;
        *) bad "and mounted, on a machine that has fusermount" "$out" ;;
    esac
else
    # Install the one dependency a player would, and prove it is the only one.
    r 'sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq fuse' >/dev/null 2>&1
    out=$(r "cd ~/od && ./out/*.AppImage 2>&1" | tail -1)
    case "$out" in
        *data=*) ok "and mounted, once fuse is installed -- its one dependency" ;;
        *) bad "and mounted, once fuse is installed" "$out" ;;
    esac
fi

# The README is the only place a player finds out, so it is part of the test.
if grep -q "fusermount\|libfuse\|  fuse" "$ROOT/README.md" 2>/dev/null; then
    ok "and README.md tells them to install it"
else
    bad "and README.md tells them to install it" "no mention of fuse in README.md"
fi

sec "taking it away again"
r 'sudo dpkg -r opendoctrines' >/dev/null 2>&1
left=$(r 'for p in /usr/lib/opendoctrines /usr/bin/opendoctrines \
            /usr/share/applications/'"$APPID"'.desktop \
            /usr/share/metainfo/'"$APPID"'.metainfo.xml; do
            [ -e "$p" ] && echo "$p"; done')
[ -z "$left" ] && ok "removal leaves nothing behind" \
               || bad "removal leaves nothing behind" "$left"
r 'test -d ~/.local/share/opendoctrines/data'
try $? "and does NOT take the player's saves with it"

printf '\n%d checks, %d failed\n' "$checks" "$failed"
[ "$failed" -eq 0 ]
