/* SMB/FTP/WebDAV reads and SMB/FTP backup writes. URL paths are escaped; FTP
 * control paths reject control characters. Callers validate copy destinations. */
#define _POSIX_C_SOURCE 200809L
#include "remote_source.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include "webdav_listing.h"
#include "ca.h"
struct RemoteSource {struct smb2_context *smb; CURL *curl; char base[320],error[CURL_ERROR_SIZE]; bool (*cancel)(void); bool dav,tls; unsigned long long range_start,range_end,range_total; bool range_valid;};
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
static size_t header(char *data,size_t size,size_t count,void *opaque){
    RemoteSource *s=opaque;size_t n=size*count;
    if(n>=5&&!strncasecmp(data,"HTTP/",5))s->range_valid=false;
    if(n>14&&!strncasecmp(data,"Content-Range:",14)){
        char value[160];if(n>=sizeof value)return 0;memcpy(value,data,n);value[n]=0;
        s->range_valid=sscanf(value+14," bytes %llu-%llu/%llu",&s->range_start,&s->range_end,&s->range_total)==3;
    }return n;
}
static int request(RemoteSource *s,const char *path,bool directory,Buffer *buffer,const char *range,bool info){
    char *address=url(s,path,directory);if(!address)return -1;s->error[0]=0;
    curl_easy_setopt(s->curl,CURLOPT_URL,address);
    curl_easy_setopt(s->curl,CURLOPT_UPLOAD,0L);
    curl_easy_setopt(s->curl,CURLOPT_CUSTOMREQUEST,directory?(s->dav?"PROPFIND":"MLSD"):NULL);
    struct curl_slist *headers=NULL;
    if(s->dav&&directory)headers=curl_slist_append(headers,"Depth: 1");
    curl_easy_setopt(s->curl,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(s->curl,CURLOPT_NOBODY,info?1L:0L);
    curl_easy_setopt(s->curl,CURLOPT_FILETIME,info?1L:0L);
    curl_easy_setopt(s->curl,CURLOPT_RANGE,range);
    curl_easy_setopt(s->curl,CURLOPT_WRITEFUNCTION,info?discard:receive);
    curl_easy_setopt(s->curl,CURLOPT_WRITEDATA,buffer);
    s->range_valid=false;CURLcode rc=curl_easy_perform(s->curl);free(address);curl_slist_free_all(headers);curl_easy_setopt(s->curl,CURLOPT_HTTPHEADER,NULL);
    if(rc && !*s->error)snprintf(s->error,sizeof s->error,"%s: %s",s->dav?"WebDAV":"FTP",curl_easy_strerror(rc));
    if(s->dav){
        long status=0;curl_easy_getinfo(s->curl,CURLINFO_RESPONSE_CODE,&status);
        if(status && status!=(directory?207:range?206:200)){snprintf(s->error,sizeof s->error,"WebDAV HTTP %ld%s",status,status==401?" (check username/password)":range?" (byte-range support required)":"");return -1;}
        if(!rc&&range){unsigned long long start,end;
            if(sscanf(range,"%llu-%llu",&start,&end)!=2||!s->range_valid||s->range_start!=start||s->range_end<start||s->range_end>end||s->range_end>=s->range_total||s->range_end!=(end<s->range_total?end:s->range_total-1)||buffer->used!=s->range_end-start+1){snprintf(s->error,sizeof s->error,"WebDAV returned an invalid or incomplete byte range");return -1;}
        }
    }
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
RemoteSource *remote_webdav(const char *host,unsigned port,const char *user,const char *password,bool tls,bool (*cancel)(void)){
    RemoteSource *s=remote_ftp(host,port,user,password,cancel);if(!s)return NULL;s->dav=true;s->tls=tls;
    snprintf(s->base,sizeof s->base,"%s://%s:%u/",tls?"https":"http",host,port?port:tls?443:80);
    curl_easy_setopt(s->curl,CURLOPT_USERNAME,user);curl_easy_setopt(s->curl,CURLOPT_PASSWORD,password);
    curl_easy_setopt(s->curl,CURLOPT_HTTPAUTH,(long)CURLAUTH_BASIC);
    curl_easy_setopt(s->curl,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(s->curl,CURLOPT_SSL_VERIFYPEER,1L);curl_easy_setopt(s->curl,CURLOPT_SSL_VERIFYHOST,2L);
    struct curl_blob ca={(void*)atmosphere_ca,sizeof atmosphere_ca-1,CURL_BLOB_COPY};curl_easy_setopt(s->curl,CURLOPT_CAINFO_BLOB,&ca);
    curl_easy_setopt(s->curl,CURLOPT_HEADERFUNCTION,header);curl_easy_setopt(s->curl,CURLOPT_HEADERDATA,s);
    return s;
}
const char *remote_protocol(RemoteSource *s){return s->smb?"smb":s->dav?(s->tls?"webdavs":"webdav"):"ftp";}
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
    if(!b.data){if(s->dav)goto fail;return d;}b.data[b.used]=0;
    if(s->dav){if(webdav_listing((char*)b.data,b.used,s->base,path,&d->entries,&d->count)){snprintf(s->error,sizeof s->error,"Invalid or unsafe WebDAV directory listing");goto fail;}free(b.data);return d;}
    char *save,*line=strtok_r((char *)b.data,"\n",&save);
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

static bool command_path(const char *path){
    if(!path||!*path||strlen(path)>4096)return false;
    for(const unsigned char *p=(const unsigned char*)path;*p;p++)if(*p<32||*p==127)return false;
    return true;
}
static int ftp_commands(RemoteSource *s,const char *verb,const char *path,const char *second){
    if(s->dav||!command_path(path)||(second&&!command_path(second)))return -1;
    char command[4200];struct curl_slist *commands=NULL;
    snprintf(command,sizeof command,"%s /%s",verb,path);commands=curl_slist_append(commands,command);
    if(second){snprintf(command,sizeof command,"RNTO /%s",second);commands=curl_slist_append(commands,command);}
    s->error[0]=0;
    curl_easy_setopt(s->curl,CURLOPT_URL,s->base);
    curl_easy_setopt(s->curl,CURLOPT_UPLOAD,0L);
    curl_easy_setopt(s->curl,CURLOPT_CUSTOMREQUEST,NULL);
    curl_easy_setopt(s->curl,CURLOPT_RANGE,NULL);
    curl_easy_setopt(s->curl,CURLOPT_NOBODY,1L);
    curl_easy_setopt(s->curl,CURLOPT_WRITEFUNCTION,discard);curl_easy_setopt(s->curl,CURLOPT_WRITEDATA,NULL);
    curl_easy_setopt(s->curl,CURLOPT_QUOTE,commands);
    CURLcode rc=curl_easy_perform(s->curl);
    curl_easy_setopt(s->curl,CURLOPT_QUOTE,NULL);curl_slist_free_all(commands);
    if(rc&&!*s->error)snprintf(s->error,sizeof s->error,"FTP: %s",curl_easy_strerror(rc));
    return rc?-1:0;
}
int remote_mkdir(RemoteSource *s,const char *path){return !command_path(path)?-1:s->smb?smb2_mkdir(s->smb,path):ftp_commands(s,"MKD",path,NULL);}
int remote_rmdir(RemoteSource *s,const char *path){return !command_path(path)?-1:s->smb?smb2_rmdir(s->smb,path):ftp_commands(s,"RMD",path,NULL);}
int remote_rename(RemoteSource *s,const char *from,const char *to){return !command_path(from)||!command_path(to)?-1:s->smb?smb2_rename(s->smb,from,to):ftp_commands(s,"RNFR",from,to);}
typedef struct {size_t (*read_data)(void *,size_t,void *);void *context;} Upload;
static size_t upload_read(char *data,size_t size,size_t count,void *opaque){
    Upload *u=opaque;if(size&&count>SIZE_MAX/size)return CURL_READFUNC_ABORT;
    size_t n=u->read_data(data,size*count,u->context);return n==SIZE_MAX?CURL_READFUNC_ABORT:n;
}
int remote_upload(RemoteSource *s,const char *path,uint64_t size,size_t (*read_data)(void *,size_t,void *),void *context){
    if(!command_path(path)||size>INT64_MAX||s->dav)return -1;
    if(s->smb){
        struct smb2fh *file=smb2_open(s->smb,path,O_WRONLY|O_CREAT|O_EXCL);if(!file)return -1;
        uint32_t chunk=smb2_get_max_write_size(s->smb);if(chunk>1024*1024)chunk=1024*1024;
        unsigned char *buffer=chunk?malloc(chunk):NULL;int rc=buffer?0:-1;uint64_t offset=0;
        while(!rc&&offset<size){size_t wanted=size-offset<chunk?(size_t)(size-offset):chunk;
            size_t n=read_data(buffer,wanted,context);if(!n||n==SIZE_MAX||n>wanted){rc=-1;break;}
            size_t done=0;while(done<n){int wrote=smb2_pwrite(s->smb,file,buffer+done,(uint32_t)(n-done),offset+done);if(wrote<=0){rc=-1;break;}done+=(size_t)wrote;}offset+=done;
        }
        if(!rc)rc=smb2_fsync(s->smb,file);free(buffer);if(smb2_close(s->smb,file))rc=-1;return rc;
    }
    char *address=url(s,path,false);if(!address)return -1;Upload upload={read_data,context};s->error[0]=0;
    curl_easy_setopt(s->curl,CURLOPT_URL,address);curl_easy_setopt(s->curl,CURLOPT_CUSTOMREQUEST,NULL);
    curl_easy_setopt(s->curl,CURLOPT_RANGE,NULL);curl_easy_setopt(s->curl,CURLOPT_NOBODY,0L);
    curl_easy_setopt(s->curl,CURLOPT_UPLOAD,1L);curl_easy_setopt(s->curl,CURLOPT_INFILESIZE_LARGE,(curl_off_t)size);
    curl_easy_setopt(s->curl,CURLOPT_WRITEFUNCTION,discard);curl_easy_setopt(s->curl,CURLOPT_WRITEDATA,NULL);
    curl_easy_setopt(s->curl,CURLOPT_READFUNCTION,upload_read);curl_easy_setopt(s->curl,CURLOPT_READDATA,&upload);
    CURLcode rc=curl_easy_perform(s->curl);free(address);
    curl_easy_setopt(s->curl,CURLOPT_UPLOAD,0L);curl_easy_setopt(s->curl,CURLOPT_READFUNCTION,NULL);curl_easy_setopt(s->curl,CURLOPT_READDATA,NULL);
    if(rc&&!*s->error)snprintf(s->error,sizeof s->error,"FTP upload: %s",curl_easy_strerror(rc));return rc?-1:0;
}
