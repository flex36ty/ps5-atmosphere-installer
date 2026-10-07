/* Native storage capacity adapter. Avoid the incompatible libc statvfs entry. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/statvfs.h>
#ifdef ATMOSPHERE_VFS_TEST
#include <sys/vfs.h>
#else
#include <sys/param.h>
#include <sys/mount.h>
#endif
#include <unistd.h>

/* The native kernel exports this descriptor-based filesystem query. */
extern int _fstatfs(int,struct statfs *);
int __wrap_fstatvfs(int fd,struct statvfs *out) {
    if(!out){errno=EINVAL;return -1;}
    struct statfs fs;
    memset(&fs,0,sizeof fs);
    if(_fstatfs(fd,&fs))return -1;
    if(fs.f_bsize<=0){errno=EIO;return -1;}
    memset(out,0,sizeof *out);
    out->f_bsize=out->f_frsize=(unsigned long)fs.f_bsize;
    out->f_blocks=fs.f_blocks;out->f_bfree=fs.f_bfree;
    out->f_bavail=(int64_t)fs.f_bavail<0?0:fs.f_bavail;
    out->f_files=fs.f_files;
    out->f_ffree=out->f_favail=(int64_t)fs.f_ffree<0?0:fs.f_ffree;
    out->f_flag=fs.f_flags & (ST_RDONLY|ST_NOSUID);
    memcpy(&out->f_fsid,&fs.f_fsid,sizeof(out->f_fsid)<sizeof(fs.f_fsid)?sizeof(out->f_fsid):sizeof(fs.f_fsid));
#ifdef ATMOSPHERE_VFS_TEST
    out->f_namemax=fs.f_namelen;
#else
    out->f_namemax=fs.f_namemax;
#endif
    return 0;
}
int __wrap_statvfs(const char *path,struct statvfs *out) {
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(fd<0)return -1;
    int rc=__wrap_fstatvfs(fd,out),saved=errno;
    close(fd);errno=saved;return rc;
}
