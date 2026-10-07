#include "atmosphere.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <time.h>
static bool capacity_unavailable(int error) {
#ifdef ATMOSPHERE_NATIVE_APP
    return error==EPERM || error==EACCES || error==ENOSYS || error==ENOTSUP;
#else
    (void)error; return false;
#endif
}
bool storage_has_space(int fd, uint64_t required) {
    struct statvfs fs;
    if(fstatvfs(fd,&fs))return capacity_unavailable(errno);
    uint64_t unit=fs.f_frsize?fs.f_frsize:fs.f_bsize;
    if(!unit){errno=EIO;return false;}
    if(fs.f_flag&ST_RDONLY){errno=EROFS;return false;}
    bool enough=required/unit+(required%unit!=0)<=(uint64_t)fs.f_bavail;
    if(!enough)errno=ENOSPC;
    return enough;
}
#ifdef ATMOSPHERE_NATIVE_APP
#include <pthread.h>
static bool probe_write(const char *root,const struct stat *st) {
    static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    static struct {char root[512];dev_t dev;ino_t ino;time_t checked;} cache[17];
    static unsigned serial;
    time_t now=time(NULL);
    pthread_mutex_lock(&lock);
    int slot=0;
    for(int i=0;i<17;i++) {
        if(!strcmp(cache[i].root,root)) {
            if(cache[i].dev==st->st_dev && cache[i].ino==st->st_ino && now>=cache[i].checked && now-cache[i].checked<60) {
                pthread_mutex_unlock(&lock);return true;
            }
            slot=i;break;
        }
        if(!cache[i].root[0])slot=i;
    }
    char path[640];
    snprintf(path,sizeof path,"%s/.atmosphere-write-test-%ld-%u",root,(long)getpid(),++serial);
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    int error=fd<0?errno:0;
    if(fd>=0) {
        if(write(fd,"x",1)!=1)error=errno?errno:EIO;
        if(close(fd) && !error)error=errno;
        if(unlink(path) && !error)error=errno;
    }
    if(!error){copy_text(cache[slot].root,sizeof cache[slot].root,root);cache[slot].dev=st->st_dev;cache[slot].ino=st->st_ino;cache[slot].checked=now;}
    pthread_mutex_unlock(&lock);
    errno=error;return !error;
}
static void storage_diagnostic(const char *root, const char *stage, int error) {
    static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    static char previous[17][192];
    int slot=16,number;
    if(sscanf(root,"/mnt/usb%d",&number)==1 && number>=0 && number<8)slot=number;
    else if(sscanf(root,"/mnt/ext%d",&number)==1 && number>=0 && number<8)slot=8+number;
    char message[192];snprintf(message,sizeof message,"%s: %s errno=%d (%s)\n",root,stage,error,error?strerror(error):"ok");
    pthread_mutex_lock(&lock);
    if(strcmp(previous[slot],message)) {
        int fd=open("/app0/atmosphere-state/storage-diagnostics.log",O_WRONLY|O_CREAT|O_APPEND,0600);
        if(fd>=0){size_t length=strlen(message);if(write(fd,message,length)==(ssize_t)length)copy_text(previous[slot],sizeof previous[slot],message);close(fd);}
    }
    pthread_mutex_unlock(&lock);
}
#else
#define storage_diagnostic(root,stage,error) ((void)0)
#endif
static bool inspect(Storage *s, const char *id, const char *label, const char *root,
                    bool external) {
    struct stat st, parent;
    struct statvfs fs;
    if(lstat(root,&st)){storage_diagnostic(root,"directory metadata",errno);return false;}
    if(!S_ISDIR(st.st_mode)){storage_diagnostic(root,"not a directory",ENOTDIR);return false;}
    if (external && !atmosphere.desktop) {
        if(stat("/mnt",&parent)){storage_diagnostic(root,"mount parent metadata",errno);return false;}
        if(st.st_dev==parent.st_dev){storage_diagnostic(root,"not a separate mounted device",ENODEV);return false;}
    }
    bool known=statvfs(root,&fs)==0;
    if(!known && !capacity_unavailable(errno)){storage_diagnostic(root,"capacity query",errno);return false;}
    if(known && (fs.f_flag&ST_RDONLY)){storage_diagnostic(root,"read-only filesystem",EROFS);return false;}
#ifdef ATMOSPHERE_NATIVE_APP
    if(!probe_write(root,&st)){storage_diagnostic(root,"write probe",errno);return false;}
#else
    if(access(root,W_OK))return false;
#endif
    memset(s, 0, sizeof *s);
    copy_text(s->id, sizeof s->id, id);
    copy_text(s->label, sizeof s->label, label);
    copy_text(s->root, sizeof s->root, root);
    s->device = st.st_dev;
    s->inode = st.st_ino;
    s->external = external;
    s->capacity_known=known;
    if(known) {
        uint64_t unit = fs.f_frsize ? fs.f_frsize : fs.f_bsize;
        s->free_bytes = (uint64_t)fs.f_bavail * unit;
        s->total_bytes = (uint64_t)fs.f_blocks * unit;
    }
    storage_diagnostic(root,known?"available":"writable; capacity unavailable",0);
    return true;
}
size_t storage_list(Storage *out) {
    size_t n = 0;
    if (atmosphere.desktop) {
        if (atmosphere.desktop_storage[0] &&
            inspect(out, "desktop", "Desktop test storage", atmosphere.desktop_storage, false))
            n++;
        return n;
    }
    for (int i = 0; i < 8; i++) {
        char id[24], label[64], root[64];
        snprintf(id, sizeof id, "usb%d", i);
        snprintf(label, sizeof label, "USB storage %d", i + 1);
        snprintf(root, sizeof root, "/mnt/usb%d", i);
        if (inspect(&out[n], id, label, root, true))
            n++;
    }
    for (int i = 0; i < 8; i++) {
        char id[24], label[64], root[64];
        snprintf(id, sizeof id, "ext%d", i);
        snprintf(label, sizeof label, "External storage %d", i + 1);
        snprintf(root, sizeof root, "/mnt/ext%d", i);
        if (inspect(&out[n], id, label, root, true))
            n++;
    }
    if (inspect(&out[n], "internal", "Internal storage", "/data", false))
        n++;
    return n;
}
bool storage_matches(const Job *j) {
    Storage a[ATMOSPHERE_MAX_STORAGE];
    size_t n = storage_list(a);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(a[i].id, j->storage_id) && !strcmp(a[i].root, j->root) &&
            a[i].device == j->device && a[i].inode == j->inode)
            return true;
    return false;
}
int storage_open(const Job *j) {
    if (!storage_matches(j)) {
        errno = ENODEV;
        return -1;
    }
    int root = open(j->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (root < 0)
        return -1;
    struct stat st;
    if (fstat(root, &st) || st.st_dev != j->device || st.st_ino != j->inode) {
        close(root);
        errno = ENODEV;
        return -1;
    }
    if (mkdirat(root, "homebrew", 0755) && errno != EEXIST) {
        close(root);
        return -1;
    }
    int fd = openat(root, "homebrew", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    close(root);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) || st.st_dev != j->device) {
        close(fd);
        errno = ENODEV;
        return -1;
    }
    return fd;
}
