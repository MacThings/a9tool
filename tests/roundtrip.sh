#!/bin/sh
# Portable smoke-test suite. ./tests/roundtrip.sh [path/to/original/csvp.bin]
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PROGRAM="${A9TOOL_BIN:-$ROOT/a9tool}"
TEMP="$(mktemp -d)"
trap 'rm -rf "$TEMP"' EXIT HUP INT TERM
printf 'Testing A9 archive C port in %s\n' "$TEMP"
mkdir -p "$TEMP/synthetic/data"
printf 'alpha.txt\nempty.bin\nß_unicode.txt\n' > "$TEMP/synthetic/files.txt"
printf 'Hello codec! Test LZSS test test test test test.\n' > "$TEMP/synthetic/data/alpha.txt"
: > "$TEMP/synthetic/data/empty.bin"
printf '\303\237 test data\n' > "$TEMP/synthetic/data/ß_unicode.txt"
"$PROGRAM" -c "$TEMP/synthetic" -o "$TEMP/test.bin"
"$PROGRAM" -i "$TEMP/test.bin" > "$TEMP/index.out"
grep -q 'Files: 3' "$TEMP/index.out"
"$PROGRAM" -x "$TEMP/test.bin" -o "$TEMP/unpacked"
"$PROGRAM" -c "$TEMP/unpacked" -o "$TEMP/rebuilt.bin"
cmp "$TEMP/test.bin" "$TEMP/rebuilt.bin"
cmp "$TEMP/synthetic/files.txt" "$TEMP/unpacked/files.txt"
for f in alpha.txt empty.bin ß_unicode.txt; do cmp "$TEMP/synthetic/data/$f" "$TEMP/unpacked/data/$f"; done
printf 'PASS: type-0 normal and unicode filename roundtrip\n'

# Check sidecar data of type 1 is retained and its archive rebuilds identically.
mkdir -p "$TEMP/type1/data"
printf 'sample.dat\n' > "$TEMP/type1/files.txt"
printf '\000\001\002\003\004' > "$TEMP/type1/data/sample.dat"
printf '\001\002\003\004\005\006\007\010' > "$TEMP/type1/data/sample.dat.a9codec_data"
"$PROGRAM" -c "$TEMP/type1" -o "$TEMP/type1.bin" -t 1
"$PROGRAM" -x "$TEMP/type1.bin" -o "$TEMP/type1_recovered" -t 1
"$PROGRAM" -c "$TEMP/type1_recovered" -o "$TEMP/type1_rebuilt.bin" -t 1
cmp "$TEMP/type1.bin" "$TEMP/type1_rebuilt.bin"
cmp "$TEMP/type1/data/sample.dat.a9codec_data" "$TEMP/type1_recovered/data/sample.dat.a9codec_data"
printf 'PASS: type-1 metadata roundtrip\n'

# Tampered archive must be rejected, and existing output must be protected.
cp "$TEMP/test.bin" "$TEMP/tampered.bin"
printf '\377' | dd of="$TEMP/tampered.bin" bs=1 seek=22 conv=notrunc 2>/dev/null
if "$PROGRAM" -i "$TEMP/tampered.bin" > "$TEMP/bad.log" 2>&1; then
    echo 'FAIL: tampered file accepted' >&2;exit 1
fi
if "$PROGRAM" -c "$TEMP/synthetic" -o "$TEMP/test.bin" > "$TEMP/overwrite.log" 2>&1; then
    echo 'FAIL: overwrite allowed' >&2;exit 1
fi
printf 'PASS: corrupted input rejected and existing output protected\n'

if [ "$#" -gt 0 ]; then
    ORIGINAL="$1"
    "$PROGRAM" -x "$ORIGINAL" -o "$TEMP/csvp"
    "$PROGRAM" -c "$TEMP/csvp" -o "$TEMP/csvp_roundtrip.bin"
    cmp "$ORIGINAL" "$TEMP/csvp_roundtrip.bin"
    echo 'PASS: real csvp.bin rebuilt BYTE IDENTICAL'
fi
printf 'ALL TESTS PASSED\n'
