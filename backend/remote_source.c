/* Read-only SMB/FTP adapter. FTP paths are URL-escaped and never interpreted
 * as commands. The caller retains destination validation and copy verification. */
#define _POSIX_C_SOURCE 200809L
#include "remote_source.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <errno.h>
struct RemoteSource {struct smb2_context *smb; CURL *curl; char base[320],error[CURL_ERROR_SIZE]; bool (*cancel)(void);};
struct RemoteFile {struct smb2fh *smb; char *path;};
struct RemoteDir {struct smb2dir *smb; struct smb2dirent *entries; size_t count,index;};
typedef struct {unsigned char *data;size_t used,capacity;bool grow;} Buffer;
static size_t receive(char *data,size_t size,size_t count,void *opaque){
    Buffer *b=opaque;if(size && count>SIZE_MAX/size)return 0;size_t n=size*count;
    if(n>b->capacity-b->used){
        if(!b->grow || n>16*1024*1024-b->used)return 0;
        size_t cap=b->capacity?b->capacity*2:65536;if(cap<b->used+n)cap=b->used+n;
        if(cap>16*1024*1024)cap=16*1024*1024;
        void *p=realloc(b->data,cap+1);if(!p)return 0;b->data=p;b->capacity=cap;
    }
    memcpy(b->data+b->used,data,n);b->used+=n;return n;
}
static int progress(void *opaque,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d){
    (void)a;(void)b;(void)c;(void)d;RemoteSource *s=opaque;return s->cancel&&s->cancel();
}
static size_t discard(char *data,size_t size,size_t count,void *opaque){(void)data;(void)opaque;return size*count;}
static char *url(RemoteSource *s,const char *path,bool directory){
    size_t cap=strlen(s->base)+strlen(path)*3+8;char *out=malloc(cap);if(!out)return NULL;
    strcpy(out,s->base);size_t pos=strlen(out);
    const unsigned char *p=(const unsigned char *)path;
    /* The double slash selects an absolute FTP server path. */
    for(;*p;p++){
        if(*p=='/' || (*p>='a'&&*p<='z') || (*p>='A'&&*p<='Z') || (*p>='0'&&*p<='9') || strchr("-_.~",*p))out[pos++]=(char)*p;
        else {snprintf(out+pos,4,"%%%02X",*p);pos+=3;}
    }
    if(directory && out[pos-1]!='/')out[pos++]='/';out[pos]=0;return out;
}
static int request(RemoteSource *s,const char *path,bool directory,Buffer *buffer,const char *range,bool info){
    char *address=url(s,path,directory);if(!address)return -1;s->error[0]=0;
    curl_easy_setopt(s->curl,CURLOPT_URL,address);
    curl_easy_setopt(s->curl,CURLOPT_CUSTOMREQUEST,directory?"MLSD":NULL);
    curl_easy_setopt(s->curl,CURLOPT_NOBODY,info?1L:0L);
    curl_easy_setopt(s->curl,CURLOPT_FILETIME,info?1L:0L);
    curl_easy_setopt(s->curl,CURLOPT_RANGE,range);
    curl_easy_setopt(s->curl,CURLOPT_WRITEFUNCTION,info?discard:receive);
    curl_easy_setopt(s->curl,CURLOPT_WRITEDATA,buffer);
    CURLcode rc=curl_easy_perform(s->curl);free(address);
    if(rc && !*s->error)snprintf(s->error,sizeof s->error,"FTP: %s",curl_easy_strerror(rc));
    return rc? -1:0;
}
RemoteSource *remote_smb(struct smb2_context *s){RemoteSource *r=calloc(1,sizeof *r);if(r)r->smb=s;else smb2_destroy_context(s);return r;}
RemoteSource *remote_ftp(const char *host,unsigned port,const char *user,const char *password,bool (*cancel)(void)){
    RemoteSource *s=calloc(1,sizeof *s);if(!s)return NULL;s->curl=curl_easy_init();if(!s->curl){free(s);return NULL;}s->cancel=cancel;
    snprintf(s->base,sizeof s->base,"ftp://%s:%u//",host,port?port:21);
    curl_easy_setopt(s->curl,CURLOPT_USERNAME,*user?user:"anonymous");
    curl_easy_setopt(s->curl,CURLOPT_PASSWORD,*user?password:(*password?password:"atmosphere@"));
    curl_easy_setopt(s->curl,CURLOPT_ERRORBUFFER,s->error);
    curl_easy_setopt(s->curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(s->curl,CURLOPT_CONNECTTIMEOUT,15L);
    curl_easy_setopt(s->curl,CURLOPT_FTP_RESPONSE_TIMEOUT,20L);
    curl_easy_setopt(s->curl,CURLOPT_LOW_SPEED_LIMIT,1L);
    curl_easy_setopt(s->curl,CURLOPT_LOW_SPEED_TIME,20L);
    curl_easy_setopt(s->curl,CURLOPT_FTP_SKIP_PASV_IP,1L);
    curl_easy_setopt(s->curl,CURLOPT_PROXY,"");
    curl_easy_setopt(s->curl,CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(s->curl,CURLOPT_XFERINFOFUNCTION,progress);
    curl_easy_setopt(s->curl,CURLOPT_XFERINFODATA,s);
    return s;
}
struct smb2_context *remote_smb_context(RemoteSource *s){return s->smb;}
void remote_forget_smb(RemoteSource *s){s->smb=NULL;}
struct smb2fh *remote_smb_file(RemoteFile *f){return f->smb;}
void remote_destroy(RemoteSource *s){if(!s)return;if(s->smb)smb2_destroy_context(s->smb);if(s->curl)curl_easy_cleanup(s->curl);free(s);}
const char *remote_error(RemoteSource *s){return s->smb?smb2_get_error(s->smb):s->error;}
/* UTC conversion independent of the console's locale/time zone. */
static uint64_t timestamp(const char *v){
    if(strlen(v)<14)return 0;int y,m,d,h,n,sec;
    if(sscanf(v,"%4d%2d%2d%2d%2d%2d",&y,&m,&d,&h,&n,&sec)!=6 || y<1970||m<1||m>12||d<1||d>31||h>23||n>59||sec>60)return 0;
    y-=m<=2;int era=y/400;unsigned yo=(unsigned)(y-era*400),mp=(unsigned)(m+(m>2?-3:9));
    long long days=(long long)era*146097+yo*365+yo/4-yo/100+(153*mp+2)/5+d-1-719468;
    return days<0?0:(uint64_t)days*86400+h*3600+n*60+sec;
}
RemoteDir *remote_opendir(RemoteSource *s,const char *path){
    RemoteDir *d=calloc(1,sizeof *d);if(!d)return NULL;
    if(s->smb){d->smb=smb2_opendir(s->smb,path);if(!d->smb){free(d);return NULL;}return d;}
    Buffer b={.grow=true};if(request(s,path,true,&b,NULL,false)){free(b.data);free(d);return NULL;}
    if(!b.data)return d;b.data[b.used]=0;char *save,*line=strtok_r((char *)b.data,"\n",&save);
    for(;line;line=strtok_r(NULL,"\n",&save)){
        size_t len=strlen(line);if(len&&line[len-1]=='\r')line[--len]=0;if(!len)continue;
        char *name=strchr(line,' ');if(!name){snprintf(s->error,sizeof s->error,"FTP server must support MLSD directory listings");goto fail;}*name++=0;
        if(!*name||!strcmp(name,".")||!strcmp(name,"..")||strchr(name,'/')||strchr(name,'\\'))continue;
        struct smb2_stat_64 st={0};bool type=false,skip=false;char *facts,*fact=strtok_r(line,";",&facts);
        for(;fact;fact=strtok_r(NULL,";",&facts)){
            if(!strncasecmp(fact,"type=",5)) {type=true;if(!strcasecmp(fact+5,"file"))st.smb2_type=SMB2_TYPE_FILE;else if(!strcasecmp(fact+5,"dir"))st.smb2_type=SMB2_TYPE_DIRECTORY;else skip=true;}
            else if(!strncasecmp(fact,"size=",5)){char *end;errno=0;unsigned long long size=strtoull(fact+5,&end,10);if(errno||*end||fact[5]=='-'||size>INT64_MAX)goto fail;st.smb2_size=size;}
            else if(!strncasecmp(fact,"modify=",7))st.smb2_mtime=timestamp(fact+7);
        }
        if(skip)continue;if(!type)goto fail;if(d->count>=100000)goto fail;
        void *entries=realloc(d->entries,(d->count+1)*sizeof *d->entries);if(!entries)goto fail;d->entries=entries;
        d->entries[d->count].name=strdup(name);if(!d->entries[d->count].name)goto fail;
        d->entries[d->count++].st=st;
    }
    free(b.data);return d;
fail:
    if(!*s->error)snprintf(s->error,sizeof s->error,"Invalid or oversized FTP directory listing");free(b.data);remote_closedir(s,d);return NULL;
}
struct smb2dirent *remote_readdir(RemoteSource *s,RemoteDir *d){return s->smb?smb2_readdir(s->smb,d->smb):(d->index<d->count?&d->entries[d->index++]:NULL);}
void remote_closedir(RemoteSource *s,RemoteDir *d){if(s->smb)smb2_closedir(s->smb,d->smb);for(size_t i=0;i<d->count;i++)free((void *)d->entries[i].name);free(d->entries);free(d);}
int remote_stat(RemoteSource *s,const char *path,struct smb2_stat_64 *st){
    if(s->smb)return smb2_stat(s->smb,path,st);
    if(!*path){memset(st,0,sizeof *st);st->smb2_type=SMB2_TYPE_DIRECTORY;return 0;}
    char *parent=strdup(path);if(!parent)return -1;char *slash=strrchr(parent,'/');const char *name=slash?slash+1:path;if(slash)*slash=0;else *parent=0;
    RemoteDir *d=remote_opendir(s,parent);int rc=-1;if(d){struct smb2dirent *e;while((e=remote_readdir(s,d)))if(!strcmp(name,e->name)){*st=e->st;rc=0;break;}remote_closedir(s,d);}free(parent);return rc;
}
RemoteFile *remote_open(RemoteSource *s,const char *path,int flags){
    RemoteFile *f=calloc(1,sizeof *f);if(!f)return NULL;
    if(s->smb){f->smb=smb2_open(s->smb,path,flags);if(!f->smb){free(f);return NULL;}}
    else {f->path=strdup(path);if(!f->path){free(f);return NULL;}}
    return f;
}
int remote_fstat(RemoteSource *s,RemoteFile *f,struct smb2_stat_64 *st){
    if(s->smb)return smb2_fstat(s->smb,f->smb,st);
    Buffer b={0};if(request(s,f->path,false,&b,NULL,true))return -1;
    curl_off_t size=-1,time=-1;curl_easy_getinfo(s->curl,CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,&size);curl_easy_getinfo(s->curl,CURLINFO_FILETIME_T,&time);
    if(size<0)return -1;memset(st,0,sizeof *st);st->smb2_type=SMB2_TYPE_FILE;st->smb2_size=(uint64_t)size;st->smb2_mtime=time<0?0:(uint64_t)time;return 0;
}
int remote_close(RemoteSource *s,RemoteFile *f){int rc=s->smb?smb2_close(s->smb,f->smb):0;free(f->path);free(f);return rc;}
int remote_pread(RemoteSource *s,RemoteFile *f,void *data,uint32_t length,uint64_t offset){
    if(s->smb)return smb2_pread(s->smb,f->smb,data,length,offset);if(!length)return 0;
    if(offset>INT64_MAX-length)return -1;char range[64];snprintf(range,sizeof range,"%llu-%llu",(unsigned long long)offset,(unsigned long long)(offset+length-1));
    Buffer b={.data=data,.capacity=length};if(request(s,f->path,false,&b,range,false))return -1;return (int)b.used;
}
uint32_t remote_max_read(RemoteSource *s){return s->smb?smb2_get_max_read_size(s->smb):1024*1024;}
