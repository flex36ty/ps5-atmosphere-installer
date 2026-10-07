/* Native-title *at compatibility: use exported filesystem functions, never
 * payload syscalls. Directory descriptors retain verified absolute paths.
 * Like PS5_PayloadSDK's directory adapter, this cannot provide kernel-atomic
 * path traversal against concurrent external renames. Reject changed parents.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_CAP 4096
#define FD_CAP 256
static struct { int fd; dev_t dev; ino_t ino; char path[PATH_CAP]; bool used; } dirs[FD_CAP];
static pthread_mutex_t dir_lock = PTHREAD_MUTEX_INITIALIZER;
static char file_error[160];
const char *atmosphere_file_error(void) {return file_error;}
static int file_failure(const char *step,const char *path,int flags) {
    int saved=errno;
    snprintf(file_error,sizeof file_error,"%s %.85s flags=0x%x errno=%d",step,path?path:"",flags,saved);
    errno=saved;return -1;
}
int __real_open(const char *, int, ...);
int __real_close(int);

static int remember(int fd, const char *path) {
    struct stat st;
    if (fstat(fd, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) return 0;
    if (path[0] != '/' || strlen(path) >= PATH_CAP) { errno=ENAMETOOLONG; return -1; }
    pthread_mutex_lock(&dir_lock);
    int slot=-1;
    for (int i=0;i<FD_CAP;i++) {
        if (dirs[i].used && dirs[i].fd==fd) { slot=i; break; }
        if (!dirs[i].used && slot<0) slot=i;
    }
    if (slot>=0) {
        dirs[slot].used=true; dirs[slot].fd=fd; dirs[slot].dev=st.st_dev; dirs[slot].ino=st.st_ino;
        strcpy(dirs[slot].path,path);
    }
    pthread_mutex_unlock(&dir_lock);
    if (slot<0) { errno=EMFILE; return -1; }
    return 0;
}
static int resolve(int fd,const char *name,char out[PATH_CAP]) {
    if (!name) { errno=EFAULT; return -1; }
    if (!*name) { errno=ENOENT; return -1; }
    if (*name=='/') {
        if (strlen(name)>=PATH_CAP) {errno=ENAMETOOLONG;return -1;}
        strcpy(out,name); return 0;
    }
    // Backend paths are absolute or relative to tracked directories, never cwd.
    struct stat st, path_st;
    if (fstat(fd,&st)) return file_failure("resolve fstat",name,0);
    int found=0;
    pthread_mutex_lock(&dir_lock);
    for(int i=0;i<FD_CAP;i++) if(dirs[i].used && dirs[i].fd==fd &&
        dirs[i].dev==st.st_dev && dirs[i].ino==st.st_ino) {
        strcpy(out,dirs[i].path); found=1; break;
    }
    pthread_mutex_unlock(&dir_lock);
    if(!found) {errno=EBADF;return -1;}
    // Native titles can reject lstat even for their own mounted app folder.
    // Open without following a final symlink, then compare descriptor identity.
    int check=__real_open(out,O_RDONLY|O_DIRECTORY|O_NOFOLLOW,0);
    if(check<0) return file_failure("resolve directory open",out,O_DIRECTORY|O_NOFOLLOW);
    int result=fstat(check,&path_st), saved=errno;
    __real_close(check);
    errno=saved;
    if(result) return file_failure("resolve directory fstat",out,0);
    if(!S_ISDIR(path_st.st_mode) || path_st.st_dev!=st.st_dev || path_st.st_ino!=st.st_ino) {
        errno=ESTALE;return -1;
    }
    size_t n=strlen(out), m=strlen(name);
    if(n+m+2>PATH_CAP) {errno=ENAMETOOLONG;return -1;}
    out[n++]='/';memcpy(out+n,name,m+1); return 0;
}
int __wrap_open(const char *path,int flags,...) {
    int mode=0;
    if(flags&O_CREAT) {va_list ap;va_start(ap,flags);mode=va_arg(ap,int);va_end(ap);}
    int fd=__real_open(path,flags,mode);
    if(fd<0)return file_failure("native open",path,flags);
    if(remember(fd,path)) {int e=errno;__real_close(fd);errno=e;return file_failure("track fstat",path,flags);}
    return fd;
}
int __wrap_close(int fd) {
    pthread_mutex_lock(&dir_lock);
    for(int i=0;i<FD_CAP;i++) if(dirs[i].used && dirs[i].fd==fd) dirs[i].used=false;
    pthread_mutex_unlock(&dir_lock);
    return __real_close(fd);
}
int __wrap_dup(int fd) {
    // Only directory duplication is used by the native transfer worker.
    char path[PATH_CAP];
    if(resolve(fd,".",path)) return -1;
    return __wrap_open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
}
int __wrap_openat(int fd,const char *name,int flags,...) {
    file_error[0]=0;
    int mode=0;
    if(flags&O_CREAT) {va_list ap;va_start(ap,flags);mode=va_arg(ap,int);va_end(ap);}
    char path[PATH_CAP];if(resolve(fd,name,path))return -1;
    return __wrap_open(path,flags,mode);
}
int __wrap_mkdirat(int fd,const char *name,mode_t mode) {
    char path[PATH_CAP];if(resolve(fd,name,path))return -1;return mkdir(path,mode);
}
int __wrap_lstat(const char *path,struct stat *st) {
    // This adapter deliberately refuses symlinks instead of reporting their
    // metadata. All native callers require real directories or regular files.
    int fd=__real_open(path,O_RDONLY|O_NONBLOCK|O_NOFOLLOW,0);
    if(fd<0)return -1;
    int result=fstat(fd,st),saved=errno;
    __real_close(fd);errno=saved;return result;
}
int __wrap_fstatat(int fd,const char *name,struct stat *st,int flags) {
    if(flags & ~AT_SYMLINK_NOFOLLOW) {errno=EINVAL;return -1;}
    char path[PATH_CAP];if(resolve(fd,name,path))return -1;
    return flags&AT_SYMLINK_NOFOLLOW ? __wrap_lstat(path,st) : stat(path,st);
}
int __wrap_unlinkat(int fd,const char *name,int flags) {
    if(flags & ~AT_REMOVEDIR) {errno=EINVAL;return -1;}
    char path[PATH_CAP];if(resolve(fd,name,path))return -1;
    return flags&AT_REMOVEDIR ? rmdir(path) : unlink(path);
}
int __wrap_renameat(int a,const char *src,int b,const char *dst) {
    char x[PATH_CAP],y[PATH_CAP];if(resolve(a,src,x)||resolve(b,dst,y))return -1;
    return rename(x,y);
}
int __wrap_linkat(int a,const char *src,int b,const char *dst,int flags) {
    // Native SMB publication uses rename; legacy hard-link publication is unsupported.
    (void)a;(void)src;(void)b;(void)dst;(void)flags;errno=ENOTSUP;return -1;
}
int __wrap_access(const char *path,int mode) {
    struct stat st;
    if(mode & ~(R_OK|W_OK|X_OK)) {errno=EINVAL;return -1;}
    if(stat(path,&st))return -1;
    if(mode==F_OK)return 0;
    if((mode&X_OK) && !(st.st_mode&0111)) {errno=EACCES;return -1;}
    if(S_ISDIR(st.st_mode)) {
        // Permission bits are preliminary; creation is verified at transfer time.
        if((mode&W_OK) && !(st.st_mode&0222)) {errno=EACCES;return -1;}
        return 0;
    }
    int fd=__real_open(path,(mode&W_OK)?((mode&R_OK)?O_RDWR:O_WRONLY):O_RDONLY,0);
    if(fd<0)return -1;return __real_close(fd);
}
