/* Reconstruction of the 2013 a9codec compression routines from the PE symbols
 * _compress_buffer, _insert_node, _delete_node and _a9_bufferextract.
 * LZSS based on the 4K-window, 18-byte lookahead Okumura scheme.
 */
#include "a9_lzss.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define N 4096
#define F 18
#define THRESHOLD 2
#define NIL N
#define OUTPUT_LIMIT ((size_t)1<<30)

typedef struct {uint8_t *data;size_t size,capacity;} Buffer;
static int append(Buffer *b,const uint8_t *data,size_t n) {
    if(n>OUTPUT_LIMIT-b->size)return -1;
    if(n>b->capacity-b->size) {
        size_t newcap=b->capacity?b->capacity:256;
        while(newcap<b->size+n) {
            if(newcap>OUTPUT_LIMIT/2){newcap=OUTPUT_LIMIT;break;}
            newcap*=2;
        }
        void *p=realloc(b->data,newcap);
        if(!p)return -1;
        b->data=p;b->capacity=newcap;
    }
    memcpy(b->data+b->size,data,n);b->size+=n;return 0;
}
int a9_lzss_decode(const uint8_t *in,size_t input_size,uint8_t **out,size_t *output_size) {
    if(!in || !out || !output_size)return -1;
    uint8_t ring[N]={0};int r=N-F;uint32_t flags=0;
    Buffer b={0};size_t index=0;
    while(index<input_size) {
        flags>>=1;
        if(!(flags&0x100)) { flags=(uint32_t)in[index++]|0xFF00; }
        if(flags&1) {
            if(index>=input_size)goto failed;
            uint8_t byte=in[index++];
            if(append(&b,&byte,1))goto failed;
            ring[r]=byte;r=(r+1)&(N-1);
        } else {
            if(input_size-index<2)goto failed;
            unsigned a=in[index++],t=in[index++];
            unsigned position=a|((t&0xF0)<<4);
            unsigned count=(t&0x0F)+THRESHOLD+1;
            for(unsigned j=0;j<count;j++) {
                uint8_t byte=ring[(position+j)&(N-1)];
                if(append(&b,&byte,1))goto failed;
                ring[r]=byte;r=(r+1)&(N-1);
            }
        }
    }
    *out=b.data;*output_size=b.size;return 0;
failed:
    free(b.data);return -1;
}

typedef struct {
    uint8_t text[N+F-1];
    int left[N+1],right[N+257],parent[N+1];
    int match_position,match_length;
} Tree;
static void init_tree(Tree *t) {
    for(int i=N+1;i<=N+256;i++)t->right[i]=NIL;
    for(int i=0;i<N;i++)t->parent[i]=NIL;
}
static void insert_node(Tree *t,int r) {
    int cmp=1,p=N+1+t->text[r];
    t->left[r]=t->right[r]=NIL;t->match_length=0;
    for(;;) {
        if(cmp>=0) {
            if(t->right[p]!=NIL)p=t->right[p];
            else { t->right[p]=r;t->parent[r]=p;return; }
        } else {
            if(t->left[p]!=NIL)p=t->left[p];
            else {t->left[p]=r;t->parent[r]=p;return;}
        }
        int i=1;
        for(;i<F;i++) {
            cmp=(int)t->text[r+i]-(int)t->text[p+i];
            if(cmp!=0)break;
        }
        if(i>t->match_length) {
            t->match_position=p;t->match_length=i;
            if(i>=F)break;
        }
    }
    t->parent[r]=t->parent[p];
    t->left[r]=t->left[p];t->right[r]=t->right[p];
    t->parent[t->left[p]]=r;t->parent[t->right[p]]=r;
    if(t->right[t->parent[p]]==p)t->right[t->parent[p]]=r;
    else t->left[t->parent[p]]=r;
    t->parent[p]=NIL;
}
static void delete_node(Tree *t,int p) {
    if(t->parent[p]==NIL)return;
    int q;
    if(t->right[p]==NIL)q=t->left[p];
    else if(t->left[p]==NIL)q=t->right[p];
    else {
        q=t->left[p];
        if(t->right[q]!=NIL) {
            do {q=t->right[q];}while(t->right[q]!=NIL);
            t->right[t->parent[q]]=t->left[q];
            t->parent[t->left[q]]=t->parent[q];
            t->left[q]=t->left[p];t->parent[t->left[p]]=q;
        }
        t->right[q]=t->right[p];t->parent[t->right[p]]=q;
    }
    t->parent[q]=t->parent[p];
    if(t->right[t->parent[p]]==p)t->right[t->parent[p]]=q;
    else t->left[t->parent[p]]=q;
    t->parent[p]=NIL;
}

int a9_lzss_encode(const uint8_t *in,size_t input_size,uint8_t **out,size_t *output_size) {
    if(!in || !out || !output_size)return -1;
    Tree *t=calloc(1,sizeof(*t));if(!t)return -1;
    init_tree(t);
    Buffer b={0};
    uint8_t code[1+8*2];code[0]=0;
    int codeptr=1;uint8_t mask=1;
    int s=0,r=N-F,len=0;size_t pos=0;
    memset(t->text,0,(size_t)r);
    while(len<F && pos<input_size)t->text[r+len++]=in[pos++];
    if(!len)goto done;
    for(int i=1;i<=F;i++)insert_node(t,r-i);
    insert_node(t,r);
    do {
        if(t->match_length>len)t->match_length=len;
        if(t->match_length<=THRESHOLD) {
            t->match_length=1;
            code[0]|=mask;
            code[codeptr++]=t->text[r];
        } else {
            code[codeptr++]=(uint8_t)t->match_position;
            code[codeptr++]=(uint8_t)(((t->match_position>>4)&0xf0) | (t->match_length-(THRESHOLD+1)));
        }
        mask=(uint8_t)(mask<<1);
        if(!mask) {
            if(append(&b,code,(size_t)codeptr))goto failed;
            code[0]=0;codeptr=1;mask=1;
        }
        int last_match=t->match_length;
        int i=0;
        for(;i<last_match && pos<input_size;i++) {
            uint8_t c=in[pos++];
            delete_node(t,s);
            t->text[s]=c;
            if(s<F-1)t->text[s+N]=c;
            s=(s+1)&(N-1);r=(r+1)&(N-1);
            insert_node(t,r);
        }
        while(i++<last_match) {
            delete_node(t,s);s=(s+1)&(N-1);r=(r+1)&(N-1);
            if(--len)insert_node(t,r);
        }
    }while(len>0);
    if(codeptr>1 && append(&b,code,(size_t)codeptr))goto failed;
done:
    free(t);*out=b.data;*output_size=b.size;return 0;
failed:
    free(t);free(b.data);return -1;
}
