#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
int __wrap_statvfs(const char *,struct statvfs *);
int __wrap_fstatvfs(int,struct statvfs *);
static int failure,negative,zero;
int _fstatfs(int fd,struct statfs *out){
 assert(fd>=0);if(failure){errno=EACCES;return -1;}
 memset(out,0,sizeof *out);out->f_bsize=zero?0:262144;out->f_blocks=3906916;
 out->f_bfree=424348;out->f_bavail=negative?-1:424348;out->f_flags=ST_RDONLY;out->f_namelen=255;return 0;
}
int main(void){
 struct statvfs fs;
 assert(__wrap_statvfs(".",&fs)==0);
 assert(fs.f_bavail*fs.f_frsize==111240282112ULL && fs.f_flag==ST_RDONLY && fs.f_namemax==255);
 negative=1;assert(__wrap_fstatvfs(7,&fs)==0 && fs.f_bavail==0);negative=0;
 failure=1;assert(__wrap_statvfs(".",&fs)==-1 && errno==EACCES);failure=0;
 zero=1;assert(__wrap_fstatvfs(7,&fs)==-1 && errno==EIO);
 assert(__wrap_fstatvfs(7,NULL)==-1 && errno==EINVAL);
 assert(__wrap_statvfs("/atmosphere-test-nonexistent-dir",&fs)==-1);
 puts("PASS: native capacity conversion, read-only flags, negative space and query failures");
}
