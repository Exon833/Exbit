#!/bin/bash
# Writes the export list of one of Wine's own DLLs as a header for src/winefix (its stand-in passes
# every export on to Wine's DLL):  tools/winefix_exports.sh /usr/lib/wine/x86_64-windows/msi.dll > src/winefix/msi_exports.h
# Each line is EXPORT(index, ordinal, name): index counts from 0 (the place of the export's address
# in the stand-in's table), ordinal is Wine's (programs may import by ordinal).
set -euo pipefail
dll=$1
objdump=$(command -v x86_64-w64-mingw32-objdump || command -v objdump)
echo "// The exports of Wine's $(basename "$dll") (made by tools/winefix_exports.sh from Wine's own DLL: run it again"
echo "// for a newer Wine): EXPORT(index, ordinal, name)."
"$objdump" -p "$dll" | sed -n '/Ordinal\/Name Pointer/,/^$/p' | grep '+base\[' |
    sed 's/.*+base\[ *\([0-9]*\)\] *[0-9a-f]* \(.*\)/\1 \2/' | sort -n |
    awk '{ printf "EXPORT(%d, %d, %s)\n", NR - 1, $1, $2 }'
