#!/bin/sh
# Guard the incremental ts/cv decoupling: a "sealed" core translation unit must
# not re-include ttwinman.h, which is what makes the ambient `ts`/`cv` globals
# reachable. Without the include the compiler already rejects any `ts.`/`cv.`
# access, so checking the include is a sufficient, false-positive-free gate.
#
# The list only grows as translation units are decoupled from the globals.
# See the modernization roadmap.
set -eu

cd "$(dirname "$0")/.."

SEALED="
teraterm/teraterm/charset.cpp
"

rc=0
for f in $SEALED; do
	if [ ! -f "$f" ]; then
		echo "sealed-TU list names a missing file: $f" >&2
		rc=1
		continue
	fi
	if grep -Eq '^[[:space:]]*#[[:space:]]*include[[:space:]]*"ttwinman\.h"' "$f"; then
		echo "SEALED TU re-includes ttwinman.h (regains ambient ts/cv access): $f" >&2
		rc=1
	fi
done

if [ "$rc" -eq 0 ]; then
	echo "sealed TUs clean:$SEALED"
fi
exit "$rc"
