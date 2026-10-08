/* Bounded, read-only DAV multistatus parser. Hrefs never select a different host. */
#define _POSIX_C_SOURCE 200809L
#include "webdav_listing.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "../vendor/yxml/yxml.c"
#pragma GCC diagnostic pop
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>

static const char *local(const char *s){const char *p=strchr(s,':');return p?p+1:s;}
static int append(char *out,size_t cap,const char *text){size_t a=strlen(out),b=strlen(text);if(a+b>=cap)return -1;memcpy(out+a,text,b+1);return 0;}
static char *same_origin_path(const char *href,const char *base){
    CURLU *a=curl_url(),*b=curl_url();char *path=NULL;
    if(!a||!b||curl_url_set(a,CURLUPART_URL,href,0)||curl_url_set(b,CURLUPART_URL,base,0))goto end;
    CURLUPart fields[]={CURLUPART_SCHEME,CURLUPART_HOST,CURLUPART_PORT};
    for(size_t i=0;i<3;i++){char *x=NULL,*y=NULL;int bad=curl_url_get(a,fields[i],&x,CURLU_DEFAULT_PORT)||curl_url_get(b,fields[i],&y,CURLU_DEFAULT_PORT);if(!bad)bad=strcasecmp(x,y);curl_free(x);curl_free(y);if(bad)goto end;}
    CURLUPart excluded[]={CURLUPART_USER,CURLUPART_PASSWORD,CURLUPART_QUERY,CURLUPART_FRAGMENT};
    for(size_t i=0;i<4;i++){char *x=NULL;int found=!curl_url_get(a,excluded[i],&x,0);curl_free(x);if(found)goto end;}
    curl_url_get(a,CURLUPART_PATH,&path,0);
end:curl_url_cleanup(a);curl_url_cleanup(b);return path;
}
static int add_entry(const char *href,const char *base,const char *path,bool collection,bool sized,uint64_t size,uint64_t modified,struct smb2dirent **entries,size_t *count){
    const char *p=href;char encoded[8192];
    if(strstr(p,"://")){
        char *absolute=same_origin_path(p,base);if(!absolute)return -1;
        size_t n=strlen(absolute);if(n>=sizeof encoded){curl_free(absolute);return -1;}memcpy(encoded,absolute,n+1);curl_free(absolute);p=encoded;
    }
    if(*p!='/'){if(snprintf(encoded,sizeof encoded,"/%s%s%s",path,*path?"/":"",p)>=(int)sizeof encoded)return -1;p=encoded;}
    if(strchr(p,'?')||strchr(p,'#'))return -1;
    int length=0;char *decoded=curl_easy_unescape(NULL,p,0,&length);if(!decoded)return -1;
    if((size_t)length!=strlen(decoded)||strchr(decoded,'\\')){curl_free(decoded);return -1;}
    for(int i=0;i<length;i++)if((unsigned char)decoded[i]<32){curl_free(decoded);return -1;}
    while(length>1&&decoded[length-1]=='/')decoded[--length]=0;
    char parent[4096];if(snprintf(parent,sizeof parent,"/%s",path)>=(int)sizeof parent){curl_free(decoded);return -1;}
    size_t n=strlen(parent);while(n>1&&parent[n-1]=='/')parent[--n]=0;
    if(!strcmp(decoded,parent)){curl_free(decoded);return 0;}
    if(n>1)parent[n++]='/';parent[n]=0;
    if(strncmp(decoded,parent,n)){curl_free(decoded);return -1;}
    const char *name=decoded+n;
    if(!*name||!strcmp(name,".")||!strcmp(name,"..")||strchr(name,'/')||strlen(name)>255){curl_free(decoded);return -1;}
    if(!collection&&!sized){curl_free(decoded);return -1;}
    for(size_t i=0;i<*count;i++)if(!strcmp((*entries)[i].name,name)){curl_free(decoded);return -1;}
    if(*count>=100000){curl_free(decoded);return -1;}
    struct smb2dirent *next=realloc(*entries,(*count+1)*sizeof **entries);if(!next){curl_free(decoded);return -1;}*entries=next;
    struct smb2dirent *e=&next[*count];memset(e,0,sizeof *e);e->name=strdup(name);curl_free(decoded);if(!e->name)return -1;
    e->st.smb2_type=collection?SMB2_TYPE_DIRECTORY:SMB2_TYPE_FILE;e->st.smb2_size=size;e->st.smb2_mtime=modified;(*count)++;return 0;
}
int webdav_listing(const char *xml,size_t length,const char *base,const char *path,struct smb2dirent **entries,size_t *count){
    if(!length||memchr(xml,0,length)||strstr(xml,"<!DOCTYPE")||strstr(xml,"<!ENTITY"))return -1;
    yxml_t x;char stack[8192];yxml_init(&x,stack,sizeof stack);
    char names[16][64],href[8192]="",status[96]="",size_text[32]="",date[128]="";
    int depth=0;bool root=false,in_response=false,in_props=false,collection=false,sized=false,good=false,pc=false;
    uint64_t size=0,modified=0;
    for(size_t i=0;i<length;i++){
        yxml_ret_t r=yxml_parse(&x,(unsigned char)xml[i]);if(r<0)return -1;
        if(r==YXML_ELEMSTART){
            if(depth>=16||strlen(local(x.elem))>=64)return -1;strcpy(names[depth++],local(x.elem));
            const char *n=names[depth-1];
            if(depth==1){if(strcmp(n,"multistatus"))return -1;root=true;}
            if(depth==2&&!strcmp(n,"response")){in_response=true;href[0]=0;collection=sized=good=false;size=modified=0;}
            if(in_response&&depth==3&&!strcmp(n,"propstat")){in_props=true;pc=false;status[0]=size_text[0]=date[0]=0;}
            if(in_props&&depth==6&&!strcmp(n,"collection")&&!strcmp(names[4],"resourcetype"))pc=true;
        } else if(r==YXML_CONTENT&&depth){
            const char *n=names[depth-1];
            if(in_response&&depth==3&&!strcmp(n,"href")){if(append(href,sizeof href,x.data))return -1;}
            if(in_props&&depth==4&&!strcmp(n,"status")){if(append(status,sizeof status,x.data))return -1;}
            if(in_props&&depth==5&&!strcmp(names[3],"prop")){
                if(!strcmp(n,"getcontentlength")&&append(size_text,sizeof size_text,x.data))return -1;
                if(!strcmp(n,"getlastmodified")&&append(date,sizeof date,x.data))return -1;
            }
        } else if(r==YXML_ELEMEND){
            if(!depth)return -1;
            if(in_props&&depth==3){
                int code=0;if(sscanf(status,"HTTP/%*s %d",&code)!=1)return -1;
                if(code==200){good=true;collection|=pc;
                    if(*size_text){char *end;errno=0;unsigned long long value=strtoull(size_text,&end,10);if(errno||*end||*size_text=='-'||value>INT64_MAX)return -1;size=value;sized=true;}
                    if(*date){time_t t=curl_getdate(date,NULL);if(t>=0)modified=(uint64_t)t;}
                }in_props=false;
            }
            if(in_response&&depth==2){if(good&&(!*href||add_entry(href,base,path,collection,sized,size,modified,entries,count)))return -1;in_response=false;}
            depth--;
        }
    }
    return !root||depth||yxml_eof(&x)<0?-1:0;
}
