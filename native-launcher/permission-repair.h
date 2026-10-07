#ifndef ATMOSPHERE_PERMISSION_REPAIR_H
#define ATMOSPHERE_PERMISSION_REPAIR_H
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Fixed, non-recursive allowlist. Never follow a link or cross a mount. */
static int repair_directory(int root, const char *name) {
    if (strcmp(name,"homebrew") && strcmp(name,".atmosphere-smb-staging") &&
        strcmp(name,".orbit-smb-staging")) { errno=EINVAL; return -1; }
    struct stat parent, child;
    if (fstat(root,&parent)) return -1;
    int fd=openat(root,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(fd<0)return -1;
    int rc=fstat(fd,&child);
    if(!rc && (!S_ISDIR(child.st_mode)||child.st_dev!=parent.st_dev)) {errno=EXDEV;rc=-1;}
    if(!rc)rc=fchmod(fd,(child.st_mode&07777)|0777);
    int saved=errno;close(fd);errno=saved;return rc;
}
#endif
