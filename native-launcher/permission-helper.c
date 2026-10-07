/* One-shot, fixed-path repair launched by the existing etaHEN ELF loader. */
#include "cJSON.h"
#include "permission-repair.h"
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
__attribute__((used)) volatile char atmosphere_permission_config[8192]="ATMOSPHERE_PERMISSION_CONFIG_V1";
static const char *text(cJSON *o,const char *k){cJSON *v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsString(v)?v->valuestring:"";}
int main(void) {
    char config[8192];memcpy(config,(const void*)atmosphere_permission_config,sizeof config);config[8191]=0;
    cJSON *r=cJSON_Parse(config);if(!r)return 1;
    const char *id=text(r,"requestId"),*name=text(r,"directory");
    double deadline=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r,"deadline"));
    double pid=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r,"pid"));
    if(strlen(id)!=32 || strspn(id,"0123456789abcdef")!=32 || !isfinite(deadline) ||
       deadline<time(NULL) || deadline>time(NULL)+60 || !isfinite(pid) || pid<2 || pid>2147483647 || pid!=(int)pid || kill((int)pid,0)) {cJSON_Delete(r);return 1;}
    int root=open("/data",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(root<0){cJSON_Delete(r);return 1;}
    /* Results live in a root-owned directory; requests cannot choose an output path. */
    if(mkdirat(root,".atmosphere-permission-repair",0755) && errno!=EEXIST){close(root);cJSON_Delete(r);return 1;}
    int resultdir=openat(root,".atmosphere-permission-repair",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    struct stat rs,ds;
    if(resultdir<0 || fstat(root,&rs) || fstat(resultdir,&ds) || ds.st_uid!=0 || ds.st_dev!=rs.st_dev || (ds.st_mode&0022)) {
        if(resultdir>=0)close(resultdir);close(root);cJSON_Delete(r);return 1;
    }
    if(fchmod(resultdir,0755)){close(resultdir);close(root);cJSON_Delete(r);return 1;}
    int rc=repair_directory(root,name),error=rc?errno:0;close(root);
    char output[256];int n=snprintf(output,sizeof output,"{\"requestId\":\"%s\",\"error\":%d}",id,error);
    int fd=openat(resultdir,id,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0644);
    if(fd>=0){if(fchmod(fd,0644) || write(fd,output,n)!=n || fsync(fd))rc=-1;close(fd);
        if(renameat(resultdir,id,resultdir,"result.json"))rc=-1;
    }else rc=-1;
    close(resultdir);cJSON_Delete(r);return rc?1:0;
}
