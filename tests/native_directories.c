#define _DEFAULT_SOURCE
#define ATMOSPHERE_DIRECTORY_TEST
#include "../native-launcher/native-directories.c"
#include <assert.h>
#include <stdio.h>
static unsigned char reply[65536];static int length,calls,fallback,fail_after,alternate_calls;
int sceKernelGetdents(int fd,void *data,int capacity){
    struct stat st;assert(!fstat(fd,&st)&&S_ISDIR(st.st_mode));assert(capacity==65536);
    if(fallback)return (int)0x80020001U;
    if(calls++){return fail_after?(int)0x8002000dU:0;}
    if(length<=capacity)memcpy(data,reply,(size_t)length);return length;
}
int sceKernelGetdirentries(int fd,void *data,int capacity,long *position){
    (void)fd;(void)capacity;alternate_calls++;*position=17;
    if(calls++)return 0;memcpy(data,reply,(size_t)length);return length;
}
static void reset(void){memset(reply,0,sizeof reply);length=calls=fallback=fail_after=alternate_calls=0;}
static void record(const char *name,uint32_t inode){
    unsigned n=(unsigned)strlen(name),size=(8+n+1+3)&~3U;unsigned char *p=reply+length;
    memcpy(p,&inode,4);p[4]=(unsigned char)size;p[5]=(unsigned char)(size>>8);p[6]=DT_REG;p[7]=(unsigned char)n;memcpy(p+8,name,n);length+=(int)size;
}
static void invalid(const char *path){
    DIR *d=__wrap_opendir(path);assert(d);errno=0;assert(!__wrap_readdir(d)&&errno==EIO);errno=0;assert(!__wrap_readdir(d)&&errno==EIO);assert(!__wrap_closedir(d));
}
int main(void){
    char path[]="/tmp/atmosphere-dir-XXXXXX";assert(mkdtemp(path));
    reset();record("",0);record("Game #1.ffpfsc",123);record("second",456);
    DIR *d=__wrap_opendir(path);assert(d);struct dirent *e=__wrap_readdir(d);assert(e&&!strcmp(e->d_name,"Game #1.ffpfsc")&&e->d_fileno==123);
    e=__wrap_readdir(d);assert(e&&!strcmp(e->d_name,"second"));errno=EPERM;assert(!__wrap_readdir(d)&&errno==0);assert(!__wrap_closedir(d));
    reset();record("fallback",42);fallback=1;d=__wrap_opendir(path);assert(d&&__wrap_readdir(d));assert(!__wrap_readdir(d)&&errno==0&&alternate_calls==2);__wrap_closedir(d);
    reset();record("first",42);fail_after=1;d=__wrap_opendir(path);assert(d&&__wrap_readdir(d));assert(!__wrap_readdir(d)&&errno==EACCES&&alternate_calls==0);__wrap_closedir(d);
    reset();length=7;invalid(path);
    reset();record("x",1);reply[4]=0;invalid(path);
    reset();record("x",1);reply[4]=255;invalid(path);
    reset();record("x",1);reply[7]=255;invalid(path);
    reset();record("x",1);reply[9]='x';invalid(path);
    reset();length=65537;invalid(path);
    assert(!__wrap_opendir("/dev/null"));
    char link[256];snprintf(link,sizeof link,"%s-link",path);assert(!symlink(path,link));assert(!__wrap_opendir(link));assert(!unlink(link));assert(!rmdir(path));
    puts("PASS: native directory records, EOF, kernel fallback, mid-stream errors, malformed buffers and link rejection");
}
