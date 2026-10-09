/* Native libc opendir enters a restricted syscall path in a title process.
 * Open through the existing file adapter and enumerate through libkernel. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
extern int sceKernelGetdents(int,void *,int);
extern int sceKernelGetdirentries(int,void *,int,long *);
typedef struct {
    int fd,error;
    size_t offset,length;
    int alternate,delivered;
    long position;
    struct dirent entry;
    unsigned char buffer[65536];
} NativeDirectory;
static int kernel_errno(int rc){
    uint32_t value=(uint32_t)rc;
    return (value&0xffff0000U)==0x80020000U?(int)(value&0xffffU):rc==-1?(errno?errno:EIO):EIO;
}
DIR *__wrap_opendir(const char *path){
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NONBLOCK|O_NOFOLLOW);if(fd<0)return NULL;
    struct stat st;if(fstat(fd,&st)||!S_ISDIR(st.st_mode)){int error=errno?errno:ENOTDIR;close(fd);errno=error;return NULL;}
    NativeDirectory *dir=calloc(1,sizeof *dir);if(!dir){close(fd);errno=ENOMEM;return NULL;}
    dir->fd=fd;return (DIR*)dir;
}
struct dirent *__wrap_readdir(DIR *opaque){
    NativeDirectory *dir=(NativeDirectory*)opaque;if(!dir){errno=EBADF;return NULL;}
    if(dir->error){errno=dir->error;return NULL;}
    for(;;){
        if(dir->offset==dir->length){
            int count=dir->alternate?sceKernelGetdirentries(dir->fd,dir->buffer,sizeof dir->buffer,&dir->position):sceKernelGetdents(dir->fd,dir->buffer,sizeof dir->buffer);
            if(count<0&&!dir->alternate&&!dir->delivered&&lseek(dir->fd,0,SEEK_SET)>=0){
                dir->alternate=1;count=sceKernelGetdirentries(dir->fd,dir->buffer,sizeof dir->buffer,&dir->position);
            }
            if(count<0){dir->error=kernel_errno(count);errno=dir->error;return NULL;}
            if(!count){errno=0;return NULL;}
            if(count>(int)sizeof dir->buffer)goto malformed;
            dir->offset=0;dir->length=(size_t)count;
        }
        size_t left=dir->length-dir->offset;const unsigned char *record=dir->buffer+dir->offset;
        if(left<8)goto malformed;
        unsigned length=(unsigned)record[4]|(unsigned)record[5]<<8,name=record[7];
        if(length<8||length>left)goto malformed;
        dir->offset+=length;
        uint32_t inode;memcpy(&inode,record,4);if(!inode)continue;
        if(name==0||name+8>=length||record[8+name]!=0||memchr(record+8,0,name))goto malformed;
        memset(&dir->entry,0,sizeof dir->entry);dir->entry.d_fileno=inode;dir->entry.d_type=record[6];
        dir->entry.d_reclen=sizeof dir->entry;
#ifndef ATMOSPHERE_DIRECTORY_TEST
        dir->entry.d_namlen=(unsigned char)name;
#endif
        memcpy(dir->entry.d_name,record+8,name);dir->entry.d_name[name]=0;dir->delivered=1;errno=0;return &dir->entry;
    }
malformed:
    dir->error=EIO;errno=EIO;return NULL;
}
int __wrap_closedir(DIR *opaque){
    NativeDirectory *dir=(NativeDirectory*)opaque;if(!dir){errno=EBADF;return -1;}
    int rc=close(dir->fd),error=errno;free(dir);errno=error;return rc;
}
