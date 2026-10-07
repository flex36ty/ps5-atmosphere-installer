/* One-shot payload: receives only a confirmed, embedded request. No listener. */
#define _POSIX_C_SOURCE 200809L
#include "cJSON.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
__attribute__((used)) volatile char atmosphere_delete_config[8192]="ATMOSPHERE_DELETE_CONFIG_V1";
static const char *text(const cJSON *o,const char *k){const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsString(v)?v->valuestring:"";}
static int number(const cJSON *o,const char *k){const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsNumber(v)?v->valueint:-1;}
static char state[1536];static const char *request_id;
static int record(const char *name,const char *status,const char *message){
    char path[1700],tmp[1700];snprintf(path,sizeof path,"%s/%s",state,name);snprintf(tmp,sizeof tmp,"%s/%s.tmp",state,name);
    cJSON *o=cJSON_CreateObject();cJSON_AddStringToObject(o,"requestId",request_id);cJSON_AddStringToObject(o,"state",status);cJSON_AddStringToObject(o,"message",message);
    char *data=cJSON_PrintUnformatted(o);cJSON_Delete(o);if(!data)return -1;
    int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600),rc=-1;
    if(fd>=0){size_t n=strlen(data);rc=write(fd,data,n)==(ssize_t)n && !fsync(fd)?0:-1;close(fd);if(!rc)rc=rename(tmp,path);}
    free(data);return rc;
}
static cJSON *read_record(const char *name){char path[1700],buf[4096];snprintf(path,sizeof path,"%s/%s",state,name);int fd=open(path,O_RDONLY|O_NOFOLLOW);if(fd<0)return NULL;ssize_t n=read(fd,buf,sizeof buf-1);close(fd);if(n<=0)return NULL;buf[n]=0;return cJSON_Parse(buf);}
typedef struct {char *p;size_t n;} Buffer;
static size_t receive(void *data,size_t a,size_t b,void *ctx){Buffer *v=ctx;size_t n=a*b;if(n>8*1024*1024-v->n)return 0;char *p=realloc(v->p,v->n+n+1);if(!p)return 0;v->p=p;memcpy(p+v->n,data,n);v->n+=n;p[v->n]=0;return n;}
static cJSON *api(int port,const char *route,const char *body){
    CURL *c=curl_easy_init();if(!c)return NULL;Buffer buf={0};char url[128];snprintf(url,sizeof url,"http://127.0.0.1:%d/api/v1/%s",port,route);
    struct curl_slist *headers=curl_slist_append(NULL,"Content-Type: application/json");
    curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_PROXY,"");curl_easy_setopt(c,CURLOPT_POSTFIELDS,body);curl_easy_setopt(c,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT_MS,1000L);curl_easy_setopt(c,CURLOPT_TIMEOUT_MS,15000L);curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(c,CURLOPT_WRITEDATA,&buf);
    CURLcode rc=curl_easy_perform(c);curl_easy_cleanup(c);curl_slist_free_all(headers);cJSON *out=rc==CURLE_OK&&buf.p?cJSON_Parse(buf.p):NULL;free(buf.p);return out;
}
static int fail(const char *message){record("delete-result.json","error",message);return 1;}
int main(int argc,char **argv){
    char config[8192];memcpy(config,(const void*)atmosphere_delete_config,sizeof config);config[sizeof config-1]=0;
#ifdef ATMOSPHERE_DELETE_TEST
    if(argc!=2)return 1;FILE *input=fopen(argv[1],"rb");if(!input)return 1;size_t n=fread(config,1,sizeof config-1,input);fclose(input);config[n]=0;
#else
    (void)argc;(void)argv;
#endif
    cJSON *r=cJSON_Parse(config);if(!r)return 1;
    request_id=text(r,"requestId");const char *path=text(r,"path"),*title=text(r,"titleId"),*dir=text(r,"statePath");
    int pid=number(r,"pid"),port=number(r,"port");double deadline=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r,"deadline"));
    if(strlen(request_id)<8||strlen(request_id)>64||strlen(title)!=9||!strcmp(title,"PPSA99005")||pid<2||port<1||port>65535||!isfinite(deadline)||deadline<time(NULL)||deadline>time(NULL)+120)return 1;
    if(strstr(path,"..")||strstr(dir,"..")||strlen(dir)>=sizeof state||!strstr(dir,"/PPSA99005/atmosphere-state")||
       (strncmp(path,"/data/",6)&&strncmp(path,"/mnt/usb",8)))return 1;
    snprintf(state,sizeof state,"%s",dir);
    if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r,"probe")))return record("delete-helper-probe.json","ready","Background delete helper started successfully.")?1:0;
    if(record("delete-ready.json","ready","Helper ready"))return 1;
    /* Parent commits only after observing this request's ready record. */
    int committed=0;for(int i=0;i<15;i++){cJSON *c=read_record("delete-commit.json");committed=!strcmp(text(c,"requestId"),request_id);cJSON_Delete(c);if(committed)break;sleep(1);}
    if(!committed)return fail("Delete cancelled: handoff was not committed.");
    int exited=0;for(int i=0;i<35;i++){errno=0;if(kill(pid,0)<0&&errno==ESRCH){exited=1;break;}sleep(1);}
    if(!exited)return fail("Delete cancelled: Atmosphere did not close in time.");
    sleep(3);if(time(NULL)>deadline)return fail("Delete request expired. Confirm it again in Atmosphere.");
    curl_global_init(CURL_GLOBAL_DEFAULT);
    cJSON *inventory=api(port,"games","{\"include_size\":false}");cJSON *g,*found=NULL;
    if(!inventory||number(inventory,"status")!=0){cJSON_Delete(inventory);return fail("Cannot recheck installed games; nothing deleted.");}
    cJSON_ArrayForEach(g,cJSON_GetObjectItemCaseSensitive(inventory,"games"))if(!strcmp(text(g,"title_id"),title)){if(found){cJSON_Delete(inventory);return fail("Ambiguous title; nothing deleted.");}found=g;}
    if(!found||strcmp(text(found,"path"),path)||!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(found,"can_manage_source"))||!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(found,"source_available"))){cJSON_Delete(inventory);return fail("Installed location changed; nothing deleted.");}
    cJSON_Delete(inventory);
    cJSON *body=cJSON_CreateObject();cJSON_AddStringToObject(body,"title_id",title);cJSON_AddBoolToObject(body,"confirm",1);char *encoded=cJSON_PrintUnformatted(body);cJSON_Delete(body);
    /* Never resend an uncertain mutation: a timeout can still mean accepted. */
    cJSON *reply=api(port,"games/delete",encoded);free(encoded);
    if(!reply)return fail("Delete response unconfirmed. Check installed status before retrying.");
    if(number(reply,"status")!=0){char error[512];snprintf(error,sizeof error,"Delete failed: %s",text(reply,"error"));cJSON_Delete(reply);return fail(error);}
    if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply,"source_missing"))){cJSON_Delete(reply);record("delete-result.json","complete","Installed source was already absent.");return 0;}
    int job=number(reply,"job_id");cJSON_Delete(reply);if(job<=0)return fail("Delete result unconfirmed: no job ID.");
    record("delete-result.json","running","Deleting installed game…");
    char query[80];snprintf(query,sizeof query,"{\"job_id\":%d}",job);
    for(int i=0;i<300;i++){
        sleep(2);reply=api(port,"games/storage/status",query);
        if(!reply||number(reply,"status")!=0||number(reply,"job_id")!=job){cJSON_Delete(reply);return fail("Delete progress unavailable. Check installed status before retrying.");}
        if(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply,"active"))){
            if(number(reply,"result_status")){char error[512];snprintf(error,sizeof error,"Delete failed: %s",text(reply,"result_error"));cJSON_Delete(reply);return fail(error);}
            cJSON_Delete(reply);record("delete-result.json","complete","Installed game deleted.");return 0;
        }cJSON_Delete(reply);
    }
    return fail("Deletion is still running in ShadowMount. Check installed status later.");
}
