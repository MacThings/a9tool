/* a9tool: portable A-Train 9 archive unpacker and packer.
 * Based on structural reconstruction of a9codec.exe (2013-10-17) using
 * symbol names and x86 disassembly. Not original source code.
 * Compile with C11 on macOS or Linux: no external libraries required.
 */
#define _POSIX_C_SOURCE 200809L
#include "a9_lzss.h"
#include "a9_md5.h"
#include "a9_table.h"
#include "a9_csv_validator.h"
#include "a9_compare.h"
#include "a9_port.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <ctype.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define A9_LIMIT ((size_t)1 << 30)
#define MAX_FILES 32000
#define NAME_BYTES 512
#define PATH_BUF 8192

typedef struct { uint8_t *ptr;size_t len,cap; } Bytes;
static int verbose=0, force=0, intermediates=0, skip_check=0;

static void usage(const char *exe) {
    fprintf(stderr,
      "a9tool (native C11 macOS/Linux, a9codec 2013-10-17 compatible)\n"
      "Usage:\n"
      "  %s -x archive.bin [-o output_directory] [-t 0|1] [--intermediate]\n"
      "  %s -c input_directory [-o output.bin] [-t 0|1] [--force]\n"
      "  %s -i archive.bin [-t 0|1]             Inspect / verify\n"
      "  %s check FOLDER                       Validate extracted CSV files\n"
      "  %s compare OLD.bin NEW.bin [-o report.txt] [--only-added]\n"
      "Options:\n"
      "  -x                Decode/extract an A9 archive\n"
      "  -c                Encode/repack an extracted directory (requires files.txt)\n"
      "  -i                Verify and inspect archive without writing files\n"
      "  -o <path>         Set output directory / output file\n"
      "  -t 0|1            0=csvp/cs*.bin, 1=mt*.bin (default 0)\n"
      "  -v / -vv         Verbose / extra verbose\n"
      "  --intermediate    Also write key, decrypted.data and decompressed image\n"
      "  --force           Allow writing to an existing output path\n"
      "  --no-check        Skip CSV validation when creating archive\n"
      "  --only-added      Compare: show additions only\n"
      "  -h, --help        Show help\n"
      "Example:\n"
      "  %s -x csvp.bin -o csvp_new\n"
      "  %s -c csvp_new -o rebuilt_csvp.bin\n",
      exe,exe,exe,exe,exe,exe,exe);
}
static int fail(const char *reason,const char *path) {
    if(path)fprintf(stderr,"ERROR: %s: %s\n",reason,path);
    else fprintf(stderr,"ERROR: %s\n",reason);
    return 1;
}
static int push(Bytes *buffer,const void *data,size_t size) {
    if(size>A9_LIMIT-buffer->len)return -1;
    if(size>buffer->cap-buffer->len) {
        size_t cap=buffer->cap?buffer->cap:256;
        while(cap<buffer->len+size) {
            if(cap>A9_LIMIT/2){cap=A9_LIMIT;break;}
            cap*=2;
        }
        void *p=realloc(buffer->ptr,cap);
        if(!p)return -1;
        buffer->ptr=p;buffer->cap=cap;
    }
    if(size)memcpy(buffer->ptr+buffer->len,data,size);
    buffer->len+=size;return 0;
}
static void set32(uint8_t *p,uint32_t n) {
    p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);p[2]=(uint8_t)(n>>16);p[3]=(uint8_t)(n>>24);
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static int push32(Bytes *buffer,uint32_t num) {uint8_t le[4];set32(le,num);return push(buffer,le,4);}
static int mkdir_tree(const char *path) {
    char scratch[PATH_BUF];size_t len=strlen(path);
    if(len>=sizeof scratch)return -1;
    memcpy(scratch,path,len+1);
    for(size_t i=0;i<len;i++)if(scratch[i]=='\\')scratch[i]='/';
    for(size_t i=1;i<=len;i++) {
        if(scratch[i]=='/'||scratch[i]=='\0') {
            char old=scratch[i];scratch[i]='\0';
            if(*scratch && !(i==2&&scratch[1]==':') && a9_mkdir(scratch)<0 && errno!=EEXIST)return -1;
            scratch[i]=old;
        }
    }
    return 0;
}
static int path_join(char out[PATH_BUF],const char *dir,const char *name) {
    size_t n=strlen(dir),m=strlen(name);
    if(n+m+2>=PATH_BUF)return -1;
    memcpy(out,dir,n);
    if(n && dir[n-1]!='/')out[n++]='/';
    memcpy(out+n,name,m+1);
    return 0;
}
static int is_safe_filename(const char *name) {
    if(!name || !*name || strlen(name)>250 || !strcmp(name,".") || !strcmp(name,".."))return 0;
    /* Source format has flat file names; refuse traversal and platform separators. */
    for(const unsigned char *p=(const unsigned char *)name;*p;p++) {
        if(*p<32 || *p=='/' || *p=='\\' || *p==':')return 0;
    }
    return 1;
}
static int file_read(const char *path,Bytes *out) {
    FILE *f=a9_fopen(path,"rb");if(!f)return -1;
    if(fseek(f,0,SEEK_END)||ftell(f)<0) {fclose(f);return -1;}
    long len=ftell(f);
    if((uint64_t)len>A9_LIMIT || fseek(f,0,SEEK_SET)) {fclose(f);return -1;}
    out->ptr=malloc(len>0?(size_t)len:1);if(!out->ptr){fclose(f);return -1;}
    out->cap=out->len=(size_t)len;
    int ok=(!len || fread(out->ptr,1,out->len,f)==out->len);
    fclose(f);
    if(!ok){free(out->ptr);out->ptr=NULL;return -1;}
    return 0;
}
static int file_write(const char *path,const void *data,size_t length,int allow_overwrite) {
    if(!allow_overwrite&&a9_exists(path))return fail("Refusing to overwrite (use --force)",path);
    FILE *f=a9_fopen(path,"wb");if(!f)return fail(strerror(errno),path);
    int ok=length==0 || fwrite(data,1,length,f)==length;
    if(fclose(f))ok=0;
    if(!ok)return fail("Write failed",path);
    return 0;
}
static int emit_file(const char *dir,const char *name,const void *data,size_t size) {
    char full[PATH_BUF];
    if(path_join(full,dir,name))return fail("Path is too long",name);
    return file_write(full,data,size,force);
}
static int utf8_to_utf16(const char *str,uint8_t out[NAME_BYTES]) {
    memset(out,0,NAME_BYTES);
    const unsigned char *p=(const unsigned char *)str;
    size_t index=0;
    while(*p) {
        uint32_t scalar;
        if(*p<0x80){scalar=*p++;}
        else if((*p&0xE0)==0xC0 && p[1] && (p[1]&0xC0)==0x80) {
            scalar=((uint32_t)(p[0]&31)<<6)|(p[1]&63);p+=2;
            if(scalar<0x80)return -1;
        } else if((*p&0xF0)==0xE0 && p[1] && p[2] && (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80) {
            scalar=((uint32_t)(p[0]&15)<<12)|((uint32_t)(p[1]&63)<<6)|(p[2]&63);p+=3;
            if(scalar<0x800)return -1;
        } else if((*p&0xF8)==0xF0 && p[1]&&p[2]&&p[3] &&
                  (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80 && (p[3]&0xC0)==0x80) {
            scalar=((uint32_t)(p[0]&7)<<18)|((uint32_t)(p[1]&63)<<12)|((uint32_t)(p[2]&63)<<6)|(p[3]&63);p+=4;
            if(scalar<0x10000 || scalar>0x10FFFF)return -1;
        } else return -1;
        if(scalar>=0xD800 && scalar<=0xDFFF)return -1;
        if(scalar>0xFFFF) {
            if(index+2>255)return -1;
            scalar-=0x10000;
            uint16_t hi=(uint16_t)(0xD800+(scalar>>10));
            uint16_t lo=(uint16_t)(0xDC00+(scalar&1023));
            out[2*index]=(uint8_t)hi;out[2*index+1]=(uint8_t)(hi>>8);index++;
            out[2*index]=(uint8_t)lo;out[2*index+1]=(uint8_t)(lo>>8);index++;
        } else {
            if(index>=255)return -1;
            out[2*index]=(uint8_t)scalar;out[2*index+1]=(uint8_t)(scalar>>8);index++;
        }
    }
    return 0;
}
static int utf16_to_utf8(const uint8_t in[NAME_BYTES],char out[1024]) {
    size_t outlen=0;
    for(size_t i=0;i<256;i++) {
        uint32_t cp=(uint32_t)in[i*2]|((uint32_t)in[i*2+1]<<8);
        if(!cp)break;
        if(cp>=0xD800&&cp<=0xDBFF) {
            if(++i>=256)return -1;
            uint32_t lo=(uint32_t)in[i*2]|((uint32_t)in[i*2+1]<<8);
            if(lo<0xDC00||lo>0xDFFF)return -1;
            cp=0x10000+((cp-0xD800)<<10)+(lo-0xDC00);
        } else if(cp>=0xDC00&&cp<=0xDFFF)return -1;
        if(outlen+4>=1024)return -1;
        if(cp<=0x7F)out[outlen++]=(char)cp;
        else if(cp<=0x7FF){out[outlen++]=(char)(0xC0|(cp>>6));out[outlen++]=(char)(0x80|(cp&63));}
        else if(cp<=0xFFFF){out[outlen++]=(char)(0xE0|(cp>>12));out[outlen++]=(char)(0x80|((cp>>6)&63));out[outlen++]=(char)(0x80|(cp&63));}
        else {out[outlen++]=(char)(0xF0|(cp>>18));out[outlen++]=(char)(0x80|((cp>>12)&63));out[outlen++]=(char)(0x80|((cp>>6)&63));out[outlen++]=(char)(0x80|(cp&63));}
    }
    out[outlen]=0;
    return is_safe_filename(out)?0:-1;
}

/* Encoded data: 16-byte MD5 of original LZSS compressed stream, then bytes
 * (compressed[i] XOR key[i%16]) - TABLE[i%256], modulo 256. */
static void crypt(uint8_t *data,size_t len,const uint8_t key[16],int decode) {
    /* Scalar table loads: avoid incorrect gather-vectorization on old/new
     * AppleClang for the two independent repeating index periods (16/256). */
    const volatile uint8_t *table = A9_TABLE;
    for(size_t i=0;i<len;i++) {
        const uint8_t salt = table[i&255];
        const uint8_t k = key[i&15];
        const uint8_t value = data[i];
        if(decode)data[i]=(uint8_t)((value+salt)^k);
        else data[i]=(uint8_t)((value^k)-salt);
    }
}

typedef struct {char filename[1024];uint32_t size;size_t payload_offset;uint8_t extra[8];} Entry;
static int read_manifest(const uint8_t *data,size_t length,int type,Entry **output,uint32_t *count) {
    if(length<4)return fail("Truncated archive payload",NULL);
    uint32_t n=get32(data);
    if(n>MAX_FILES)return fail("Implausible file count",NULL);
    Entry *list=calloc(n?n:1,sizeof(Entry));
    if(!list)return fail("Out of memory",NULL);
    size_t index=4;
    for(uint32_t i=0;i<n;i++) {
        if(length-index<NAME_BYTES+(type?8U:0U)+4U){free(list);return fail("Truncated record header",NULL);}
        if(utf16_to_utf8(data+index,list[i].filename)) {free(list);return fail("Unsafe or invalid filename",NULL);}
        index+=NAME_BYTES;
        if(type){memcpy(list[i].extra,data+index,8);index+=8;}
        list[i].size=get32(data+index);index+=4;
        if(length-index<list[i].size){free(list);return fail("Truncated file content",NULL);}
        list[i].payload_offset=index;
        index+=list[i].size;
    }
    if(index!=length) {
        fprintf(stderr,"WARN: Archive has %zu trailing payload bytes\n",length-index);
    }
    *output=list;*count=n;return 0;
}
static int decode_archive(const char *archive,const char *output_dir,int type,int inspect) {
    Bytes source={0};
    if(file_read(archive,&source))return fail("Cannot read archive",archive);
    if(source.len<17){free(source.ptr);return fail("Archive too short",archive);}
    uint8_t key[16];memcpy(key,source.ptr,16);
    size_t compressed_len=source.len-16;
    crypt(source.ptr+16,compressed_len,key,1);
    uint8_t digest[16];a9_md5(source.ptr+16,compressed_len,digest);
    int valid_key=memcmp(key,digest,16)==0;
    if(!valid_key) {
        free(source.ptr);
        return fail("MD5 key mismatch: archive corrupted or unsupported format",archive);
    }
    uint8_t *decompressed=NULL;size_t unpacked_len=0;
    if(a9_lzss_decode(source.ptr+16,compressed_len,&decompressed,&unpacked_len)) {
        free(source.ptr);return fail("LZSS decode failed",archive);
    }
    Entry *list=NULL;uint32_t count=0;
    if(read_manifest(decompressed,unpacked_len,type,&list,&count)) {
        free(decompressed);free(source.ptr);return 1;
    }
    printf("Archive: %s\nFormat: type %d\n",archive,type);
    printf("Archive size: %zu bytes, compressed: %zu bytes, expanded: %zu bytes\n",
           source.len,compressed_len,unpacked_len);
    printf("MD5 key: ");for(unsigned i=0;i<16;i++)printf("%02x",key[i]);
    printf(" (verified)\nFiles: %u\n",count);
    if(!inspect) {
        if(!output_dir){free(list);free(decompressed);free(source.ptr);return fail("No output directory",NULL);}
        if(a9_exists(output_dir)&&!force){free(list);free(decompressed);free(source.ptr);return fail("Output directory exists (choose -o or use --force)",output_dir);}
        char data_dir[PATH_BUF];
        if(mkdir_tree(output_dir)||path_join(data_dir,output_dir,"data")||mkdir_tree(data_dir)) {
            free(list);free(decompressed);free(source.ptr);return fail("Cannot create output directories",output_dir);
        }
        Bytes filenames={0};
        for(uint32_t i=0;i<count;i++) {
            const char *name=list[i].filename;
            if(push(&filenames,name,strlen(name))||push(&filenames,"\n",1)) {
                free(filenames.ptr);free(list);free(decompressed);free(source.ptr);return fail("Out of memory",NULL);
            }
            char full[PATH_BUF];
            if(path_join(full,data_dir,name) || file_write(full,decompressed+list[i].payload_offset,list[i].size,force)) {
                free(filenames.ptr);free(list);free(decompressed);free(source.ptr);return 1;
            }
            if(type) {
                char meta[PATH_BUF];
                if(strlen(full)+strlen(".a9codec_data")+1>=PATH_BUF){free(filenames.ptr);free(list);free(decompressed);free(source.ptr);return fail("Metadata path too long",NULL);}
                snprintf(meta,sizeof meta,"%s.a9codec_data",full);
                if(file_write(meta,list[i].extra,8,force)){
                    free(filenames.ptr);free(list);free(decompressed);free(source.ptr);return 1;
                }
            }
            if(verbose)printf("  [%u] %-40s %u bytes\n",i,name,list[i].size);
        }
        if(emit_file(output_dir,"files.txt",filenames.ptr,filenames.len)){
            free(filenames.ptr);free(list);free(decompressed);free(source.ptr);return 1;
        }
        free(filenames.ptr);
        if(intermediates) {
            if(emit_file(output_dir,"xor.key",key,16) ||
               emit_file(output_dir,"decrypted.data",source.ptr+16,compressed_len) ||
               emit_file(output_dir,"decrypted.extracted",decompressed,unpacked_len)) {
                free(list);free(decompressed);free(source.ptr);return 1;
            }
        }
        printf("Extracted into: %s\n",output_dir);
    } else {
        for(uint32_t i=0;i<count;i++)printf("  [%u] %-40s %u bytes\n",i,list[i].filename,list[i].size);
        printf("Archive is valid.\n");
    }
    free(list);free(decompressed);free(source.ptr);return 0;
}
/* Compare two A9 archive containers entirely in memory. No temporary CSVs. */
typedef struct {
    Bytes source;
    uint8_t *plain;
    size_t plain_size;
    Entry *entries;
    uint32_t count;
} LoadedArchive;
static void archive_unload(LoadedArchive *archive) {
    free(archive->source.ptr);
    free(archive->plain);
    free(archive->entries);
    memset(archive,0,sizeof *archive);
}
static int archive_load(const char *name,int type,LoadedArchive *archive) {
    memset(archive,0,sizeof *archive);
    if(file_read(name,&archive->source))return fail("Cannot open archive",name);
    if(archive->source.len<17){archive_unload(archive);return fail("Archive too short",name);}
    const size_t compressed_size=archive->source.len-16;
    uint8_t md5[16];
    memcpy(md5,archive->source.ptr,16);
    crypt(archive->source.ptr+16,compressed_size,md5,1);
    uint8_t verified[16];a9_md5(archive->source.ptr+16,compressed_size,verified);
    if(memcmp(md5,verified,16)) {
        archive_unload(archive);return fail("Archive MD5 verification failed",name);
    }
    if(a9_lzss_decode(archive->source.ptr+16,compressed_size,&archive->plain,&archive->plain_size)) {
        archive_unload(archive);return fail("Archive LZSS decoding failed",name);
    }
    if(read_manifest(archive->plain,archive->plain_size,type,&archive->entries,&archive->count)) {
        archive_unload(archive);return fail("Cannot parse archive manifest",name);
    }
    return 0;
}
static int csv_name(const char *name) {
    size_t length=strlen(name);
    return length>=4 && name[length-4]=='.' &&
           (name[length-3]=='c'||name[length-3]=='C') &&
           (name[length-2]=='s'||name[length-2]=='S') &&
           (name[length-1]=='v'||name[length-1]=='V');
}
static const Entry *archive_find(const LoadedArchive *archive,const char *name) {
    for(uint32_t i=0;i<archive->count;i++)
        if(strcmp(archive->entries[i].filename,name)==0)return archive->entries+i;
    return NULL;
}
static int compare_archives(const char *oldpath,const char *newpath,int type,FILE *report,int additions_only) {
    LoadedArchive old={0},newer={0};
    if(archive_load(oldpath,type,&old))return 1;
    if(archive_load(newpath,type,&newer)){archive_unload(&old);return 1;}
    A9CompareStats stats={0};
    fprintf(report,"a9tool Archivvergleich\nALT: %s (%u Dateien)\nNEU: %s (%u Dateien)\n",
            oldpath,old.count,newpath,newer.count);
    fprintf(report,"CSV-Zeilennummern beziehen sich auf physische Zeilen (auch bei mehrzeiligen Textfeldern).\n");
    int failed=0;
    for(uint32_t i=0;i<newer.count;i++) {
        const Entry *new_entry=newer.entries+i;
        const Entry *old_entry=archive_find(&old,new_entry->filename);
        const uint8_t *latest=newer.plain+new_entry->payload_offset;
        const uint8_t *previous=old_entry?old.plain+old_entry->payload_offset:NULL;
        if(csv_name(new_entry->filename)) {
            if(a9_compare_csv(report,new_entry->filename,previous,old_entry?old_entry->size:0,
                              latest,new_entry->size,additions_only,&stats)){
                failed=1;break;
            }
        }else if(!old_entry) {
            stats.new_files++;
            fprintf(report,"\nDATEI NEU: %s (%u Bytes, keine CSV)\n",new_entry->filename,new_entry->size);
        }else if(old_entry->size==new_entry->size && memcmp(previous,latest,new_entry->size)==0) {
            stats.identical_files++;
        }else {
            stats.changed_files++;
            if(!additions_only)fprintf(report,"\nDATEI GEÄNDERT: %s (keine CSV)\n",new_entry->filename);
        }
    }
    if(!failed)for(uint32_t i=0;i<old.count;i++) {
        const Entry *old_entry=old.entries+i;
        if(archive_find(&newer,old_entry->filename))continue;
        if(csv_name(old_entry->filename)) {
            if(a9_compare_csv(report,old_entry->filename,old.plain+old_entry->payload_offset,
                              old_entry->size,NULL,0,additions_only,&stats)) {
                failed=1;break;
            }
        }else {
            stats.removed_files++;
            if(!additions_only)fprintf(report,"\nDATEI ENTFERNT: %s (keine CSV)\n",old_entry->filename);
        }
    }
    if(!failed) {
        fprintf(report,"\n=== ZUSAMMENFASSUNG ===\n");
        fprintf(report,"Dateien: %u neu, %u geändert, %u entfernt, %u identisch\n",
                stats.new_files,stats.changed_files,stats.removed_files,stats.identical_files);
        fprintf(report,"CSV-Datensätze: +%llu neu, ~%llu geändert, -%llu entfernt\n",
                (unsigned long long)stats.added_rows,
                (unsigned long long)stats.modified_rows,
                (unsigned long long)stats.removed_rows);
        fprintf(report,"Vergleich erfolgreich.\n");
    }
    archive_unload(&old);archive_unload(&newer);
    return failed;
}
static int encode_archive(const char *directory,const char *output,int type) {
    if(!a9_is_directory(directory))return fail("Input must be a directory",directory);
    if(!skip_check && a9_csv_folder_has_csv(directory) && a9_csv_check_folder(directory))return fail("CSV-Check nicht bestanden; Archiv NICHT erstellt (--no-check zum Umgehen)",directory);
    char files_path[PATH_BUF],data_dir[PATH_BUF];
    if(path_join(files_path,directory,"files.txt")||path_join(data_dir,directory,"data"))return fail("Path too long",directory);
    FILE *manifest=a9_fopen(files_path,"rb");if(!manifest)return fail("Missing files.txt",files_path);
    Bytes body={0};if(push32(&body,0)){fclose(manifest);return fail("Out of memory",NULL);}
    char *line=NULL;size_t capacity=0;ptrdiff_t read_count;
    uint32_t count=0;
    while((read_count=a9_getline(&line,&capacity,manifest))>=0) {
        while(read_count>0 && (line[read_count-1]=='\n'||line[read_count-1]=='\r'))line[--read_count]='\0';
        if(!read_count || line[0]=='#')continue;
        if(!is_safe_filename(line)) {fail("Unsafe filename in files.txt",line);free(body.ptr);free(line);fclose(manifest);return 1;}
        if(count>=MAX_FILES){free(body.ptr);free(line);fclose(manifest);return fail("Too many files",NULL);}
        uint8_t filename[NAME_BYTES];
        if(utf8_to_utf16(line,filename)) {free(body.ptr);free(line);fclose(manifest);return fail("Cannot encode Unicode filename",NULL);}
        char file_path[PATH_BUF];
        if(path_join(file_path,data_dir,line)) {free(body.ptr);free(line);fclose(manifest);return fail("File path too long",NULL);}
        Bytes content={0};
        if(file_read(file_path,&content)){free(body.ptr);free(line);fclose(manifest);return fail("Cannot read input file",file_path);}
        if(content.len>UINT32_MAX){free(content.ptr);free(body.ptr);free(line);fclose(manifest);return fail("File too large",file_path);}
        if(push(&body,filename,NAME_BYTES)) {free(content.ptr);free(body.ptr);free(line);fclose(manifest);return fail("Out of memory",NULL);}
        if(type) {
            char meta[PATH_BUF];
            if(strlen(file_path)+strlen(".a9codec_data")+1>=PATH_BUF){free(content.ptr);free(body.ptr);free(line);fclose(manifest);return fail("Metadata filename too long",NULL);}
            snprintf(meta,sizeof meta,"%s.a9codec_data",file_path);
            Bytes m={0};
            if(file_read(meta,&m)||m.len!=8){free(m.ptr);free(content.ptr);free(body.ptr);free(line);fclose(manifest);return fail("Missing 8-byte .a9codec_data metadata",meta);}
            if(push(&body,m.ptr,8)){free(m.ptr);free(content.ptr);free(body.ptr);free(line);fclose(manifest);return fail("Out of memory",NULL);}
            free(m.ptr);
        }
        if(push32(&body,(uint32_t)content.len)||push(&body,content.ptr,content.len)) {
            free(content.ptr);free(body.ptr);free(line);fclose(manifest);return fail("Out of memory",NULL);
        }
        if(verbose)printf("  [%u] %-40s %zu bytes\n",count,line,content.len);
        free(content.ptr);count++;
    }
    free(line);fclose(manifest);
    set32(body.ptr,count);
    if(count==0){free(body.ptr);return fail("files.txt contains no files",files_path);}
    printf("Packing %u files, %zu uncompressed bytes\n",count,body.len);
    uint8_t *compressed=NULL;size_t length=0;
    if(a9_lzss_encode(body.ptr,body.len,&compressed,&length)) {
        free(body.ptr);return fail("LZSS encoding failed",NULL);
    }
    uint8_t digest[16];a9_md5(compressed,length,digest);
    crypt(compressed,length,digest,0);
    Bytes output_data={0};
    if(push(&output_data,digest,16)||push(&output_data,compressed,length)) {
        free(output_data.ptr);free(compressed);free(body.ptr);return fail("Out of memory",NULL);
    }
    if(file_write(output,output_data.ptr,output_data.len,force)){
        free(output_data.ptr);free(compressed);free(body.ptr);return 1;
    }
    printf("Archive created: %s (%zu bytes)\n",output,output_data.len);
    free(output_data.ptr);free(compressed);free(body.ptr);return 0;
}
static int default_output(const char *input,int extracting,char result[PATH_BUF]) {
    size_t n=strlen(input);
    if(n>=PATH_BUF-8)return -1;
    memcpy(result,input,n+1);
    if(extracting) {
        const char *slash=strrchr(result,'/');
        char *name=slash?(char *)slash+1:result;
        char *dot=strrchr(name,'.');
        if(dot&&strcmp(dot,".bin")==0)*dot=0;
        else if(n+7<PATH_BUF)strcat(result,"_files");
    } else {
        if(n+5>=PATH_BUF)return -1;
        strcat(result,".bin");
    }
    return 0;
}
static int a9tool_main(int argc,char **argv) {
    if(argc<2){usage(argv[0]);return 2;}
    enum {NONE,EXTRACT,CREATE,INSPECT,CHECK,COMPARE} mode=NONE;
    const char *input=NULL,*comparison_new=NULL,*custom_output=NULL;
    int type=0,only_added=0;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"compare") || !strcmp(argv[i],"--compare")) {
            if(mode!=NONE)return fail("Multiple commands specified",NULL);
            mode=COMPARE;continue;
        }
        if(!strcmp(argv[i],"--only-added")){only_added=1;continue;}
        if(!strcmp(argv[i],"check") || !strcmp(argv[i],"--check")) {
            if(mode!=NONE)return fail("Multiple commands specified",NULL);
            mode=CHECK;continue;
        }
        if(!strcmp(argv[i],"--no-check")){skip_check=1;continue;}
        if(!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")){usage(argv[0]);return 0;}
        if(!strcmp(argv[i],"-v")){verbose=1;continue;}
        if(!strcmp(argv[i],"-vv")){verbose=2;continue;}
        if(!strcmp(argv[i],"--force")||!strcmp(argv[i],"--forcedir")){force=1;continue;}
        if(!strcmp(argv[i],"--intermediate")){intermediates=1;continue;}
        if(!strcmp(argv[i],"-x")||!strcmp(argv[i],"-c")||!strcmp(argv[i],"-i")) {
            if(mode!=NONE)return fail("Specify only one of -x, -c, -i",NULL);
            mode=argv[i][1]=='x'?EXTRACT:(argv[i][1]=='c'?CREATE:INSPECT);
            if(i+1<argc && argv[i+1][0]!='-')input=argv[++i];
            continue;
        }
        if(!strcmp(argv[i],"-o")) {
            if(i+1>=argc)return fail("Missing -o path",NULL);
            custom_output=argv[++i];continue;
        }
        if(!strcmp(argv[i],"-t")) {
            if(i+1>=argc || (strcmp(argv[i+1],"0")&&strcmp(argv[i+1],"1")))return fail("-t expects 0 or 1",NULL);
            type=atoi(argv[++i]);continue;
        }
        if(argv[i][0]=='-')return fail("Unknown option",argv[i]);
        if(input) {
            if(mode==COMPARE && !comparison_new)comparison_new=argv[i];
            else return fail("Unexpected extra argument",argv[i]);
        }else input=argv[i];
    }
    if(mode==NONE||!input){usage(argv[0]);return 2;}
    char outpath[PATH_BUF];
    if(mode==INSPECT)return decode_archive(input,NULL,type,1);
    if(mode==CHECK)return a9_csv_check_folder(input);
    if(mode==COMPARE) {
        if(!comparison_new)return fail("Usage: a9tool compare ALT.bin NEU.bin",NULL);
        FILE *report=stdout;
        if(custom_output) {
            if(!strcmp(custom_output,input)||!strcmp(custom_output,comparison_new))
                return fail("Report must not overwrite an input archive",custom_output);
            if(a9_exists(custom_output)&&!force)return fail("Report already exists (use --force)",custom_output);
            report=a9_fopen(custom_output,"wb");
            if(!report)return fail("Cannot open report output",custom_output);
        }
        int result=compare_archives(input,comparison_new,type,report,only_added);
        if(custom_output) {
            if(fclose(report))return fail("Could not save comparison report",custom_output);
            if(!result)printf("Vergleichsbericht geschrieben: %s\n",custom_output);
        }
        return result;
    }
    if(custom_output) {
        if(strlen(custom_output)>=PATH_BUF)return fail("Output path too long",NULL);
        strcpy(outpath,custom_output);
    } else if(default_output(input,mode==EXTRACT,outpath))return fail("Output path too long",NULL);
    return mode==EXTRACT?decode_archive(input,outpath,type,0):encode_archive(input,outpath,type);
}

#ifdef _WIN32
/* Real UTF-16 Windows command line -> UTF-8, so German/Asian filenames work
 * without forcing the user to rename files or change the system ANSI page. */
int wmain(int argc,wchar_t **wide_args) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    char **args=calloc((size_t)argc+1,sizeof(char *));
    if(!args)return 1;
    int result=1;
    for(int i=0;i<argc;i++) {
        int n=WideCharToMultiByte(CP_UTF8,0,wide_args[i],-1,NULL,0,NULL,NULL);
        if(!n)goto done;
        args[i]=malloc((size_t)n);
        if(!args[i])goto done;
        if(!WideCharToMultiByte(CP_UTF8,0,wide_args[i],-1,args[i],n,NULL,NULL))goto done;
    }
    result=a9tool_main(argc,args);
done:
    for(int i=0;i<argc;i++)free(args[i]);
    free(args);return result;
}
#else
int main(int argc,char **argv) { return a9tool_main(argc,argv); }
#endif
