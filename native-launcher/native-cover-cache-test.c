#define _GNU_SOURCE
#define ATMOSPHERE_NATIVE_APP
#include "../backend/smb.c"
#include <assert.h>
Atmosphere atmosphere;
void copy_text(char *out,size_t cap,const char *value) {snprintf(out,cap,"%s",value?value:"");}
const char *json_text(const cJSON *o,const char *key) {const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsString(v)?v->valuestring:"";}
int64_t json_int(const cJSON *o,const char *key) {const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsNumber(v)?(int64_t)v->valuedouble:0;}
static void *deny(size_t size) {(void)size;return NULL;}
int main(void) {
    char dir[]="/tmp/atmosphere-covers-XXXXXX";assert(mkdtemp(dir));
    copy_text(atmosphere.state_dir,sizeof atmosphere.state_dir,dir);
    atmosphere.state_fd=open(dir,O_RDONLY|O_DIRECTORY);assert(atmosphere.state_fd>=0);
    settings=cJSON_CreateObject();games=cJSON_CreateArray();sources=cJSON_CreateArray();
    cJSON_AddStringToObject(settings,"server","192.168.0.113");
    unsigned char png[]={137,80,78,71,13,10,26,10,1,2,3,4};
    cJSON *g=cJSON_CreateObject();apply_icon(g,png,sizeof png);
    const char *path=json_text(g,"cover");assert(!strncmp(path,dir,strlen(dir)) && !strstr(path,"base64"));
    char cover[1200];copy_text(cover,sizeof cover,path);
    int fd=open(cover,O_RDONLY);assert(fd>=0);unsigned char data[sizeof png];assert(read(fd,data,sizeof data)==sizeof data);close(fd);assert(!memcmp(data,png,sizeof data));
    cJSON_AddItemToArray(games,g);
    for(int i=0;i<99;i++){g=cJSON_CreateObject();apply_icon(g,png,sizeof png);assert(!strcmp(json_text(g,"cover"),cover));cJSON_AddItemToArray(games,g);}
    cJSON_AddItemToArray(sources,capture_source());assert(save_locked()==0);
    char *list=cJSON_PrintUnformatted(games);assert(strlen(list)<20000);free(list);
    // A failed JSON allocation must not overwrite the good on-disk list.
    cJSON_Hooks hooks={deny,free};cJSON_InitHooks(&hooks);assert(save_locked()==-1);cJSON_InitHooks(NULL);
    fd=openat(atmosphere.state_fd,state_name,O_RDONLY);assert(fd>=0);char saved[40000]={0};int n=read(fd,saved,sizeof(saved)-1);close(fd);assert(n>0);
    cJSON *state=cJSON_Parse(saved);assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(state,"games"))==100);cJSON_Delete(state);
    cJSON *broken=cJSON_CreateObject();cJSON_AddStringToObject(broken,"id","default");cJSON *cfg=cJSON_AddObjectToObject(broken,"settings");cJSON_AddStringToObject(cfg,"server","192.168.0.113");
    load_source(broken);assert(cJSON_IsArray(games) && !strcmp(json_text(settings,"server"),"192.168.0.113"));cJSON_Delete(broken);
    cJSON_Delete(settings);cJSON_Delete(games);cJSON_Delete(sources);
    unlink(cover);unlinkat(atmosphere.state_fd,state_name,0);close(atmosphere.state_fd);rmdir(dir);
    puts("PASS: file-backed covers, compact game list, failed-save preservation and missing-list recovery");
}
