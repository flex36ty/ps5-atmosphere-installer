/* Read-only local title index. No artwork is loaded and no network source is
 * contacted. Scans run off the UI thread and skip private/staging directories. */
#include "atmosphere.h"
#include "image_metadata.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
static pthread_mutex_t index_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t index_changed=PTHREAD_COND_INITIALIZER;
static pthread_t index_thread;
static bool index_started,index_stop,index_pending,index_busy,index_complete;
static cJSON *index_games;
static char custom_folders[8][256];
static unsigned custom_count;
static bool stopping(void){pthread_mutex_lock(&index_lock);bool stop=index_stop;pthread_mutex_unlock(&index_lock);return stop;}
static bool normalize(const char *id,char out[10]){
    if(strlen(id)!=9 || (strncasecmp(id,"PPSA",4)&&strncasecmp(id,"CUSA",4)))return false;
    for(int i=4;i<9;i++)if(id[i]<'0'||id[i]>'9')return false;
    memcpy(out,!strncasecmp(id,"PPSA",4)?"PPSA":"CUSA",4);memcpy(out+4,id+4,6);return true;
}
static void add(cJSON *list,const char *id,const char *path,const char *storage,const char *label){
    char key[10];if(!normalize(id,key))return;cJSON *item;
    cJSON_ArrayForEach(item,list)if(!strcmp(json_text(item,"titleId"),key)&&!strcmp(json_text(item,"path"),path))return;
    if(cJSON_GetArraySize(list)>=4096)return;
    item=cJSON_CreateObject();cJSON_AddStringToObject(item,"titleId",key);cJSON_AddStringToObject(item,"path",path);
    cJSON_AddStringToObject(item,"storageId",storage);cJSON_AddStringToObject(item,"location",label);cJSON_AddItemToArray(list,item);
}
static bool param_id(const void *data,size_t size,char id[10]){
    if(!data||!size||size>65536)return false;
    cJSON *param=cJSON_ParseWithLength(data,size);bool ok=normalize(json_text(param,"titleId"),id);cJSON_Delete(param);return ok;
}
static bool folder_id(int fd,char id[10]){
    int sce=openat(fd,"sce_sys",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(sce<0)return false;
    int p=openat(sce,"param.json",O_RDONLY|O_NOFOLLOW);close(sce);if(p<0)return false;
    struct stat st;bool ok=false;
    if(!fstat(p,&st)&&S_ISREG(st.st_mode)&&st.st_size>0&&st.st_size<=65536){
        size_t length=(size_t)st.st_size;char *data=malloc(length+1);
        if(data){size_t n=0;while(n<length){ssize_t got=read(p,data+n,length-n);if(got<=0)break;n+=(size_t)got;}data[n]=0;ok=n==length&&param_id(data,n,id);free(data);}
    }
    close(p);return ok;
}
static int image_read(void *ctx,uint64_t offset,void *out,size_t size){
    int fd=*(int *)ctx;size_t done=0;
    while(done<size){if(stopping())return -1;ssize_t n=pread(fd,(char *)out+done,size-done,(off_t)(offset+done));if(n<=0)return -1;done+=(size_t)n;}return 0;
}
static bool image_id(int fd,const struct stat *st,char id[10]){
    ImageSource source={.context=&fd,.size=(uint64_t)st->st_size,.read_at=image_read,.skip_background=1,.skip_icon=1};
    ImageMetadata metadata;int rc=image_metadata_read(&source,&metadata);
    bool ok=!rc&&param_id(metadata.param,metadata.param_size,id);image_metadata_free(&metadata);return ok;
}
typedef struct {cJSON *list;const Storage *drive;unsigned visited;bool complete;} Scan;
static void scan_error(Scan *scan,const char *path,const char *step,int error){
#ifdef ATMOSPHERE_NATIVE_APP
    if(scan->complete){
        char line[1800];int n=snprintf(line,sizeof line,"%s: %s errno=%d\n",path,step,error);
        int fd=open("/app0/atmosphere-state/installed-scan.log",O_WRONLY|O_CREAT|O_APPEND,0600);
        if(fd>=0){if(n>0&&n<(int)sizeof line)write(fd,line,(size_t)n);close(fd);}
    }
#else
    (void)path;(void)step;(void)error;
#endif
    scan->complete=false;
}
static void scan_dir(Scan *scan,int fd,const char *path,unsigned depth,unsigned maxdepth){
    if(stopping()||++scan->visited>10000){scan->complete=false;close(fd);return;}
    char id[10];if(folder_id(fd,id)){add(scan->list,id,path,scan->drive->id,scan->drive->external?scan->drive->label:"Internal storage");close(fd);return;}
    /* Native libc exposes opendir, not fdopendir. Keep metadata opens anchored
     * to the verified descriptor even if the displayed path changes mid-scan. */
    DIR *dir=opendir(path);if(!dir){scan_error(scan,path,"opendir",errno);close(fd);return;}struct dirent *entry;
    while(!stopping()){
        errno=0;entry=readdir(dir);if(!entry){if(errno)scan_error(scan,path,"readdir",errno);break;}
        if(entry->d_name[0]=='.')continue;
        if(++scan->visited>10000){scan->complete=false;break;}
        char child[1536];if(snprintf(child,sizeof child,"%s/%s",path,entry->d_name)>=(int)sizeof child){scan->complete=false;continue;}
        struct stat st;if(fstatat(fd,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)){scan_error(scan,child,"fstatat",errno);continue;}
        if(S_ISDIR(st.st_mode) && depth<maxdepth){int sub=openat(fd,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(sub>=0)scan_dir(scan,sub,child,depth+1,maxdepth);else scan->complete=false;}
        else if(S_ISREG(st.st_mode)){
            const char *ext=strrchr(entry->d_name,'.');if(!ext||(strcasecmp(ext,".exfat")&&strcasecmp(ext,".ffpfsc")&&strcasecmp(ext,".ffpfs")&&strcasecmp(ext,".img")))continue;
            int image=openat(fd,entry->d_name,O_RDONLY|O_NOFOLLOW);if(image<0){scan->complete=false;continue;}
            if(image_id(image,&st,id))add(scan->list,id,child,scan->drive->id,scan->drive->external?scan->drive->label:"Internal storage");
            else scan_error(scan,child,"image metadata",0);close(image);
        }
    }
    closedir(dir);close(fd);
}
static void *index_worker(void *unused){
    (void)unused;for(;;){
        pthread_mutex_lock(&index_lock);while(!index_stop&&!index_pending)pthread_cond_wait(&index_changed,&index_lock);
        if(index_stop){pthread_mutex_unlock(&index_lock);break;}
        index_pending=false;index_busy=true;char folders[8][256];unsigned count=custom_count;memcpy(folders,custom_folders,sizeof folders);pthread_mutex_unlock(&index_lock);
        Storage drives[ATMOSPHERE_MAX_STORAGE];size_t n=storage_list(drives);cJSON *list=cJSON_CreateArray();bool complete=list!=NULL&&n>0;
        for(size_t i=0;list&&i<n&&!stopping();i++){
            Scan scan={.list=list,.drive=&drives[i],.complete=true};
            int root=open(drives[i].root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
            if(root<0){scan_error(&scan,drives[i].root,"open root",errno);complete=false;continue;}
            /* Root images and immediate game folders, then normal/custom libraries. */
            scan_dir(&scan,root,drives[i].root,0,1);
            for(unsigned f=0;f<=count&&!stopping();f++){
                const char *folder=f?folders[f-1]:"homebrew";if(!*folder)continue;
                char path[1536];if(snprintf(path,sizeof path,"%s/%s",drives[i].root,folder)>=(int)sizeof path)continue;
                int dir=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
                if(dir>=0)scan_dir(&scan,dir,path,0,6);else if(errno!=ENOENT)scan.complete=false;
            }
            complete=complete&&scan.complete;
        }
        pthread_mutex_lock(&index_lock);cJSON_Delete(index_games);index_games=list;index_complete=complete;index_busy=false;pthread_mutex_unlock(&index_lock);
    }return NULL;
}
void installed_refresh(const char *folder){
    pthread_mutex_lock(&index_lock);
    if(folder&&*folder&&strcmp(folder,"homebrew")&&strlen(folder)<256&&folder[0]!='/'&&!strstr(folder,"..")){
        unsigned i;for(i=0;i<custom_count;i++)if(!strcmp(folder,custom_folders[i]))break;
        if(i==custom_count&&custom_count<8)copy_text(custom_folders[custom_count++],256,folder);
    }
    if(index_started&&!index_stop){index_pending=true;pthread_cond_signal(&index_changed);}pthread_mutex_unlock(&index_lock);
    cJSON_Delete(library_snapshot(true));
}
int installed_start(void){
    pthread_attr_t attr;if(pthread_attr_init(&attr))return -1;
    int rc=pthread_attr_setstacksize(&attr,ATMOSPHERE_THREAD_STACK);index_pending=true;
    if(!rc)rc=pthread_create(&index_thread,&attr,index_worker,NULL);pthread_attr_destroy(&attr);index_started=!rc;return rc;
}
void installed_stop(void){pthread_mutex_lock(&index_lock);index_stop=true;pthread_cond_signal(&index_changed);pthread_mutex_unlock(&index_lock);if(index_started)pthread_join(index_thread,NULL);cJSON_Delete(index_games);index_games=NULL;index_started=false;}
cJSON *installed_snapshot(void){
    pthread_mutex_lock(&index_lock);cJSON *out=cJSON_CreateObject();cJSON *list=index_games?cJSON_Duplicate(index_games,true):cJSON_CreateArray();
    cJSON_AddBoolToObject(out,"checking",index_busy||index_pending);cJSON_AddBoolToObject(out,"complete",index_started&&index_complete&&!index_busy&&!index_pending);pthread_mutex_unlock(&index_lock);
    cJSON *library=library_snapshot(false);
    cJSON *targets=cJSON_AddArrayToObject(out,"deleteTargets");
    cJSON_AddItemToObject(out,"action",cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(library,"action"),true));
    cJSON_AddItemToObject(out,"storageJob",cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(library,"storageJob"),true));
    if(!strcmp(json_text(library,"status"),"ready")&&!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(library,"stale"))){
        /* ShadowMount supplies a validated complete inventory even when the
         * application sandbox prevents direct directory enumeration. */
        cJSON_ReplaceItemInObjectCaseSensitive(out,"complete",cJSON_CreateBool(true));
        cJSON_ReplaceItemInObjectCaseSensitive(out,"checking",cJSON_CreateBool(false));
        cJSON *game;cJSON_ArrayForEach(game,cJSON_GetObjectItemCaseSensitive(library,"games")){
            if(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(game,"installed"))&&!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(game,"onDrive")))continue;
            add(list,json_text(game,"titleId"),json_text(game,"path"),"",json_text(game,"location"));
            if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(game,"canManageSource")) && strcmp(json_text(game,"titleId"),"PPSA99005"))
                cJSON_AddItemToArray(targets,cJSON_Duplicate(game,true));
        }
    }
    cJSON_Delete(library);cJSON_AddItemToObject(out,"games",list);return out;
}
bool installed_match(const char *id){
    char key[10];if(!normalize(id,key))return false;cJSON *snapshot=installed_snapshot(),*game;bool found=false;
    cJSON_ArrayForEach(game,cJSON_GetObjectItemCaseSensitive(snapshot,"games"))if(!strcmp(json_text(game,"titleId"),key)){found=true;break;}
    cJSON_Delete(snapshot);return found;
}
