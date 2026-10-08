#include "remote_source.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv){
    if(argc!=3)return 2;curl_global_init(CURL_GLOBAL_DEFAULT);
    RemoteSource *s=remote_webdav("127.0.0.1",(unsigned)atoi(argv[1]),"test","secret",false,NULL);
    RemoteDir *d=remote_opendir(s,"games");
    if(!strcmp(argv[2],"bad-list")){if(d)return 3;remote_destroy(s);return 0;}
    if(!d){fprintf(stderr,"%s\n",remote_error(s));return 4;}
    struct smb2dirent *e=remote_readdir(s,d);if(!e||strcmp(e->name,"a & b.ffpfsc")||e->st.smb2_size!=1024)return 5;
    remote_closedir(s,d);RemoteFile *f=remote_open(s,"games/a & b.ffpfsc",0);unsigned char data[64];
    int n=remote_pread(s,f,data,sizeof data,123);
    if(!strcmp(argv[2],"bad-read")){if(n>=0)return 6;}
    else {if(n!=64)return 7;for(int i=0;i<n;i++)if(data[i]!=(123+i)%251)return 8;}
    remote_close(s,f);remote_destroy(s);curl_global_cleanup();return 0;
}
