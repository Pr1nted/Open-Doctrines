#!/bin/sh
# Build and test one odseal archive inside a BSD VM -- the `run` of every BSD
# entry in .github/workflows/odseal-prebuilts.yml. Expects the sealed source in
# guard/ (that workflow fetches it) and leaves the archive in out/.
#
# One script for FreeBSD, OpenBSD and NetBSD on every architecture: the same
# flags, the same exports check and the same test, so an archive cannot differ
# from its neighbours by which copy of the steps built it. Each system's own
# c++, strip, ar and nm -- base clang on FreeBSD and OpenBSD, base gcc on
# NetBSD -- because those are what the game links with there.
#
# Building AND testing here, in one VM session, is deliberate: the archive
# never leaves the VM in between, so the test runs on exactly what is shipped.
set -eu

uname -a
mkdir -p out
# NOT `strip -x`: that discards every local symbol, and OpenBSD's strip
# refuses rather than corrupt the object --
#
#   strip: error: 'odseal.o': not stripping symbol '.L.str.1'
#   because it is named in a relocation
#
# which is correct of it and was the first thing the OpenBSD job did.
# --strip-unneeded is the flag that means what was intended: remove what
# relocation processing does not need, keep what it does. It is also exactly
# what the Linux steps use.
c++ -std=c++17 -O3 -fvisibility=hidden -fno-rtti -fomit-frame-pointer -fPIC \
    -c guard/odseal.cpp -o odseal.o
strip --strip-unneeded odseal.o
ar rcs out/libodseal.a odseal.o

# The exports, asserted rather than assumed. Whatever strip did or declined to
# do, what leaves this VM must be the entry points and nothing else -- a stray
# symbol is what points a reader at what this library does. Same two rules as
# the android check.
#
# Plain `nm -g`, and its errors are NOT hidden. --defined-only is a GNU long
# option OpenBSD's nm does not take; with stderr sent to /dev/null the check
# simply saw an empty list and reported "missing entry point: od_fz" about an
# archive that was fine. Filtering on T already excludes undefined symbols,
# which is all --defined-only was doing.
GOT="$(nm -g out/libodseal.a | awk '$2=="T"{print $3}' | sort | tr '\n' ' ')"
echo "exports: $GOT"
for sym in od_fz od_k9 od_s7 od_v3; do
  printf '%s\n' $GOT | grep -qx "$sym" || { echo "missing entry point: $sym"; exit 1; }
done
for sym in $GOT; do
  case " od_fz od_k9 od_p2 od_p3 od_t4 od_v3 " in
    *" $sym "*) ;;
    *) echo "unexpected export: $sym"; exit 1 ;;
  esac
done

# Native, so the real test runs rather than a symbol check -- emulated, on the
# Arm guests, which is still the target's own kernel and libc.
c++ -std=c++17 -O1 -I third_party/odseal tests/odseal_test.cpp out/libodseal.a -o odseal_test
./odseal_test
rm -rf guard odseal_test odseal.o
