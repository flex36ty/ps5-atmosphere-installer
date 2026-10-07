#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char root[]="/tmp/atmosphere-files-XXXXXX";
    assert(mkdtemp(root));
    int dir=open(root,O_RDONLY|O_DIRECTORY);assert(dir>=0);
    assert(mkdirat(dir,"stage",0700)==0);
    int stage=openat(dir,"stage",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);assert(stage>=0);
    int copy=dup(stage);assert(copy>=0);
    int fd=openat(copy,"part",O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW,0600);assert(fd>=0);
    assert(write(fd,"verified",8)==8);close(fd);
    assert(renameat(stage,"part",dir,"game")==0);
    struct stat st;assert(fstatat(dir,"game",&st,AT_SYMLINK_NOFOLLOW)==0 && st.st_size==8);
    fd=openat(dir,"game",O_RDONLY|O_NOFOLLOW);assert(fd>=0);
    char data[9]={0};assert(read(fd,data,8)==8 && !strcmp(data,"verified"));close(fd);
    char sym[4096];snprintf(sym,sizeof sym,"%s/symlink",root);assert(symlink("game",sym)==0);
    assert(openat(dir,"symlink",O_RDONLY|O_NOFOLLOW)<0 && errno==ELOOP);
    assert(fstatat(dir,"symlink",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ELOOP);
    assert(lstat(root,&st)==0 && S_ISDIR(st.st_mode));
    assert(lstat(sym,&st)<0 && errno==ELOOP);
    assert(fstatat(dir,"missing",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT);
    // A renamed/replaced directory must not send writes to the replacement.
    assert(renameat(dir,"stage",dir,"old-stage")==0);
    assert(mkdirat(dir,"stage",0700)==0);
    assert(openat(stage,"wrong",O_CREAT|O_WRONLY,0600)<0 && errno==ESTALE);
    close(copy);close(stage);
    // Closed descriptor records are rejected even when the number is reused.
    int stale=openat(dir,"stage",O_RDONLY|O_DIRECTORY);assert(stale>=0);close(stale);
    assert(openat(stale,"wrong",O_CREAT|O_WRONLY,0600)<0 && errno==EBADF);
    assert(access(root,F_OK)==0);
    assert(unlinkat(dir,"game",0)==0);assert(unlinkat(dir,"symlink",0)==0);
    assert(unlinkat(dir,"stage",AT_REMOVEDIR)==0);assert(unlinkat(dir,"old-stage",AT_REMOVEDIR)==0);
    close(dir);assert(rmdir(root)==0);
    puts("PASS: native file creation, publication, directory duplication, symlink refusal and stale descriptor rejection");
}
