#include "game_details.h"
#include "game_region.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static void text(cJSON *game,const char *key,const char *value){cJSON_DeleteItemFromObjectCaseSensitive(game,key);cJSON_AddStringToObject(game,key,value);}
bool game_apply_param(cJSON *game,const unsigned char *data){
    if(!data)return false;cJSON *param=cJSON_Parse((const char*)data);if(!param)return false;
    const char *title=json_text(param,"title");
    if(!*title){cJSON *localized=cJSON_GetObjectItemCaseSensitive(param,"localizedParameters");cJSON *lang=cJSON_GetObjectItemCaseSensitive(localized,json_text(localized,"defaultLanguage"));if(!lang)lang=cJSON_GetObjectItemCaseSensitive(localized,"en-US");title=json_text(lang,"titleName");}
    bool found=*title&&strlen(title)<256;if(found)text(game,"title",title);
    const char *id=json_text(param,"titleId");if(*id&&strlen(id)<32)text(game,"titleId",id);
    const char *content=json_text(param,"contentId"),*region=game_region(content);
    if(*region&&(!*id||(strlen(id)==9&&!strncmp(content+7,id,9))))text(game,"region",region);
    const char *fw=json_text(param,"requiredSystemSoftwareVersion");
    if(strlen(fw)==18&&fw[0]=='0'&&(fw[1]=='x'||fw[1]=='X')){
        bool valid=true;for(int i=2;i<18;i++)if(!isxdigit((unsigned char)fw[i]))valid=false;
        for(int i=2;i<6;i++)if(fw[i]<'0'||fw[i]>'9')valid=false;
        if(valid&&(fw[2]!='0'||fw[3]!='0'||fw[4]!='0'||fw[5]!='0')){char version[16];snprintf(version,sizeof version,"%d.%c%c",(fw[2]-'0')*10+fw[3]-'0',fw[4],fw[5]);text(game,"minimumFirmware",version);}
    }
    cJSON_Delete(param);return found;
}
bool game_cache_icon(cJSON *game,const unsigned char *data,size_t length){
    if(!data||length<8||length>4U*1024*1024||memcmp(data,"\x89PNG\r\n\x1a\n",8))return false;
    unsigned char digest[32];unsigned digest_length;
    if(EVP_Digest(data,length,digest,&digest_length,EVP_sha256(),NULL)!=1||digest_length!=32)return false;
    char name[80]="cover-",path[1200];for(unsigned i=0;i<32;i++)snprintf(name+6+2*i,3,"%02x",digest[i]);strcat(name,".png");
    int fd=openat(atmosphere.state_fd,name,O_RDONLY|O_NOFOLLOW);struct stat st;
    bool exists=fd>=0&&!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_size==(off_t)length;if(fd>=0)close(fd);
    if(!exists){
        char token[17],temporary[48];random_hex(token,8);snprintf(temporary,sizeof temporary,".local-icon-%s",token);
        fd=openat(atmosphere.state_fd,temporary,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0)return false;
        size_t done=0;while(done<length){ssize_t n=write(fd,data+done,length-done);if(n<0&&errno==EINTR)continue;if(n<=0)break;done+=(size_t)n;}
        bool ok=done==length&&!fsync(fd);if(close(fd))ok=false;
        if(ok&&renameat(atmosphere.state_fd,temporary,atmosphere.state_fd,name))ok=false;
        if(!ok){unlinkat(atmosphere.state_fd,temporary,0);return false;}
    }
    if(snprintf(path,sizeof path,"%s/%s",atmosphere.state_dir,name)>=(int)sizeof path)return false;
    text(game,"cover",path);return true;
}
