#!/bin/sh
# Archive comparison end-to-end test (macOS and Linux).
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PROGRAM="${A9TOOL_BIN:-$ROOT/a9tool}"
TEMP=$(mktemp -d)
trap 'rm -rf "$TEMP"' EXIT HUP INT TERM
mkdir -p "$TEMP/old/data" "$TEMP/new/data"
export A9_COMPARISON_TEST_FOLDER="$TEMP"
python3 - <<'PY'
import os,pathlib
root=pathlib.Path(os.environ['A9_COMPARISON_TEST_FOLDER'])

def write(side,name,contents):
    (root/side/'data'/name).write_bytes(b'\xff\xfe'+contents.encode('utf-16le'))

write('old','alpha.csv','header,col\r\nITEM_A,same\r\nITEM_B,before\r\nITEM_C,remove me\r\nITEM_D,unchanged\r\nITEM_E,tail\r\n')
write('new','alpha.csv','header,col\r\nITEM_A,same\r\nITEM_X,added\r\nITEM_B,after\r\nITEM_D,unchanged\r\nITEM_E,tail\r\n')
write('old','multi.csv','id,comment\r\nREC_ONE,"old\r\ntext"\r\nREC_TWO,ok\r\n')
write('new','multi.csv','id,comment\r\nREC_ONE,"old\r\ntext"\r\nREC_NEW,"new\r\nmultiline"\r\nREC_TWO,ok\r\n')
write('old','replace.csv','id,value\r\nSECTION_OLD,before\r\nSECTION_END,same\r\n')
write('new','replace.csv','id,value\r\nSECTION_NEW,after\r\nSECTION_END,same\r\n')
write('new','created.csv','id,value\r\nCREATED_ID,yes\r\n')
write('old','removed.csv','id,value\r\nREMOVED_ID,no\r\n')
(root/'old'/'files.txt').write_text('alpha.csv\nmulti.csv\nreplace.csv\nremoved.csv\n')
(root/'new'/'files.txt').write_text('alpha.csv\nmulti.csv\nreplace.csv\ncreated.csv\n')
PY
"$PROGRAM" -c "$TEMP/old" -o "$TEMP/old.bin" --no-check >/dev/null
"$PROGRAM" -c "$TEMP/new" -o "$TEMP/new.bin" --no-check >/dev/null
"$PROGRAM" compare "$TEMP/old.bin" "$TEMP/new.bin" >"$TEMP/changes.txt"
cat "$TEMP/changes.txt"
grep -q '+ alpha.csv:3: NEU ITEM_X' "$TEMP/changes.txt"
grep -q '~ alpha.csv:4: GEÄNDERT (alt Zeile 3) ITEM_B' "$TEMP/changes.txt"
grep -q -- '- alpha.csv:4: ENTFERNT (alte Zeile) ITEM_C' "$TEMP/changes.txt"
grep -q '+ multi.csv:4: NEU REC_NEW' "$TEMP/changes.txt"
grep -q '+ replace.csv:2: NEU SECTION_NEW' "$TEMP/changes.txt"
grep -q -- '- replace.csv:2: ENTFERNT (alte Zeile) SECTION_OLD' "$TEMP/changes.txt"
grep -q 'DATEI NEU: created.csv' "$TEMP/changes.txt"
grep -q 'DATEI ENTFERNT: removed.csv' "$TEMP/changes.txt"
grep -q 'CSV-Datensätze: +5 neu, ~1 geändert, -4 entfernt' "$TEMP/changes.txt"
echo 'PASS: archive compare detects inserted, changed, deleted and multiline rows with physical line numbers'

"$PROGRAM" compare "$TEMP/old.bin" "$TEMP/new.bin" --only-added >"$TEMP/added.txt"
grep -q '+ alpha.csv:3: NEU ITEM_X' "$TEMP/added.txt"
if grep -Eq '^~ |^- ' "$TEMP/added.txt";then echo 'FAIL: only-added shows removal/change' >&2;exit 1;fi
"$PROGRAM" compare "$TEMP/old.bin" "$TEMP/new.bin" -o "$TEMP/report.txt" >/dev/null
cmp "$TEMP/report.txt" "$TEMP/changes.txt"
if "$PROGRAM" compare "$TEMP/old.bin" "$TEMP/new.bin" -o "$TEMP/report.txt" >"$TEMP/overwrite.log" 2>&1;then
  echo 'FAIL: overwrote existing report' >&2;exit 1
fi
if "$PROGRAM" compare "$TEMP/old.bin" "$TEMP/new.bin" -o "$TEMP/old.bin" >"$TEMP/unsafe.log" 2>&1;then
  echo 'FAIL: overwrote input archive' >&2;exit 1
fi
"$PROGRAM" compare "$TEMP/new.bin" "$TEMP/new.bin" >"$TEMP/same.txt"
grep -q 'CSV-Datensätze: +0 neu, ~0 geändert, -0 entfernt' "$TEMP/same.txt"
echo 'PASS: --only-added, --output report, overwrite protection, identical archives'

echo 'A9TOOL COMPARISON TESTS PASSED'
