#!/bin/sh
# Release-manifest gate: assert the putup build produces every binary that the
# installer ships. installer/teraterm.iss [Files] is the authoritative shipped-
# binary manifest; each .exe/.dll it installs must exist in the putup build tree
# -- except the Cygwin/MSYS2 companion binaries, which are built with their own
# gcc toolchain on the Windows runner (not clang-cl) and are allowlisted.
#
# Catches "a new shipped binary was added to teraterm.iss but no putup Tupfile
# builds it" (packaging-manifest drift) before it silently ships short.
#
# Usage: check_release_manifest.sh [build-dir]   (default: build-win)
set -eu

BUILD="${1:-build-win}"
ISS="installer/teraterm.iss"

# Built by the Cygwin/MSYS2 gcc toolchain, not putup/clang-cl.
EXTERNAL=" cygterm.exe msys2term.exe "

names=$(grep -iE '^Source:' "$ISS" \
  | grep -iE '\.(exe|dll)' \
  | sed -E 's/.*[\\/]([A-Za-z0-9_]+\.(exe|dll)).*/\1/' \
  | sort -u)

fail=0
for n in $names; do
  if find "$BUILD" -name "$n" -type f | grep -q .; then
    echo "OK       $n"
  elif expr "$EXTERNAL" : ".* $n .*" >/dev/null; then
    echo "EXTERNAL $n (Cygwin/MSYS2 gcc, built on the Windows runner)"
  else
    echo "MISSING  $n -- shipped by teraterm.iss but not built by putup"
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "release-manifest gate: FAILED"
  exit 1
fi
echo "release-manifest gate: OK (all shipped binaries accounted for)"
