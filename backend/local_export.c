/* Copy a validated installed source into a new server-owned backup container.
 * Every local component is opened without following links; files are read-only.
 * Failed transfers remain hidden and are never mistaken for complete backups. */
#define _POSIX_C_SOURCE 200809L
#include "local_export.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#define EXPORT_PATH 1536
#define EXPORT_CHUNK (1024*1024)
typedef struct {char *path;struct stat st;bool directory;} Entry;
typedef struct {Entry *entries;size_t count;uint64_t total,done;dev_t device;bool (*cancel)(void);ExportProgress progress;const char *stage;int fd;EVP_MD_CTX *digest;} Export;
static bool safe_name(const char *name){
    if(!*name||!strcmp(name,".")||!strcmp(name,"..")||strlen(name)>240)return false;
    for(const unsigned char *p=(const unsigned char*)name;*p;p++)if(*p<32||*p==127||strchr("/\\:*?\"<>|",*p))return false;
    return name[strlen(name)-1]!='.'&&name[strlen(name)-1]!=' ';
}
static bool combine(char out[EXPORT_PATH],const char *a,const char *b){return snprintf(out,EXPORT_PATH,"%s%s%s",a,*a&&*b?"/":"",b)<EXPORT_PATH;}
static int destination_available(RemoteSource *remote,const char *folder,const char *name){
    RemoteDir *dir=remote_opendir(remote,folder);if(!dir)return -1;
    bool exists=false;struct smb2dirent *entry;
    while((entry=remote_readdir(remote,dir)))if(!strcasecmp(entry->name,name))exists=true;
    remote_closedir(remote,dir);return exists?1:0;
}
/* Open each component from /, including ancestors of the source root. */
static int open_path(int parent,const char *path){
    if(!*path||strlen(path)>=EXPORT_PATH)return -1;
    int fd=parent<0?open("/",O_RDONLY|O_DIRECTORY):dup(parent);if(fd<0)return -1;
    char copy[EXPORT_PATH];strcpy(copy,path);char *save=NULL,*part=strtok_r(copy,"/",&save);
    while(part){if(!safe_name(part)){close(fd);return -1;}char *next=strtok_r(NULL,"/",&save);
        int child=openat(fd,part,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|(next?O_DIRECTORY:0));close(fd);fd=child;if(fd<0)return -1;part=next;
    }return fd;
}
static bool unchanged(const struct stat *a,const struct stat *b){return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_mode==b->st_mode&&a->st_size==b->st_size&&a->st_mtime==b->st_mtime&&a->st_ctime==b->st_ctime;}
static int enumerate(Export *x,int fd,const char *absolute,const char *relative,unsigned depth){
    if(x->cancel()||depth>32||x->count>=100000)return -1;
    struct stat st;if(fstat(fd,&st)||st.st_dev!=x->device||(!S_ISREG(st.st_mode)&&!S_ISDIR(st.st_mode)))return -1;
    if(S_ISREG(st.st_mode)&&(st.st_size<0||UINT64_MAX-x->total<(uint64_t)st.st_size))return -1;
    Entry *items=realloc(x->entries,(x->count+1)*sizeof *items);if(!items)return -1;x->entries=items;
    char *name=strdup(relative);if(!name)return -1;x->entries[x->count++]=(Entry){name,st,S_ISDIR(st.st_mode)};
    if(S_ISREG(st.st_mode)){x->total+=(uint64_t)st.st_size;return 0;}
    /* fdopendir is not exported by native PS5 libc. Enumerate names with
     * opendir, but resolve all entries through the anchored descriptor. */
    DIR *dir=opendir(absolute);if(!dir)return -1;struct dirent *entry;int rc=0;
    for(;;){errno=0;entry=readdir(dir);if(!entry){if(errno)rc=-1;break;}
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        char local[EXPORT_PATH],child[EXPORT_PATH];
        if(!safe_name(entry->d_name)||!combine(local,absolute,entry->d_name)||!combine(child,relative,entry->d_name)){rc=-1;break;}
        int sub=openat(fd,entry->d_name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(sub<0){rc=-1;break;}
        rc=enumerate(x,sub,local,child,depth+1);close(sub);if(rc)break;
    }closedir(dir);return rc;
}
static size_t source_read(void *data,size_t capacity,void *opaque){
    Export *x=opaque;if(x->cancel())return SIZE_MAX;
    ssize_t n;do{n=read(x->fd,data,capacity);}while(n<0&&errno==EINTR);
    if(n<0)return SIZE_MAX;if(n&&EVP_DigestUpdate(x->digest,data,(size_t)n)!=1)return SIZE_MAX;
    x->done+=(uint64_t)n;x->progress(x->done,x->total,"Uploading",x->stage);return (size_t)n;
}
static int verify(RemoteSource *remote,const char *path,const Entry *entry,const unsigned char expected[32],Export *x){
    RemoteFile *file=remote_open(remote,path,O_RDONLY);if(!file)return -1;
    struct smb2_stat_64 st;int rc=remote_fstat(remote,file,&st);
    if(rc||st.smb2_size!=(uint64_t)entry->st.st_size){remote_close(remote,file);return -1;}
    if(!expected)return remote_close(remote,file);
    EVP_MD_CTX *digest=EVP_MD_CTX_new();unsigned char *buffer=malloc(EXPORT_CHUNK);unsigned char actual[32];unsigned length=0;
    rc=!digest||!buffer||EVP_DigestInit_ex(digest,EVP_sha256(),NULL)!=1?-1:0;
    uint32_t chunk=remote_max_read(remote);if(chunk>EXPORT_CHUNK)chunk=EXPORT_CHUNK;if(!chunk)rc=-1;
    for(uint64_t offset=0;!rc&&offset<st.smb2_size;){
        if(x->cancel()){rc=-1;break;}uint32_t wanted=st.smb2_size-offset<chunk?(uint32_t)(st.smb2_size-offset):chunk;
        int n=remote_pread(remote,file,buffer,wanted,offset);if(n<=0||(uint32_t)n>wanted||EVP_DigestUpdate(digest,buffer,(size_t)n)!=1){rc=-1;break;}offset+=(unsigned)n;
    }
    if(!rc&&(EVP_DigestFinal_ex(digest,actual,&length)!=1||length!=32||memcmp(expected,actual,32)))rc=-1;
    free(buffer);EVP_MD_CTX_free(digest);if(remote_close(remote,file))rc=-1;return rc;
}
int local_export(RemoteSource *remote,const cJSON *work,const char *folder,bool (*cancel)(void),ExportProgress progress,char error[256]){
    Export x={.cancel=cancel,.progress=progress,.fd=-1};int root=-1,source=-1,rc=-1;bool created=false;
    bool skip_verify=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(work,"skipVerification"));
    char stage[EXPORT_PATH]={0},final[EXPORT_PATH],name[160],token[33];const char *path=json_text(work,"path"),*base=strrchr(path,'/');
    const char *root_path=json_text(work,"root");size_t root_length=strlen(root_path);
    snprintf(error,256,"Local source is unavailable or changed. Refresh the library.");
    if(!base||!safe_name(base+1)||!root_length||strncmp(path,root_path,root_length)||path[root_length]!='/')goto end;
    root=open_path(-1,root_path);if(root<0)goto end;
    source=open_path(root,path+root_length+1);if(source<0)goto end;struct stat st;
    if(fstat(source,&st)||(uint64_t)st.st_dev!=strtoull(json_text(work,"device"),NULL,10)||(uint64_t)st.st_ino!=strtoull(json_text(work,"inode"),NULL,10))goto end;
    x.device=st.st_dev;
    progress(0,0,"Checking local files","");
    if(enumerate(&x,source,path,base+1,0)){snprintf(error,256,"Cannot read every source file safely; links or changed/unreadable files are not copied.");goto end;}
    random_hex(token,16);snprintf(name,sizeof name,".atmosphere-upload-%s",token);
    if(!combine(stage,folder,name))goto end;
    if(!combine(final,folder,base+1))goto end;
    int available=destination_available(remote,folder,base+1);
    if(available){snprintf(error,256,available>0?"A file or folder with this name already exists on the server. Nothing was replaced.":"Cannot check the server destination folder.");goto end;}
    x.stage=stage;progress(0,x.total,"Checking server write access",stage);
    if(remote_mkdir(remote,stage)){snprintf(error,256,"Server folder is not writable: %.190s",remote_error(remote));goto end;}
    created=true;
    for(size_t i=0;i<x.count;i++){
        Entry *entry=&x.entries[i];char target[EXPORT_PATH];if(cancel()||!combine(target,stage,entry->path))goto partial;
        /* The first component is the original game's basename. */
        const char *relative=entry->path+strlen(base+1);if(*relative=='/')relative++;
        /* Native dup supports directories only; reopen the root image through
         * its verified parent and compare its identity before reading. */
        int fd=*relative?open_path(source,relative):open_path(root,path+root_length+1);if(fd<0)goto partial;
        struct stat before,after;if(fstat(fd,&before)||!unchanged(&entry->st,&before)){close(fd);goto partial;}
        if(entry->directory){int result=remote_mkdir(remote,target);close(fd);if(result)goto partial;continue;}
        x.fd=fd;x.digest=EVP_MD_CTX_new();unsigned char hash[32];unsigned length=0;
        int result=!x.digest||EVP_DigestInit_ex(x.digest,EVP_sha256(),NULL)!=1?-1:remote_upload(remote,target,(uint64_t)before.st_size,source_read,&x);
        if(!result&&(EVP_DigestFinal_ex(x.digest,hash,&length)!=1||length!=32||fstat(fd,&after)||!unchanged(&before,&after)))result=-1;
        EVP_MD_CTX_free(x.digest);x.digest=NULL;close(fd);x.fd=-1;
        if(result)goto partial;
        progress(x.done,x.total,skip_verify?"Checking server file size":"Verifying server copy (SHA-256)",stage);
        if(verify(remote,target,entry,skip_verify?NULL:hash,&x))goto partial;
    }
    /* Recheck directory metadata too: newly added/removed local files invalidate
     * the manifest rather than silently producing an incomplete folder backup. */
    for(size_t i=0;i<x.count;i++){
        const char *relative=x.entries[i].path+strlen(base+1);if(*relative=='/')relative++;
        int fd=*relative?open_path(source,relative):open_path(root,path+root_length+1);struct stat now;
        bool valid=fd>=0&&!fstat(fd,&now)&&unchanged(&x.entries[i].st,&now);if(fd>=0)close(fd);if(!valid||cancel())goto partial;
    }
    /* Publish the game itself, without a backup container. Recheck names just
     * before rename in case another client created the destination meanwhile. */
    char staged_game[EXPORT_PATH];
    if(destination_available(remote,folder,base+1)||!combine(staged_game,stage,base+1)||cancel()||remote_rename(remote,staged_game,final))goto partial;
    remote_rmdir(remote,stage); /* Empty staging cleanup cannot invalidate a completed copy. */
    progress(x.done,x.total,skip_verify?"Server backup complete (verification skipped)":"Server backup verified",final);error[0]=0;rc=0;goto end;
partial:
    snprintf(error,256,"%s; partial backup kept at %.140s. %.55s",cancel()?"Upload cancelled":"Upload or verification failed",stage,remote_error(remote));
end:
    if(rc&&created)progress(x.done,x.total,"Incomplete server backup (hidden)",stage);
    if(source>=0)close(source);if(root>=0)close(root);
    for(size_t i=0;i<x.count;i++)free(x.entries[i].path);free(x.entries);return rc;
}
