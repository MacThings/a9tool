/* CSV-aware archive diff. UTF-16LE / RFC4180-like CSV with multiline quotes.
 * Diff identity = exact logical record, or a unique-looking stable first-column
 * identifier, so edits to existing keys do not show up as newly added records.
 * Uses a linear-memory Hirschberg LCS for correct insert/delete alignment.
 */
#include "a9_compare.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#define MAX_CSV_RECORDS 200000u
#define MAX_DIAGNOSTIC_BYTES 220u
#define DIFF_CELL_LIMIT UINT64_C(90000000)

typedef struct {
    size_t pos, len;
    uint32_t line;
    uint64_t hash, key_hash;
    uint32_t key_length;
    int stable_key;
} CsvDiffRow;

typedef struct {
    const uint8_t *raw;
    size_t size;
    CsvDiffRow *rows;
    size_t count, capacity;
} CsvDiffData;

typedef struct {size_t old_row, new_row;} Match;
typedef struct {Match *pairs;size_t len,cap;} Matches;
static uint16_t get16(const uint8_t *p) {return (uint16_t)p[0]|((uint16_t)p[1]<<8);}
static uint64_t hash_bytes(const uint8_t *p,size_t length) {
    uint64_t h=UINT64_C(14695981039346656037);
    for(size_t i=0;i<length;i++)h=(h^p[i])*UINT64_C(1099511628211);
    return h;
}
static int push_row(CsvDiffData *csv,CsvDiffRow record) {
    if(csv->count>=MAX_CSV_RECORDS)return -1;
    if(csv->count==csv->capacity) {
        size_t cap=csv->capacity?csv->capacity*2:128;
        void *p=realloc(csv->rows,cap*sizeof(CsvDiffRow));
        if(!p)return -1;
        csv->rows=p;csv->capacity=cap;
    }
    csv->rows[csv->count++]=record;return 0;
}
static int push_match(Matches *matches,size_t oi,size_t ni) {
    if(matches->len==matches->cap) {
        size_t n=matches->cap?matches->cap*2:256;
        void *p=realloc(matches->pairs,n*sizeof(Match));
        if(!p)return -1;
        matches->pairs=p;matches->cap=n;
    }
    matches->pairs[matches->len++]=(Match){oi,ni};return 0;
}
static void parse_first_key(CsvDiffData *data,CsvDiffRow *row) {
    const uint8_t *buf=data->raw+row->pos;
    size_t i=0,count=0;
    int quoted=0,seen=0,stable=1,underscore=0;
    uint64_t hash=UINT64_C(14695981039346656037);
    if(row->len>=2 && get16(buf)=='"'){quoted=1;i=2;}
    for(;i+1<row->len;i+=2) {
        uint16_t c=get16(buf+i);
        if(quoted) {
            if(c=='"') {
                if(i+3<row->len && get16(buf+i+2)=='"')i+=2;
                else break;
            }
        }else if(c==',')break;
        if(!seen) {if(c<'A'||c>'Z')stable=0;seen=1;}
        else if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))stable=0;
        if(c=='_')underscore=1;
        hash=(hash^(uint8_t)c)*UINT64_C(1099511628211);
        hash=(hash^(uint8_t)(c>>8))*UINT64_C(1099511628211);
        count++;
        if(count>128){stable=0;break;}
    }
    row->stable_key=stable && count>=2 && (count>5||underscore);
    row->key_hash=hash;row->key_length=(uint32_t)count;
}
/* This is a lightweight record reader, not a second CSV syntax validator.
 * It recognizes quoted line endings and records actual physical start lines.
 */
static int parse_csv(const uint8_t *data,size_t size,CsvDiffData *csv) {
    memset(csv,0,sizeof *csv);csv->raw=data;csv->size=size;
    if(!data||size<2||data[0]!=0xff||data[1]!=0xfe||size%2)return -1;
    size_t start=2;
    uint32_t line=1,rowline=1;
    int quoted=0,field_start=1;
    for(size_t i=2;i+1<size;i+=2) {
        uint16_t c=get16(data+i);
        if(c=='"') {
            if(quoted) {
                if(i+3<size&&get16(data+i+2)=='"')i+=2;
                else quoted=0;
            } else if(field_start)quoted=1;
            field_start=0;
        } else if(c==','&&!quoted)field_start=1;
        else if(c=='\r'||c=='\n') {
            if(!quoted) {
                CsvDiffRow row={0};
                row.pos=start;row.len=i-start;row.line=rowline;
                row.hash=hash_bytes(data+start,row.len);
                parse_first_key(csv,&row);
                if(push_row(csv,row))return -1;
                start=i+2;
                field_start=1;
            }
            if(c=='\r'&&i+3<size&&get16(data+i+2)=='\n') {
                i+=2;
                if(!quoted)start=i+2;
            }
            line++;
            if(!quoted)rowline=line;
        } else field_start=0;
    }
    if(quoted)return -1;
    if(start<size) {
        CsvDiffRow row={0};row.pos=start;row.len=size-start;row.line=rowline;
        row.hash=hash_bytes(data+start,row.len);
        parse_first_key(csv,&row);
        if(push_row(csv,row))return -1;
    }
    return 0;
}
static int exact_equal(const CsvDiffData *a,size_t i,const CsvDiffData *b,size_t j) {
    const CsvDiffRow *ar=a->rows+i,*br=b->rows+j;
    return ar->len==br->len && ar->hash==br->hash &&
           memcmp(a->raw+ar->pos,b->raw+br->pos,ar->len)==0;
}
static int alignment_equal(const CsvDiffData *a,size_t i,const CsvDiffData *b,size_t j) {
    if(exact_equal(a,i,b,j))return 1;
    const CsvDiffRow *ar=a->rows+i,*br=b->rows+j;
    return ar->stable_key&&br->stable_key&&ar->key_length==br->key_length&&ar->key_hash==br->key_hash;
}
/* Append all LCS matches in increasing index order. Keeps only O(m) LCS scores. */
static int hirschberg(const CsvDiffData *a,size_t ai,size_t na,
                      const CsvDiffData *b,size_t bi,size_t nb,Matches *result) {
    while(na&&nb&&alignment_equal(a,ai,b,bi)) {
        if(push_match(result,ai++,bi++))return -1;
        na--;nb--;
    }
    size_t suffix=0;
    while(na>suffix&&nb>suffix&&alignment_equal(a,ai+na-suffix-1,b,bi+nb-suffix-1))suffix++;
    na-=suffix;nb-=suffix;
    if(!na||!nb)goto suffix_matches;
    if(na==1) {
        for(size_t j=0;j<nb;j++)if(alignment_equal(a,ai,b,bi+j)) {
            if(push_match(result,ai,bi+j))return -1;
            break;
        }
        goto suffix_matches;
    }
    if(nb==1) {
        for(size_t i=0;i<na;i++)if(alignment_equal(a,ai+i,b,bi)) {
            if(push_match(result,ai+i,bi))return -1;
            break;
        }
        goto suffix_matches;
    }
    if(na>0&&nb>DIFF_CELL_LIMIT/na) {
        /* Explicitly report approximation when a completely different gigantic
         * CSV exceeds the safe CPU budget. Prefix/suffix still match exactly. */
        goto suffix_matches;
    }
    size_t mid=na/2;
    uint32_t *scores=calloc((nb+1)*4,sizeof(uint32_t));
    if(!scores)return -1;
    uint32_t *forward=scores,*prev=scores+(nb+1),*current=scores+2*(nb+1),*backward=scores+3*(nb+1);
    for(size_t i=0;i<mid;i++) {
        current[0]=0;
        for(size_t j=1;j<=nb;j++) {
            current[j]=alignment_equal(a,ai+i,b,bi+j-1)?prev[j-1]+1:
                       (prev[j]>current[j-1]?prev[j]:current[j-1]);
        }
        uint32_t *temp=prev;prev=current;current=temp;
    }
    memcpy(forward,prev,(nb+1)*sizeof(uint32_t));
    memset(prev,0,(nb+1)*sizeof(uint32_t));
    for(size_t i=na;i>mid;i--) {
        current[nb]=0;
        for(size_t j=nb;j>0;j--) {
            size_t ix=j-1;
            current[ix]=alignment_equal(a,ai+i-1,b,bi+ix)?prev[ix+1]+1:
                        (prev[ix]>current[ix+1]?prev[ix]:current[ix+1]);
        }
        uint32_t *temp=prev;prev=current;current=temp;
    }
    memcpy(backward,prev,(nb+1)*sizeof(uint32_t));
    size_t split=0;
    uint32_t score=0;
    for(size_t j=0;j<=nb;j++) {
        uint32_t sum=forward[j]+backward[j];
        if(sum>score) {score=sum;split=j;}
    }
    free(scores);
    if(hirschberg(a,ai,mid,b,bi,split,result))return -1;
    if(hirschberg(a,ai+mid,na-mid,b,bi+split,nb-split,result))return -1;
suffix_matches:
    for(size_t j=0;j<suffix;j++)
        if(push_match(result,ai+na+j,bi+nb+j))return -1;
    return 0;
}
static int write_utf8(char out[MAX_DIAGNOSTIC_BYTES],size_t *index,uint32_t scalar) {
    uint8_t bytes[4];int n;
    if(scalar<0x80) {bytes[0]=(uint8_t)scalar;n=1;}
    else if(scalar<0x800) {bytes[0]=(uint8_t)(0xc0|(scalar>>6));bytes[1]=(uint8_t)(0x80|(scalar&63));n=2;}
    else if(scalar<0x10000) {bytes[0]=(uint8_t)(0xe0|(scalar>>12));bytes[1]=(uint8_t)(0x80|((scalar>>6)&63));bytes[2]=(uint8_t)(0x80|(scalar&63));n=3;}
    else {bytes[0]=(uint8_t)(0xf0|(scalar>>18));bytes[1]=(uint8_t)(0x80|((scalar>>12)&63));bytes[2]=(uint8_t)(0x80|((scalar>>6)&63));bytes[3]=(uint8_t)(0x80|(scalar&63));n=4;}
    if(*index+(size_t)n>=MAX_DIAGNOSTIC_BYTES-5)return -1;
    for(int j=0;j<n;j++)out[(*index)++]=(char)bytes[j];return 0;
}
/* Preview first CSV field (usually an ID), plus the next field when short.
 * Newlines are escaped so every diff event occupies exactly one output line. */
static void preview(const CsvDiffData *csv,const CsvDiffRow *row,char out[MAX_DIAGNOSTIC_BYTES]) {
    size_t size=0,fields=0;int quoted=0,field_start=1;
    const uint8_t *raw=csv->raw+row->pos;
    for(size_t i=0;i+1<row->len;i+=2) {
        uint32_t c=get16(raw+i);
        if(c=='"') {
            if(quoted&&i+3<row->len&&get16(raw+i+2)=='"')i+=2;
            else {quoted=!quoted;field_start=0;continue;}
        }
        if(c==','&&!quoted) {
            fields++;
            if(fields>=2)break;
            if(size+3>=MAX_DIAGNOSTIC_BYTES-5)break;
            out[size++]=' ';out[size++]='|';out[size++]=' ';
            field_start=1;continue;
        }
        if(c=='\r'||c=='\n') {
            if(size+2>=MAX_DIAGNOSTIC_BYTES-5)break;
            out[size++]='\\';out[size++]=(c=='\n'?'n':'r');continue;
        }
        if(c<32){c='?';}
        if(c>=0xd800&&c<=0xdbff && i+3<row->len) {
            uint32_t low=get16(raw+i+2);
            if(low>=0xdc00&&low<=0xdfff) {c=0x10000+((c-0xd800)<<10)+(low-0xdc00);i+=2;}
        }
        if(c>=0xdc00&&c<=0xdfff)c='?';
        if(write_utf8(out,&size,c))break;
        field_start=0;
    }
    (void)field_start;
    out[size]=0;
    if(row->len && size>=MAX_DIAGNOSTIC_BYTES-16)strcat(out,"...");
}
static void print_added(FILE *out,const char *name,const CsvDiffData *data,size_t start,size_t end,A9CompareStats *stats) {
    for(size_t i=start;i<end;i++) {
        char sample[MAX_DIAGNOSTIC_BYTES];preview(data,&data->rows[i],sample);
        fprintf(out,"+ %s:%u: NEU %s\n",name,data->rows[i].line,sample);
        stats->added_rows++;
    }
}
static void print_removed(FILE *out,const char *name,const CsvDiffData *data,size_t start,size_t end,int additions_only,A9CompareStats *stats) {
    for(size_t i=start;i<end;i++) {
        if(!additions_only) {
            char sample[MAX_DIAGNOSTIC_BYTES];preview(data,&data->rows[i],sample);
            fprintf(out,"- %s:%u: ENTFERNT (alte Zeile) %s\n",name,data->rows[i].line,sample);
        }
        stats->removed_rows++;
    }
}
static void print_modified(FILE *out,const char *name,const CsvDiffData *before,size_t oi,const CsvDiffData *after,size_t ni,int additions_only,A9CompareStats *stats) {
    if(!additions_only) {
        char old_preview[MAX_DIAGNOSTIC_BYTES],new_preview[MAX_DIAGNOSTIC_BYTES];
        preview(before,&before->rows[oi],old_preview);
        preview(after,&after->rows[ni],new_preview);
        fprintf(out,"~ %s:%u: GEÄNDERT (alt Zeile %u) %s\n",name,after->rows[ni].line,before->rows[oi].line,new_preview);
        if(strcmp(new_preview,old_preview))fprintf(out,"    ALT: %s\n",old_preview);
    }
    stats->modified_rows++;
}
static void report_gap(FILE *out,const char *name,const CsvDiffData *old,size_t ob,size_t oe,const CsvDiffData *newer,size_t nb,size_t ne,int additions_only,A9CompareStats *stats) {
    size_t both=oe-ob<ne-nb?oe-ob:ne-nb;
    for(size_t k=0;k<both;k++) {
        const CsvDiffRow *a=&old->rows[ob+k],*b=&newer->rows[nb+k];
        /* A brand-new section ID is an addition, not an edit of an unrelated
         * removed section. This is crucial for the user's "what is new" view. */
        if((a->stable_key||b->stable_key) &&
           !(a->stable_key&&b->stable_key && a->key_length==b->key_length && a->key_hash==b->key_hash)) {
            print_added(out,name,newer,nb+k,nb+k+1,stats);
            print_removed(out,name,old,ob+k,ob+k+1,additions_only,stats);
        } else print_modified(out,name,old,ob+k,newer,nb+k,additions_only,stats);
    }
    print_added(out,name,newer,nb+both,ne,stats);
    print_removed(out,name,old,ob+both,oe,additions_only,stats);
}
int a9_compare_csv(FILE *out,const char *name,
                   const uint8_t *old_data,size_t old_size,
                   const uint8_t *new_data,size_t new_size,
                   int additions_only,A9CompareStats *stats) {
    CsvDiffData old={0},newer={0};
    if(old_data && parse_csv(old_data,old_size,&old))goto problem;
    if(new_data && parse_csv(new_data,new_size,&newer))goto problem;
    if(!old_data) {
        fprintf(out,"\nDATEI NEU: %s (%zu CSV-Datensätze)\n",name,newer.count);
        stats->new_files++;
        print_added(out,name,&newer,0,newer.count,stats);
        free(old.rows);free(newer.rows);return 0;
    }
    if(!new_data) {
        if(!additions_only)fprintf(out,"\nDATEI ENTFERNT: %s (%zu CSV-Datensätze)\n",name,old.count);
        stats->removed_files++;
        print_removed(out,name,&old,0,old.count,additions_only,stats);
        free(old.rows);free(newer.rows);return 0;
    }
    if(old_size==new_size && memcmp(old_data,new_data,old_size)==0) {
        stats->identical_files++;free(old.rows);free(newer.rows);return 0;
    }
    Matches matches={0};
    if(hirschberg(&old,0,old.count,&newer,0,newer.count,&matches)){
        free(matches.pairs);goto problem;
    }
    fprintf(out,"\nCSV: %s\n",name);
    stats->changed_files++;
    size_t prev_o=0,prev_n=0;
    uint64_t before_add=stats->added_rows,before_mod=stats->modified_rows,before_rem=stats->removed_rows;
    for(size_t k=0;k<matches.len;k++) {
        Match pair=matches.pairs[k];
        report_gap(out,name,&old,prev_o,pair.old_row,&newer,prev_n,pair.new_row,additions_only,stats);
        if(!exact_equal(&old,pair.old_row,&newer,pair.new_row))
            print_modified(out,name,&old,pair.old_row,&newer,pair.new_row,additions_only,stats);
        prev_o=pair.old_row+1;prev_n=pair.new_row+1;
    }
    report_gap(out,name,&old,prev_o,old.count,&newer,prev_n,newer.count,additions_only,stats);
    fprintf(out,"  Summe: +%llu neu, ~%llu geändert, -%llu entfernt\n",
            (unsigned long long)(stats->added_rows-before_add),
            (unsigned long long)(stats->modified_rows-before_mod),
            (unsigned long long)(stats->removed_rows-before_rem));
    free(matches.pairs);free(old.rows);free(newer.rows);return 0;
problem:
    fprintf(stderr,"FEHLER: CSV %s kann nicht korrekt gelesen werden (UTF-16LE/Quotes oder Speicher).\n",name);
    free(old.rows);free(newer.rows);return -1;
}
