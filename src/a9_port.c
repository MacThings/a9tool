#define _POSIX_C_SOURCE 200809L
#include "a9_port.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <wchar.h>

static wchar_t *to_wide(const char *utf8) {
    if(!utf8)return NULL;
    int chars=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8,-1,NULL,0);
    if(!chars)return NULL;
    wchar_t *result=malloc((size_t)chars*sizeof(wchar_t));
    if(!result)return NULL;
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8,-1,result,chars)) {
        free(result);return NULL;
    }
    return result;
}
FILE *a9_fopen(const char *path,const char *mode) {
    wchar_t *wide=to_wide(path);
    if(!wide){errno=EINVAL;return NULL;}
    wchar_t wmode[16]={0};
    size_t n=strlen(mode);
    if(n>=16){free(wide);errno=EINVAL;return NULL;}
    for(size_t i=0;i<n;i++)wmode[i]=(wchar_t)(unsigned char)mode[i];
    FILE *file=_wfopen(wide,wmode);
    free(wide);return file;
}
int a9_exists(const char *path) {
    wchar_t *wide=to_wide(path);
    if(!wide)return 0;
    DWORD attrs=GetFileAttributesW(wide);
    free(wide);return attrs!=INVALID_FILE_ATTRIBUTES;
}
int a9_is_directory(const char *path) {
    wchar_t *wide=to_wide(path);
    if(!wide)return 0;
    DWORD attrs=GetFileAttributesW(wide);
    free(wide);
    return attrs!=INVALID_FILE_ATTRIBUTES && (attrs&FILE_ATTRIBUTE_DIRECTORY)!=0;
}
int a9_mkdir(const char *path) {
    wchar_t *wide=to_wide(path);
    if(!wide){errno=EINVAL;return -1;}
    int result=_wmkdir(wide);
    free(wide);return result;
}
struct A9Dir {
    HANDLE find;
    WIN32_FIND_DATAW current;
    A9Dirent item;
    int pending;
};
A9Dir *a9_dir_open(const char *path) {
    wchar_t *wide=to_wide(path);if(!wide)return NULL;
    size_t n=wcslen(wide);
    wchar_t *pattern=malloc((n+3)*sizeof(wchar_t));
    if(!pattern){free(wide);return NULL;}
    wcscpy(pattern,wide);
    if(n && wide[n-1]!=L'\\' && wide[n-1]!=L'/')pattern[n++]=L'\\';
    pattern[n++]=L'*';pattern[n]=0;
    A9Dir *d=calloc(1,sizeof *d);
    if(d) {
        d->find=FindFirstFileW(pattern,&d->current);
        d->pending=1;
        if(d->find==INVALID_HANDLE_VALUE){free(d);d=NULL;}
    }
    free(pattern);free(wide);return d;
}
const A9Dirent *a9_dir_read(A9Dir *d) {
    if(!d)return NULL;
    WIN32_FIND_DATAW *entry=&d->current;
    for(;;) {
        if(d->pending)d->pending=0;
        else if(!FindNextFileW(d->find,entry))return NULL;
        int len=WideCharToMultiByte(CP_UTF8,0,entry->cFileName,-1,d->item.name,(int)sizeof d->item.name,NULL,NULL);
        if(len)return &d->item;
    }
}
void a9_dir_close(A9Dir *dir) {
    if(dir){FindClose(dir->find);free(dir);}
}
#else
#include <dirent.h>
#include <sys/stat.h>
struct A9Dir { DIR *dir; A9Dirent item; };
FILE *a9_fopen(const char *path,const char *mode) {return fopen(path,mode);}
int a9_exists(const char *path) {struct stat st;return stat(path,&st)==0;}
int a9_is_directory(const char *path) {struct stat st;return !stat(path,&st)&&S_ISDIR(st.st_mode);}
int a9_mkdir(const char *path) {return mkdir(path,0755);}
A9Dir *a9_dir_open(const char *path) {
    DIR *sys=opendir(path);if(!sys)return NULL;
    A9Dir *dir=calloc(1,sizeof *dir);
    if(!dir){closedir(sys);return NULL;}
    dir->dir=sys;return dir;
}
const A9Dirent *a9_dir_read(A9Dir *dir) {
    if(!dir)return NULL;
    struct dirent *entry;
    while((entry=readdir(dir->dir))!=NULL) {
        size_t len=strlen(entry->d_name);
        if(len>=sizeof dir->item.name)continue;
        memcpy(dir->item.name,entry->d_name,len+1);
        return &dir->item;
    }
    return NULL;
}
void a9_dir_close(A9Dir *dir) {
    if(dir){closedir(dir->dir);free(dir);}
}
#endif

/* getline replacement for MSVC and regular C11. Includes trailing LF when present. */
ptrdiff_t a9_getline(char **line,size_t *cap,FILE *stream) {
    if(!line||!cap||!stream)return -1;
    if(!*line || *cap==0) {
        size_t n=256;
        char *p=realloc(*line,n);
        if(!p)return -1;
        *line=p;*cap=n;
    }
    size_t used=0;
    int ch;
    while((ch=fgetc(stream))!=EOF) {
        if(used>=8u*1024u*1024u){errno=EOVERFLOW;return -1;}
        if(used+2>*cap) {
            size_t next=*cap*2;
            char *p=realloc(*line,next);
            if(!p)return -1;
            *line=p;*cap=next;
        }
        (*line)[used++]=(char)ch;
        if(ch=='\n')break;
    }
    if(used==0)return -1;
    (*line)[used]='\0';return (ptrdiff_t)used;
}
char *a9_strdup(const char *src) {
    size_t n=strlen(src)+1;
    char *out=malloc(n);
    if(out)memcpy(out,src,n);
    return out;
}
