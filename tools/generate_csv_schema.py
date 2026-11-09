#!/usr/bin/env python3
"""Generate reference CSV structure from verified A9 csvp.bin extraction.

Stores CSV column profiles and hashes of ASCII section IDs, not game text.
"""
import csv
import io
import collections
import pathlib
import re
import sys

if len(sys.argv) != 3:
    raise SystemExit('Usage: generate_csv_schema.py path/to/unpacked/data src/csvp_schema.c')
source = pathlib.Path(sys.argv[1]);target=pathlib.Path(sys.argv[2])
files = sorted(source.glob('*.csv'))
if not files:raise SystemExit('No CSV files in '+str(source))

def hash_id(s):
    h=2166136261
    for c in s.encode('utf-16le'):h=((h^c)*16777619)&0xffffffff
    return h

def quote(s):return '"'+s.replace('\\','\\\\').replace('"','\\"')+'"'

lines=[
 '/* GENERATED from known-good A9 csvp.bin. Contains only layout and ID hashes. */',
 '#include "csvp_schema.h"',
 '#include <string.h>',
 '',
 'uint32_t csvp_anchor_hash(const uint16_t *text,size_t len) {',
 '    uint32_t h=2166136261u;',
 '    for(size_t i=0;i<len;i++) {',
 '        uint16_t c=text[i];',
 '        h=(h^(uint8_t)c)*16777619u;',
 '        h=(h^(uint8_t)(c>>8))*16777619u;',
 '    }',
 '    return h;',
 '}',
 ''
]
schemas=[]
for number,file in enumerate(files):
    raw=file.read_bytes()
    if not raw.startswith(b'\xff\xfe'):raise SystemExit(f'No UTF-16LE BOM: {file}')
    text=raw.decode('utf-16')
    rows=list(csv.reader(io.StringIO(text,newline=''),strict=True))
    cols=collections.Counter(map(len,rows))
    common=cols.most_common(1)[0][0]
    overridden=[(i,len(row)) for i,row in enumerate(rows,1) if len(row)!=common]
    anchors=[(i,hash_id(row[0])) for i,row in enumerate(rows,1)
             if row and re.fullmatch(r'[A-Z][A-Z0-9_]+',row[0]) and
             (len(row[0])>5 or '_' in row[0]) and row[0] not in ('ON','OFF')]
    tag=f'{number:02d}'
    ws=f'width_{tag}' if overridden else 'NULL'
    ans=f'anchors_{tag}' if anchors else 'NULL'
    if overridden:
        lines.append(f'static const CsvWidth {ws}[] = {{')
        lines.extend(f'    {{{i}u,{count}u}},' for i,count in overridden)
        lines.append('};')
    if anchors:
        lines.append(f'static const CsvAnchor {ans}[] = {{')
        lines.extend(f'    {{{i}u,0x{hashv:08x}u}},' for i,hashv in anchors)
        lines.append('};')
    maxlen=max(map(len,text.splitlines()),default=0)
    schemas.append((file.name,len(rows),maxlen,common,ws,len(overridden),ans,len(anchors)))
    print(f'{file.name}: {len(rows)} records / {len(overridden)} exceptional widths / {len(anchors)} anchors / max-line {maxlen}')
lines.append('static const CsvSchema SCHEMAS[] = {')
for name,records,maxline,common,ws,wn,ans,an in schemas:
    lines.append(f'    {{{quote(name)},{records}u,{maxline}u,{common}u,{ws},{wn}u,{ans},{an}u}},')
lines+=['};','const CsvSchema *csvp_find_schema(const char *filename) {',
 '    for(size_t i=0;i<sizeof SCHEMAS/sizeof SCHEMAS[0];i++)',
 '        if(strcmp(SCHEMAS[i].filename,filename)==0)return &SCHEMAS[i];',
 '    return NULL;',
 '}','']
target.write_text('\n'.join(lines))
print('Written',target,target.stat().st_size,'bytes')
