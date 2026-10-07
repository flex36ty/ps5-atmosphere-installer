#include "atmosphere.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef ATMOSPHERE_NATIVE_APP
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
int __real_connect(int,const struct sockaddr *,socklen_t);
int library_api_port(void);
static bool handoff_committed;
static int save_record(const char *name,const char *json){int fd=openat(atmosphere.state_fd,name,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(fd<0)return -1;size_t n=strlen(json);int rc=write(fd,json,n)==(ssize_t)n&&!fsync(fd)?0:-1;close(fd);return rc;}
#endif
cJSON *delete_handoff_result(void){
    char buf[4096];int fd=openat(atmosphere.state_fd,"delete-result.json",O_RDONLY|O_NOFOLLOW);if(fd<0)return NULL;
    ssize_t n=read(fd,buf,sizeof buf-1);close(fd);if(n<=0)return NULL;buf[n]=0;return cJSON_Parse(buf);
}
int delete_handoff(const cJSON *input,char *error,size_t cap){
#ifndef ATMOSPHERE_NATIVE_APP
    (void)input;copy_text(error,cap,"Background deletion requires the native app.");return 400;
#else
    cJSON *lib=library_snapshot(false),*target=NULL,*self=NULL,*g;int result=409;
    unsigned char *elf=NULL;cJSON *request=NULL;char *encoded=NULL;int socket_fd=-1;
    const char *id=json_text(input,"titleId"),*key=json_text(input,"sourceKey"),*rid=json_text(input,"requestId");
    if(handoff_committed){copy_text(error,cap,"A deletion is already waiting for Atmosphere to close.");goto done;}
    if(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input,"confirmed"))||strlen(rid)<8||strlen(rid)>64||!strcmp(id,"PPSA99005")){copy_text(error,cap,"Confirm a game deletion first.");goto done;}
    pthread_mutex_lock(&atmosphere.mutex);bool busy=atmosphere.smb_storage_busy||atmosphere.library_storage_busy;pthread_mutex_unlock(&atmosphere.mutex);
    if(busy){copy_text(error,cap,"Wait for storage operations to finish.");goto done;}
    if(strcmp(json_text(lib,"status"),"ready")||cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(lib,"stale"))){copy_text(error,cap,"Installed inventory is not current. Refresh and try again.");goto done;}
    cJSON_ArrayForEach(g,cJSON_GetObjectItemCaseSensitive(lib,"games")){
        if(!strcmp(json_text(g,"titleId"),id)&&!strcmp(json_text(g,"sourceKey"),key))target=g;
        if(!strcmp(json_text(g,"titleId"),"PPSA99005"))self=g;
    }
    if(!target||!self||!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(target,"canManageSource"))||!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(target,"onDrive"))){copy_text(error,cap,"Cannot verify the installed game or Atmosphere location.");goto done;}
    int port=library_api_port();if(!port){copy_text(error,cap,"ShadowMount API is disabled.");goto done;}
    char state[1536];if(snprintf(state,sizeof state,"%s/atmosphere-state",json_text(self,"path"))>=(int)sizeof state){copy_text(error,cap,"Atmosphere path is too long.");goto done;}
    request=cJSON_CreateObject();cJSON_AddStringToObject(request,"requestId",rid);cJSON_AddStringToObject(request,"titleId",id);cJSON_AddStringToObject(request,"path",json_text(target,"path"));cJSON_AddStringToObject(request,"statePath",state);
    cJSON_AddNumberToObject(request,"pid",getpid());cJSON_AddNumberToObject(request,"port",port);cJSON_AddNumberToObject(request,"deadline",time(NULL)+90);encoded=cJSON_PrintUnformatted(request);
    int fd=open("/app0/atmosphere_delete.elf",O_RDONLY|O_NOFOLLOW);struct stat st;size_t size=0;
    if(fd>=0){if(!fstat(fd,&st)&&st.st_size>8192&&st.st_size<16*1024*1024){size=(size_t)st.st_size;elf=malloc(size);if(elf){size_t n=0;while(n<size){ssize_t got=read(fd,elf+n,size-n);if(got<=0)break;n+=(size_t)got;}if(n!=size){free(elf);elf=NULL;}}}close(fd);}
    if(!elf||!encoded||strlen(encoded)>=8192){copy_text(error,cap,"Bundled delete helper is missing or unreadable.");goto done;}
    const char marker[]="ATMOSPHERE_DELETE_CONFIG_V1";size_t offset=0;unsigned matches=0;
    for(size_t i=0;i+8192<=size;i++)if(!memcmp(elf+i,marker,sizeof marker)){offset=i;matches++;}
    if(matches!=1){copy_text(error,cap,"Delete helper configuration is invalid.");goto done;}
    memset(elf+offset,0,8192);memcpy(elf+offset,encoded,strlen(encoded));
    socket_fd=socket(AF_INET,SOCK_STREAM,0);struct sockaddr_in address={0};address.sin_len=sizeof address;address.sin_family=AF_INET;address.sin_port=htons(9021);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    struct timeval timeout={5,0};setsockopt(socket_fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
    if(socket_fd<0||__real_connect(socket_fd,(struct sockaddr*)&address,sizeof address)){copy_text(error,cap,"Cannot start delete helper. Enable etaHEN's ELF loader (9021).");goto done;}
    size_t sent=0;while(sent<size){ssize_t n=send(socket_fd,elf+sent,size-sent,MSG_NOSIGNAL);if(n<=0)break;sent+=(size_t)n;}shutdown(socket_fd,SHUT_WR);close(socket_fd);socket_fd=-1;
    if(sent!=size){copy_text(error,cap,"Could not send the delete helper. Atmosphere will stay open.");goto done;}
    for(int i=0;i<12;i++){
        char buf[4096];int ready=openat(atmosphere.state_fd,"delete-ready.json",O_RDONLY|O_NOFOLLOW);ssize_t n=ready<0?-1:read(ready,buf,sizeof buf-1);if(ready>=0)close(ready);
        if(n>0){buf[n]=0;cJSON *ack=cJSON_Parse(buf);bool ok=!strcmp(json_text(ack,"requestId"),rid);cJSON_Delete(ack);if(ok){
            if(save_record("delete-commit.json",encoded)){copy_text(error,cap,"Cannot commit deletion handoff.");goto done;}
            handoff_committed=true;result=200;goto done;
        }}sleep(1);
    }
    copy_text(error,cap,"Delete helper did not acknowledge startup. Nothing was deleted.");
done:
    if(socket_fd>=0)close(socket_fd);free(elf);free(encoded);cJSON_Delete(request);cJSON_Delete(lib);return result;
#endif
}
