#include "atmosphere.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef ATMOSPHERE_NATIVE_APP
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <openssl/rand.h>
int __real_connect(int,const struct sockaddr *,socklen_t);
#endif
int storage_repair_permissions(const char *name,char *error,size_t cap) {
#ifndef ATMOSPHERE_NATIVE_APP
    (void)name;copy_text(error,cap,"Internal permission repair requires the native app.");return -1;
#else
    if(strcmp(name,"homebrew") && strcmp(name,".atmosphere-smb-staging"))return -1;
    unsigned char nonce[16];if(RAND_bytes(nonce,sizeof nonce)!=1){copy_text(error,cap,"Cannot create permission repair request.");return -1;}
    char id[33],config[256];for(unsigned i=0;i<sizeof nonce;i++)snprintf(id+i*2,3,"%02x",nonce[i]);
    snprintf(config,sizeof config,"{\"requestId\":\"%s\",\"directory\":\"%s\",\"pid\":%d,\"deadline\":%lld}",id,name,(int)getpid(),(long long)time(NULL)+45);
    int fd=open("/app0/atmosphere_permissions.elf",O_RDONLY|O_NOFOLLOW);struct stat st;unsigned char *elf=NULL;size_t size=0;
    if(fd>=0){if(!fstat(fd,&st)&&st.st_size>8192&&st.st_size<4*1024*1024){size=st.st_size;elf=malloc(size);if(elf){size_t n=0;while(n<size){ssize_t got=read(fd,elf+n,size-n);if(got<=0)break;n+=got;}if(n!=size){free(elf);elf=NULL;}}}close(fd);}
    if(!elf){copy_text(error,cap,"Internal permission helper missing or unreadable. Update the complete app folder.");return -1;}
    const char marker[]="ATMOSPHERE_PERMISSION_CONFIG_V1";size_t offset=0;unsigned matches=0;
    for(size_t i=0;i+8192<=size;i++)if(!memcmp(elf+i,marker,sizeof marker)){offset=i;matches++;}
    if(matches!=1){free(elf);copy_text(error,cap,"Invalid internal permission helper.");return -1;}
    memset(elf+offset,0,8192);memcpy(elf+offset,config,strlen(config));
    int sock=socket(AF_INET,SOCK_STREAM,0);struct sockaddr_in address={0};address.sin_len=sizeof address;address.sin_family=AF_INET;address.sin_port=htons(9021);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    struct timeval timeout={5,0};if(sock>=0)setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
    if(sock<0||__real_connect(sock,(struct sockaddr*)&address,sizeof address)){if(sock>=0)close(sock);free(elf);copy_text(error,cap,"Internal folder permission denied. Enable etaHEN's ELF loader (9021) for automatic repair.");return -1;}
    size_t sent=0;while(sent<size){ssize_t n=send(sock,elf+sent,size-sent,MSG_NOSIGNAL);if(n<=0)break;sent+=n;}
    shutdown(sock,SHUT_WR);close(sock);free(elf);
    if(sent!=size){copy_text(error,cap,"Could not send internal permission helper. Retry the copy.");return -1;}
    for(int i=0;i<12;i++){
        char buf[512];fd=open("/data/.atmosphere-permission-repair/result.json",O_RDONLY|O_NOFOLLOW);ssize_t n=fd<0?-1:read(fd,buf,sizeof buf-1);if(fd>=0)close(fd);
        if(n>0){buf[n]=0;cJSON *r=cJSON_Parse(buf);bool match=!strcmp(json_text(r,"requestId"),id);cJSON *code=cJSON_GetObjectItemCaseSensitive(r,"error");
            if(match && cJSON_IsNumber(code)){int e=code->valueint;cJSON_Delete(r);if(!e)return 0;snprintf(error,cap,"Internal permission repair failed: %s (errno %d)",strerror(e),e);return -1;}cJSON_Delete(r);
        }sleep(1);
    }
    copy_text(error,cap,"Internal permission repair was not confirmed. Check etaHEN's ELF loader and retry.");return -1;
#endif
}
