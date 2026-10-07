#include <assert.h>
#include <dirent.h>
#include <stdlib.h>
#include "../backend/storage.c"
static int failure;
static struct statvfs capacity;
int fstatvfs(int fd,struct statvfs *out) {
    (void)fd;
    if(failure){errno=failure;return -1;}
    *out=capacity;return 0;
}
void copy_text(char *dst,size_t size,const char *src){snprintf(dst,size,"%s",src);}
int main(void) {
    capacity.f_frsize=4096;capacity.f_bavail=2;
    assert(storage_has_space(0,8192));
    assert(!storage_has_space(0,8193) && errno==ENOSPC);
    assert(!storage_has_space(0,UINT64_MAX));
    capacity.f_flag=ST_RDONLY;
    assert(!storage_has_space(0,1) && errno==EROFS);
    failure=EPERM;assert(storage_has_space(0,8192));
    failure=EACCES;assert(storage_has_space(0,8192));
    failure=EIO;assert(!storage_has_space(0,8192));
    failure=ENODEV;assert(!storage_has_space(0,8192));
    char path[]="/tmp/atmosphere-probe-XXXXXX";
    assert(mkdtemp(path));struct stat st;assert(!stat(path,&st));
    assert(probe_write(path,&st));assert(probe_write(path,&st));
    DIR *dir=opendir(path);assert(dir);struct dirent *entry;
    while((entry=readdir(dir)))assert(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."));
    closedir(dir);assert(!rmdir(path));
    puts("Storage capacity policy and write-probe cleanup passed.");
}
