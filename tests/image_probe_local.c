#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include "image_metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int read_at(void *context,uint64_t offset,void *out,size_t length) {
    FILE *f=context; return fseeko(f,(off_t)offset,SEEK_SET)||fread(out,1,length,f)!=length;
}
static FILE *background;
static int stream_art(void *context,const void *data,size_t length){
    (void)context;if(length>65536)return -1;
    return fwrite(data,1,length,background)!=length;
}
static int save(const char *prefix,const char *ext,const void *data,size_t length) {
    if(!data)return 0;char path[1024];snprintf(path,sizeof path,"%s.%s",prefix,ext);
    FILE *f=fopen(path,"wb");if(!f)return -1;
    int rc=fwrite(data,1,length,f)!=length;return fclose(f)||rc;
}
int main(int argc,char **argv) {
    if(argc!=4||strcmp(argv[1],"--local"))return 2;
    FILE *f=fopen(argv[2],"rb");if(!f)return 2;struct stat st;fstat(fileno(f),&st);
    ImageSource source={f,(uint64_t)st.st_size,read_at};ImageMetadata result;
    source.skip_background=getenv("SKIP_BACKGROUND")!=NULL;
    if(getenv("STREAM_DDS")){char path[1024];snprintf(path,sizeof path,"%s.background.dds",argv[3]);background=fopen(path,"wb");if(!background)return 2;source.background_write=stream_art;}
    int rc=image_metadata_read(&source,&result);
    if(save(argv[3],"json",result.param,result.param_size)||save(argv[3],"png",result.icon,result.icon_size)||save(argv[3],"background.png",result.background,result.background_size))rc=-1;
    image_metadata_free(&result);fclose(f);if(background)fclose(background);return rc?1:0;
}
