#!/bin/sh
# Stage the putup build into the flat teraterm-<arch>/ layout the Inno Setup
# script (installer/teraterm.iss SrcDir) and the portable zip expect. This is the
# host-neutral replacement for the Linux-buildable part of collect_files.bat:
# it flattens the putup-built binaries out of build-win's mirrored tree, lays in
# the generated .lng sets and the static release/ asset tree, and applies the
# disable-by-default `_` renames plus the KEYBOARD.CNF / TERATERM.INI transforms.
#
# The Cygwin/MSYS2 companions (cygterm*, msys2term*) and the CHM help files are
# added on the Windows runner before ISCC; they are not produced by putup.
#
# Usage: stage_release.sh <build-dir> <out-dir>
set -eu
BUILD="${1:?usage: stage_release.sh <build-dir> <out-dir>}"
OUT="${2:?usage: stage_release.sh <build-dir> <out-dir>}"
REL="installer/release"
LNGDIR="$BUILD/installer/release/lang_utf8"

rm -rf "$OUT"
mkdir -p "$OUT" "$OUT/lang" "$OUT/lang_utf16le"

# 1. Binaries: flatten build-win's mirrored tree by name. collect_files stages
#    the full TTX sample set, not only teraterm.iss's shipped subset.
for b in ttermpro.exe ttpcmn.dll keycode.exe keycodeW.exe ttpmacro.exe ttxssh.dll \
         TTXProxy.dll ttxkanjimenu.dll cyglaunch.exe ttpmenu.exe \
         TTXAlwaysOnTop.dll TTXCallSysMenu.dll TTXCommandLineOpt.dll TTXCopyIniFile.dll \
         TTXFixedWinSize.dll TTXKcodeChange.dll TTXOutputBuffering.dll TTXRecurringCommand.dll \
         TTXResizeMenu.dll TTXResizeWin.dll TTXShowCommandLine.dll TTXViewMode.dll \
         TTXtest.dll TTXttyplay.dll TTXttyrec.dll TTXChangeFontSize.dll; do
  src=$(find "$BUILD" -name "$b" -type f | head -1)
  if [ -n "$src" ]; then cp "$src" "$OUT/"; else echo "stage: WARNING $b not built"; fi
done

# 2. Disable-by-default plugins: leading underscore (per collect_files).
for p in TTXFixedWinSize TTXOutputBuffering TTXResizeWin TTXShowCommandLine TTXtest; do
  [ -f "$OUT/$p.dll" ] && mv "$OUT/$p.dll" "$OUT/_$p.dll"
done

# 3. Static release/ root assets (*.ttl, *.CNF, license, TSPECIAL1.TTF, ssh_known_hosts).
find "$REL" -maxdepth 1 -type f -exec cp {} "$OUT/" \;
cp "$REL/IBMKEYB.CNF" "$OUT/KEYBOARD.CNF"

# 4. theme/ and plugin/ trees, excluding VCS dirs + CMakeLists (archive-exclude.txt).
for d in theme plugin; do
  cp -r "$REL/$d" "$OUT/$d"
  find "$OUT/$d" \( -name CMakeLists.txt -o -name CVS -o -name .svn \) -exec rm -rf {} +
done

# 5. Generated .lng from the putup build; drop en_US.lng from the ANSI set only.
cp "$LNGDIR/lang"/*.lng "$OUT/lang/"
cp "$LNGDIR/lang_utf16le"/*.lng "$OUT/lang_utf16le/"
rm -f "$OUT/lang/en_US.lng"

# 6. Generated TERATERM.INI, empty ini stubs, and the ttpmenu readme rename.
perl installer/setini.pl "$REL/TERATERM.INI" > "$OUT/TERATERM.INI"
: > "$OUT/ttpmenu.ini"
: > "$OUT/portable.ini"
[ -f ttpmenu/readme.txt ] && cp ttpmenu/readme.txt "$OUT/ttmenu_readme-j.txt"

echo "stage: done -> $OUT"
