#define _POSIX_C_SOURCE 200809L
/* Reconstructed CSVP CSV syntax and structural validator for A9Codec.
 * CSV source encoding: UTF-16LE BOM. Logical records can contain quoted
 * multiline text, so record numbers and physical line numbers differ.
 */
#include "a9_csv_validator.h"
#include "csvp_schema.h"
#include "a9_port.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>


#define CSV_PATH_MAX 8192
#define CSV_RECORD_LIMIT 200000u
#define CSV_FILE_LIMIT ((size_t)1<<30)
#define CSV_MAX_ERRORS_PER_FILE 30u

typedef struct {
    uint32_t line;
    uint32_t columns;
    uint32_t id_hash;
    int is_identifier;
} CsvRecord;
typedef struct {CsvRecord *items;size_t length,capacity;} CsvRecords;
typedef struct {
    unsigned errors,warnings;
    unsigned files,records;
    unsigned file_errors,file_warnings;
    const char *path;
    int silent_errors;
} Report;
static void diag(Report *r,uint32_t line,int error,const char *msg) {
    if(error) {r->errors++;r->file_errors++;}else {r->warnings++;r->file_warnings++;}
    if(r->file_errors+r->file_warnings<=CSV_MAX_ERRORS_PER_FILE)
        fprintf(error?stderr:stdout,"%s:%u: %s: %s\n",r->path,line,error?"FEHLER":"WARNUNG",msg);
    else if(r->file_errors+r->file_warnings==CSV_MAX_ERRORS_PER_FILE+1u)
        fprintf(stderr,"%s:%u: (weitere Meldungen für diese Datei unterdrückt)\n",r->path,line);
}
static int join_path(char out[CSV_PATH_MAX],const char *dir,const char *name) {
    size_t a=strlen(dir),b=strlen(name);
    if(a+b+2>=CSV_PATH_MAX)return -1;
    memcpy(out,dir,a);
    if(a&&dir[a-1]!='/')out[a++]='/';
    memcpy(out+a,name,b+1);return 0;
}
static int safe_name(const char *s) {
    size_t n=strlen(s);
    if(n==0||n>250||!strcmp(s,".")||!strcmp(s,".."))return 0;
    for(const unsigned char *p=(const unsigned char *)s;*p;p++)
        if(*p<32||*p=='/'||*p=='\\'||*p==':')return 0;
    return 1;
}
static int push_record(CsvRecords *records,const CsvRecord *r) {
    if(records->length>=CSV_RECORD_LIMIT)return -1;
    if(records->length==records->capacity) {
        size_t capacity=records->capacity?records->capacity*2:256;
        void *p=realloc(records->items,capacity*sizeof(CsvRecord));
        if(!p)return -1;
        records->items=p;records->capacity=capacity;
    }
    records->items[records->length++]=*r;return 0;
}
static int read_csv(const char *path,uint16_t **data,size_t *length,Report *report) {
    FILE *f=a9_fopen(path,"rb");
    if(!f){diag(report,0,1,"Datei nicht lesbar / fehlt");return -1;}
    if(fseek(f,0,SEEK_END)||ftell(f)<0){diag(report,0,1,"Dateigröße nicht lesbar");fclose(f);return -1;}
    long size=ftell(f);rewind(f);
    if((uint64_t)size>CSV_FILE_LIMIT){diag(report,0,1,"Datei zu groß");fclose(f);return -1;}
    if(size<2) {diag(report,1,1,"Datei leer bzw. UTF-16LE-BOM fehlt");fclose(f);return -1;}
    unsigned char *raw=malloc((size_t)size);
    if(!raw){diag(report,0,1,"Kein Speicher");fclose(f);return -1;}
    int ok=fread(raw,1,(size_t)size,f)==(size_t)size;
    fclose(f);
    if(!ok){diag(report,0,1,"Lesefehler");free(raw);return -1;}
    if(raw[0]!=0xff||raw[1]!=0xfe){diag(report,1,1,"UTF-16LE BOM (FF FE) fehlt; CSV-Zeichencodierung falsch");free(raw);return -1;}
    if(size&1){diag(report,1,1,"Ungerade Bytezahl: beschädigtes UTF-16LE");free(raw);return -1;}
    size_t count=(size_t)size/2-1;
    uint16_t *units=malloc((count+1)*sizeof(uint16_t));
    if(!units){diag(report,0,1,"Kein Speicher");free(raw);return -1;}
    for(size_t i=0;i<count;i++)units[i]=(uint16_t)raw[2+i*2] | ((uint16_t)raw[3+i*2]<<8);
    free(raw);
    units[count]=0;
    *data=units;*length=count;return 0;
}
static unsigned expected_columns(const CsvSchema *schema,uint32_t reference_record) {
    if(!schema)return 0;
    /* sorted by record number: small arrays, binary lookup */
    size_t lo=0,hi=schema->width_count;
    while(lo<hi) {
        size_t mid=lo+(hi-lo)/2;
        if(schema->widths[mid].record<reference_record)lo=mid+1;
        else hi=mid;
    }
    if(lo<schema->width_count && schema->widths[lo].record==reference_record)
        return schema->widths[lo].columns;
    return schema->default_columns;
}
static uint32_t find_anchor(const CsvSchema *schema,uint32_t hash,uint32_t suggested) {
    if(!schema || !hash)return 0;
    uint32_t best=0,dist=UINT32_MAX;
    for(size_t i=0;i<schema->anchor_count;i++) {
        if(schema->anchors[i].hash!=hash)continue;
        uint32_t p=schema->anchors[i].record;
        uint32_t d=p>suggested?p-suggested:suggested-p;
        if(d<dist){dist=d;best=p;}
    }
    return best;
}
static int push_complete_record(CsvRecords *records,Report *report,
                                uint32_t line,uint32_t columns,
                                const uint16_t *first,size_t firstlen,int first_overflow) {
    CsvRecord r={line,columns,0,0};
    if(!first_overflow&&firstlen>1 && firstlen<=128) {
        int id=(first[0]>='A'&&first[0]<='Z');
        int has_underscore=0;
        for(size_t i=1;i<firstlen;i++) {
            if(!((first[i]>='A'&&first[i]<='Z')||(first[i]>='0'&&first[i]<='9')||first[i]=='_'))id=0;
            if(first[i]=='_')has_underscore=1;
        }
        if(id && (firstlen>5 || has_underscore)) {
            r.id_hash=csvp_anchor_hash(first,firstlen);r.is_identifier=1;
        }
    }
    if(push_record(records,&r)){diag(report,line,1,"Zu viele CSV-Datensätze oder kein Speicher");return -1;}
    return 0;
}
static int parse_csv(const uint16_t *data,size_t length,const CsvSchema *schema,
                     CsvRecords *records,Report *report) {
    enum {START,UNQUOTED,QUOTED,AFTER_QUOTE} state=START;
    uint16_t first[129]={0};size_t firstlen=0;
    uint32_t line=1,record_line=1,columns=1,physical_length=0;
    int first_overflow=0;
    int row_seen=0;
    uint32_t line_limit=schema?schema->max_physical_line:1024u;
    /* Don't flag normal text extensions; report only substantial growth. */
    if(line_limit<512u)line_limit=512u;
    else if(line_limit<=UINT32_MAX/2)line_limit*=2u;
    int linewarn=0,line_error=0;
    for(size_t i=0;i<length;i++) {
        uint16_t c=data[i];
        if(c==0xfeff){diag(report,line,1,"Unerwartetes UTF-BOM innerhalb des Inhalts");continue;}
        if(c==0){diag(report,line,1,"NUL-Zeichen in CSV");continue;}
        if(c<32 && c!='\r' && c!='\n' && c!='\t')
            diag(report,line,1,"Unerlaubtes Steuerzeichen in CSV");
        if(c>=0xd800&&c<=0xdbff) {
            if(i+1>=length||data[i+1]<0xdc00||data[i+1]>0xdfff)
                diag(report,line,1,"Ungültiges UTF-16 Surrogatpaar");
        }else if(c>=0xdc00&&c<=0xdfff && (i==0||data[i-1]<0xd800||data[i-1]>0xdbff))
            diag(report,line,1,"Verwaistes UTF-16 Low Surrogate");
        int newline=c=='\n'||c=='\r';
        if(!newline) {
            physical_length++;
            if(physical_length>32768u && !line_error) {
                diag(report,line,1,"Physische CSV-Zeile überschreitet das Sicherheitslimit von 32768 UTF-16-Zeichen");
                line_error=1;
            } else if(physical_length>line_limit && !linewarn) {
                char buf[220];
                snprintf(buf,sizeof buf,"Physische Zeile ungewöhnlich lang (> %u UTF-16-Zeichen; Referenzmaximum %u)",line_limit,schema?schema->max_physical_line:0u);
                diag(report,line,0,buf);linewarn=1;
            }
        }
        if(newline) {
            if(state!=QUOTED) {
                if(push_complete_record(records,report,record_line,row_seen?columns:0,first,firstlen,first_overflow))return -1;
                state=START;columns=1;row_seen=0;firstlen=0;first_overflow=0;
            }
            if(c=='\r' && i+1<length && data[i+1]=='\n')i++;
            line++;record_line=state==QUOTED?record_line:line;
            physical_length=0;linewarn=0;line_error=0;continue;
        }
        row_seen=1;
        if(state==QUOTED) {
            if(c=='"') {
                if(i+1<length && data[i+1]=='"'){
                    if(columns==1) {if(firstlen<128)first[firstlen++]=c;else first_overflow=1;}
                    i++;
                } else state=AFTER_QUOTE;
            } else if(columns==1) {
                if(firstlen<128)first[firstlen++]=c;else first_overflow=1;
            }
        } else if(state==AFTER_QUOTE) {
            if(c==','){columns++;state=START;}
            else {diag(report,line,1,"Unerlaubtes Zeichen nach schließendem Anführungszeichen (erwartet Komma oder Zeilenende)");state=UNQUOTED;}
        } else if(c==',') {
            columns++;state=START;
        } else if(c=='"' && state==START)state=QUOTED;
        else {
            if(columns==1) {if(firstlen<128)first[firstlen++]=c;else first_overflow=1;}
            if(c=='"')diag(report,line,1,"Anführungszeichen innerhalb unquotiertem Feld");
            state=UNQUOTED;
        }
    }
    if(state==QUOTED)diag(report,record_line,1,"Nicht geschlossenes Anführungszeichen; CSV-Abschnitt bis Dateiende offen");
    if(row_seen && push_complete_record(records,report,record_line,columns,first,firstlen,first_overflow))return -1;
    if(records->length==0)diag(report,1,1,"Keine CSV-Datensätze gefunden");
    return 0;
}
static int validate_csv(const char *path,const char *name,Report *report) {
    report->path=path;report->file_errors=0;report->file_warnings=0;
    const CsvSchema *schema=csvp_find_schema(name);
    uint16_t *data=NULL;size_t n=0;
    if(read_csv(path,&data,&n,report))return -1;
    CsvRecords rows={0};
    if(parse_csv(data,n,schema,&rows,report)){free(data);free(rows.items);return -1;}
    free(data);
    if(!schema)diag(report,1,0,"Kein Referenzschema bekannt; nur UTF-16LE und CSV-Syntax geprüft");
    int64_t shift=0;
    uint32_t last_anchor=0;
    uint8_t *seen_anchors=schema&&schema->anchor_count?calloc(schema->anchor_count,1):NULL;
    for(size_t i=0;i<rows.length;i++) {
        CsvRecord *row=rows.items+i;
        uint32_t current=(uint32_t)(i+1);
        int64_t logical=(int64_t)current-shift;
        uint32_t expected_position=logical>0&&logical<=UINT32_MAX?(uint32_t)logical:current;
        uint32_t anchor=0;
        if(row->is_identifier && schema)anchor=find_anchor(schema,row->id_hash,expected_position);
        if(anchor) {
            if(seen_anchors) {
                for(size_t a=0;a<schema->anchor_count;a++)
                    if(schema->anchors[a].record==anchor&&schema->anchors[a].hash==row->id_hash){
                        if(seen_anchors[a])diag(report,row->line,0,"Abschnitts-ID mehrfach vorhanden");
                        seen_anchors[a]=1;break;
                    }
            }
            if(anchor<=last_anchor)diag(report,row->line,0,"Abschnitts-ID doppelt oder nicht in Referenzreihenfolge");
            else last_anchor=anchor;
            expected_position=anchor;
            shift=(int64_t)current-(int64_t)anchor;
        } else if(row->is_identifier && schema && schema->anchor_count>0) {
            diag(report,row->line,0,"Unbekannte Abschnitts-ID (nicht im Referenzschema der Original-csvp.bin)");
        }
        if(schema) {
            unsigned expected=expected_columns(schema,expected_position);
            if(expected_position>schema->records || (row->is_identifier && !anchor)) {
                /* Added data or new section: compare with predominant layout. */
                expected=schema->default_columns;
            }
            if(row->columns!=expected && expected) {
                char buf[210];
                snprintf(buf,sizeof buf,"Falsche Spaltenzahl in CSV-Abschnitt: %u gefunden, %u erwartet (Datensatz %u)",row->columns,expected,current);
                diag(report,row->line,1,buf);
            }
        } else if(i && row->columns!=rows.items[0].columns) {
            char buf[160];snprintf(buf,sizeof buf,"Spaltenzahl %u, erste Zeile hat %u (ohne Schema nur Warnung)",row->columns,rows.items[0].columns);
            diag(report,row->line,0,buf);
        }
    }
    if(schema && seen_anchors) {
        unsigned absent=0;uint32_t first_ref=0;
        for(size_t j=0;j<schema->anchor_count;j++)if(!seen_anchors[j]){
            absent++;if(!first_ref)first_ref=schema->anchors[j].record;
        }
        if(absent) {
            char buf[200];
            snprintf(buf,sizeof buf,"%u bekannte Abschnitts-ID(s) aus Referenz fehlen (erste im Referenz-Datensatz %u); Abschnitte auf Löschung/Umbenennung prüfen",absent,first_ref);
            uint32_t line=rows.length?rows.items[first_ref<=rows.length?first_ref-1:rows.length-1].line:1;
            diag(report,line,0,buf);
        }
    }
    free(seen_anchors);
    if(schema && schema->records!=rows.length) {
        char buf[190];
        snprintf(buf,sizeof buf,"Anzahl Datensätze geändert: %zu statt Referenz %u (neue/entfernte Abschnitte prüfen)",rows.length,schema->records);
        diag(report,rows.length?rows.items[rows.length-1].line:1,0,buf);
    }
    report->files++;report->records+=(unsigned)rows.length;
    if(!report->file_errors && !report->file_warnings)
        printf("OK      %-30s %zu Datensätze\n",name,rows.length);
    else printf("PRÜFUNG %-30s %u Fehler, %u Warnungen\n",name,report->file_errors,report->file_warnings);
    free(rows.items);return 0;
}
/* If FOLDER has only CSV files, scan them directly (no files.txt needed). */
static int check_direct_csv_folder(const char *folder) {
    A9Dir *dir=a9_dir_open(folder);
    if(!dir){fprintf(stderr,"%s:0: FEHLER: Ordner nicht lesbar\n",folder);return 1;}
    Report report={0};
    printf("CSV-Validierung: %s (direkter CSV-Ordner)\n",folder);
    const A9Dirent *entry;
    while((entry=a9_dir_read(dir))) {
        const char *name=entry->name;size_t len=strlen(name);
        if(len<4||strcmp(name+len-4,".csv"))continue;
        char full[CSV_PATH_MAX];
        if(join_path(full,folder,name)){
            fprintf(stderr,"FEHLER: CSV-Pfad zu lang: %s\n",name);report.errors++;continue;
        }
        validate_csv(full,name,&report);
    }
    a9_dir_close(dir);
    if(report.files==0){fprintf(stderr,"FEHLER: Keine CSV-Dateien gefunden\n");report.errors++;}
    printf("CSV-Check abgeschlossen: %u Dateien, %u Datensätze, %u Fehler, %u Warnungen\n",
           report.files,report.records,report.errors,report.warnings);
    return report.errors?1:0;
}
int a9_csv_check_folder(const char *folder) {
    char manifest_path[CSV_PATH_MAX],data_dir[CSV_PATH_MAX];
    if(join_path(manifest_path,folder,"files.txt")||join_path(data_dir,folder,"data")){
        fprintf(stderr,"FEHLER: Ordnerpfad zu lang: %s\n",folder);return 1;
    }
    FILE *list=a9_fopen(manifest_path,"rb");
    if(!list)return check_direct_csv_folder(folder);
    Report summary={0};
    char **seen=NULL;size_t seen_n=0,seen_cap=0;
    char *line=NULL;size_t cap=0;
    ptrdiff_t n;
    printf("CSV-Validierung: %s\n",folder);
    while((n=a9_getline(&line,&cap,list))>=0) {
        while(n>0 && (line[n-1]=='\n'||line[n-1]=='\r'))line[--n]='\0';
        if(n==0||line[0]=='#')continue;
        if(!safe_name(line)) {
            fprintf(stderr,"%s:0: FEHLER: Ungültiger Dateiname in files.txt\n",manifest_path);summary.errors++;continue;
        }
        int duplicate=0;
        for(size_t i=0;i<seen_n;i++)if(!strcmp(line,seen[i])){duplicate=1;break;}
        if(duplicate) {
            fprintf(stderr,"%s:0: FEHLER: Datei doppelt in files.txt: %s\n",manifest_path,line);
            summary.errors++;continue;
        }
        if(seen_n==seen_cap) {
            size_t nc=seen_cap?seen_cap*2:64;
            char **p=realloc(seen,nc*sizeof(char *));
            if(!p){fprintf(stderr,"Kein Speicher\n");summary.errors++;break;}
            seen=p;seen_cap=nc;
        }
        seen[seen_n]=a9_strdup(line);
        if(!seen[seen_n]){summary.errors++;break;}
        seen_n++;
        size_t m=strlen(line);
        if(m<4 || strcmp(line+m-4,".csv"))continue;
        char path[CSV_PATH_MAX];
        if(join_path(path,data_dir,line)){
            fprintf(stderr,"FEHLER: Dateipfad zu lang: %s\n",line);summary.errors++;continue;
        }
        validate_csv(path,line,&summary);
    }
    free(line);fclose(list);
    /* A CSV in data/ but absent in files.txt is ignored when packing. */
    A9Dir *dir=a9_dir_open(data_dir);
    if(dir) {
        const A9Dirent *entry;
        while((entry=a9_dir_read(dir))) {
            const char *name=entry->name;size_t length=strlen(name);
            if(length<4||strcmp(name+length-4,".csv"))continue;
            int present=0;
            for(size_t i=0;i<seen_n;i++)if(!strcmp(name,seen[i])){present=1;break;}
            if(!present) {
                char p[CSV_PATH_MAX];if(join_path(p,data_dir,name))continue;
                summary.path=p;summary.file_errors=summary.file_warnings=0;
                diag(&summary,1,0,"CSV-Datei steht nicht in files.txt und wird nicht gepackt");
            }
        }
        a9_dir_close(dir);
    }
    for(size_t i=0;i<seen_n;i++)free(seen[i]);free(seen);
    if(!summary.files){fprintf(stderr,"FEHLER: Keine CSV-Dateien in files.txt\n");summary.errors++;}
    printf("CSV-Check abgeschlossen: %u Dateien, %u Datensätze, %u Fehler, %u Warnungen\n",
           summary.files,summary.records,summary.errors,summary.warnings);
    return summary.errors?1:0;
}

int a9_csv_folder_has_csv(const char *folder) {
    char path[CSV_PATH_MAX];
    if(join_path(path,folder,"files.txt"))return 0;
    FILE *f=a9_fopen(path,"rb");
    if(f) {
        char *line=NULL;size_t cap=0;ptrdiff_t n;
        int result=0;
        while((n=a9_getline(&line,&cap,f))>=0) {
            while(n>0 && (line[n-1]=='\n'||line[n-1]=='\r'))line[--n]='\0';
            if(n>=4&&!strcmp(line+n-4,".csv")){result=1;break;}
        }
        free(line);fclose(f);return result;
    }
    A9Dir *dir=a9_dir_open(folder);
    if(!dir)return 0;
    const A9Dirent *e;int result=0;
    while((e=a9_dir_read(dir))) {
        size_t n=strlen(e->name);
        if(n>=4&&!strcmp(e->name+n-4,".csv")){result=1;break;}
    }
    a9_dir_close(dir);return result;
}
