#include "remote_source.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool cancelled;
static bool stop(void){return cancelled;}
int main(int argc,char **argv){
    if(argc!=2)return 2;curl_global_init(CURL_GLOBAL_DEFAULT);
    RemoteSource *s=remote_ftp("127.0.0.1",(unsigned)atoi(argv[1]),"","",stop);
    RemoteDir *d=remote_opendir(s,"games");if(!d){fprintf(stderr,"%s\n",remote_error(s));return 1;}
    struct smb2dirent *e=remote_readdir(s,d);if(!e||strcmp(e->name,"game #1.exfat")||e->st.smb2_size!=9000000)return 3;
    if(remote_readdir(s,d))return 4;remote_closedir(s,d);
    struct smb2_stat_64 st;if(remote_stat(s,"games/game #1.exfat",&st)||st.smb2_mtime!=1704067200)return 5;
    RemoteFile *f=remote_open(s,"games/game #1.exfat",0);if(!f||remote_fstat(s,f,&st)||st.smb2_size!=9000000||st.smb2_mtime!=1704067200){fprintf(stderr,"stat failed: %s size=%llu time=%llu\n",remote_error(s),(unsigned long long)st.smb2_size,(unsigned long long)st.smb2_mtime);if(f)remote_close(s,f);remote_destroy(s);return 6;}
    unsigned char *buf=malloc(4*1024*1024);
    const unsigned offsets[]={0,123,4194304,8999900};const unsigned lengths[]={4194304,100003,4194304,100};
    for(unsigned k=0;k<4;k++){
        int n=remote_pread(s,f,buf,lengths[k],offsets[k]);if(n!=(int)lengths[k]){fprintf(stderr,"read %u: %d %s\n",k,n,remote_error(s));return 7;}
        for(int i=0;i<n;i++)if(buf[i]!=(unsigned char)((offsets[k]+i)%251))return 8;
    }
    cancelled=true;if(remote_pread(s,f,buf,100,0)>=0)return 9;
    remote_close(s,f);remote_destroy(s);free(buf);curl_global_cleanup();
    puts("PASS: FTP MLSD, escaped names, stat timestamps, 4MB ranges, nonsequential reads and cancellation");return 0;
}
