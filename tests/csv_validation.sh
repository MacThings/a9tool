#!/bin/sh
# Standalone synthetic CSV validation tests; requires Python 3 only for fixtures.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PROGRAM="${A9TOOL_BIN:-$ROOT/a9tool}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP/fixture/data"
export A9_CSV_FIXTURE_DIR="$TMP/fixture"
python3 - <<'PY'
import os,pathlib
p=pathlib.Path(os.environ['A9_CSV_FIXTURE_DIR'])
(p/'files.txt').write_text('weather.csv\n')
rows=[]
for index in range(13):
    row=[f'entry{index}']+[f'cell{j}' for j in range(12)]
    rows.append(','.join(row))
(p/'data/weather.csv').write_bytes(b'\xff\xfe'+ ('\r\n'.join(rows)+'\r\n').encode('utf-16le'))
PY
"$PROGRAM" check "$TMP/fixture" > "$TMP/valid.log" 2>&1
grep -q '0 Fehler, 0 Warnungen' "$TMP/valid.log"
"$PROGRAM" check "$TMP/fixture/data" > "$TMP/direct.log" 2>&1
grep -q '0 Fehler, 0 Warnungen' "$TMP/direct.log"
echo 'PASS manually check FOLDER (extracted and direct CSV folder)'

python3 - <<'PY'
import os,pathlib
p=pathlib.Path(os.environ['A9_CSV_FIXTURE_DIR'])/'data/weather.csv'
s=p.read_bytes().decode('utf-16');lines=s.splitlines()
lines[3]+=',EXTRA_COLUMN'
p.write_bytes(b'\xff\xfe'+ ('\r\n'.join(lines)+'\r\n').encode('utf-16le'))
PY
if "$PROGRAM" check "$TMP/fixture" > "$TMP/columns.log" 2>&1;then echo 'FAIL: invalid field count accepted' >&2;exit 1;fi
grep -Eq 'weather.csv:4: FEHLER: Falsche Spaltenzahl' "$TMP/columns.log"
if "$PROGRAM" -c "$TMP/fixture" -o "$TMP/should_not_exist.bin" > "$TMP/packing.log" 2>&1;then echo 'FAIL: invalid CSV was packed' >&2;exit 1;fi
test ! -e "$TMP/should_not_exist.bin"
grep -q 'Archiv NICHT erstellt' "$TMP/packing.log"
"$PROGRAM" -c "$TMP/fixture" -o "$TMP/explicit_override.bin" --no-check > /dev/null
"$PROGRAM" -i "$TMP/explicit_override.bin" > /dev/null
echo 'PASS bad column count at weather.csv:4, packing prevented, --no-check explicit override'

python3 - <<'PY'
import os,pathlib
p=pathlib.Path(os.environ['A9_CSV_FIXTURE_DIR'])/'data/weather.csv'
s=p.read_bytes().decode('utf-16');rows=s.splitlines();rows[3]=rows[3].replace(',EXTRA_COLUMN','').replace('cell1','"quoted"invalid',1)
p.write_bytes(b'\xff\xfe'+ ('\r\n'.join(rows)+'\r\n').encode('utf-16le'))
PY
if "$PROGRAM" check "$TMP/fixture" > "$TMP/quote.log" 2>&1;then echo 'FAIL: invalid quotes accepted' >&2;exit 1;fi
grep -Eq 'weather.csv:4: FEHLER:.*Anführungszeichen' "$TMP/quote.log"
echo 'PASS malformed quotes reported at physical line 4'

python3 - <<'PY'
import os,pathlib
p=pathlib.Path(os.environ['A9_CSV_FIXTURE_DIR'])/'data/weather.csv'
s=p.read_bytes().decode('utf-16');rows=s.splitlines();rows[3]=rows[3].replace('"quoted"invalid','"quoted\r\nmultiline"',1)
p.write_bytes(b'\xff\xfe'+ ('\r\n'.join(rows)+'\r\n').encode('utf-16le'))
PY
"$PROGRAM" check "$TMP/fixture" > "$TMP/multiline.log" 2>&1
grep -q '0 Fehler, 0 Warnungen' "$TMP/multiline.log"
echo 'PASS legal multiline quoted CSV field'

python3 - <<'PY'
import os,pathlib
p=pathlib.Path(os.environ['A9_CSV_FIXTURE_DIR'])/'data/weather.csv';b=p.read_bytes();p.write_bytes(b'NOT_A_UTF16_CSV'+b[2:])
PY
if "$PROGRAM" check "$TMP/fixture" > "$TMP/bom.log" 2>&1;then echo 'FAIL: invalid encoding accepted' >&2;exit 1;fi
grep -Eq 'weather.csv:1: FEHLER: UTF-16LE BOM' "$TMP/bom.log"
echo 'PASS invalid encoding detected at weather.csv:1'

python3 - <<'PY'
import os,pathlib
p=pathlib.Path(os.environ['A9_CSV_FIXTURE_DIR'])/'data/weather.csv'
rows=[','.join([f'entry{i}']+[f'cell{j}' for j in range(12)]) for i in range(13)]
rows[3]=rows[3].replace('cell1','X'*9000,1)
p.write_bytes(b'\xff\xfe'+ ('\r\n'.join(rows)+'\r\n').encode('utf-16le'))
PY
"$PROGRAM" check "$TMP/fixture" > "$TMP/long.log" 2>&1
grep -q 'weather.csv:4: WARNUNG: Physische Zeile ungewöhnlich lang' "$TMP/long.log"
echo 'PASS unusually long physical line detected with filename:line warning'

python3 - <<'PY'
import os,pathlib
p=pathlib.Path(os.environ['A9_CSV_FIXTURE_DIR'])/'data/weather.csv'
s=p.read_bytes().decode('utf-16');s=s.replace('X'*9000,'X'*34000)
p.write_bytes(b'\xff\xfe'+s.encode('utf-16le'))
PY
if "$PROGRAM" check "$TMP/fixture" > "$TMP/huge.log" 2>&1;then echo 'FAIL: huge physical line accepted' >&2;exit 1;fi
grep -q 'weather.csv:4: FEHLER: Physische CSV-Zeile' "$TMP/huge.log"
echo 'PASS oversized line rejected with physical line number'

# Validate missing data and duplicate manifest references.
rm "$TMP/fixture/data/weather.csv"
if "$PROGRAM" check "$TMP/fixture" > "$TMP/missing.log" 2>&1;then echo 'FAIL: missing CSV accepted' >&2;exit 1;fi
grep -q 'weather.csv:0: FEHLER: Datei nicht lesbar' "$TMP/missing.log"
printf 'weather.csv\n' >> "$TMP/fixture/files.txt"
if "$PROGRAM" check "$TMP/fixture" > "$TMP/duplicate.log" 2>&1;then echo 'FAIL: duplicate manifest accepted' >&2;exit 1;fi
grep -q 'Datei doppelt in files.txt: weather.csv' "$TMP/duplicate.log"
echo 'PASS missing file and duplicate manifest entry reported'
echo 'CSV VALIDATION TESTS PASSED'
