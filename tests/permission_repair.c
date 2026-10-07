#define _GNU_SOURCE
#include "../native-launcher/permission-repair.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int main(void){
    char temp[]="/tmp/atmosphere-permissions-XXXXXX";assert(mkdtemp(temp));
    int root=open(temp,O_RDONLY|O_DIRECTORY);assert(root>=0);
    assert(!mkdirat(root,"homebrew",0700));
    int file=openat(root,"homebrew/game",O_WRONLY|O_CREAT|O_EXCL,0600);assert(file>=0);assert(write(file,"keep",4)==4);close(file);
    assert(!repair_directory(root,"homebrew"));struct stat st;
    assert(!fstatat(root,"homebrew",&st,0));assert((st.st_mode&0777)==0777);
    assert(!fstatat(root,"homebrew/game",&st,0));assert((st.st_mode&0777)==0600 && st.st_size==4);
    assert(!repair_directory(root,"homebrew"));
    assert(repair_directory(root,"../homebrew")<0 && errno==EINVAL);
    assert(repair_directory(root,"unrelated")<0 && errno==EINVAL);
    assert(!symlinkat("homebrew",root,".atmosphere-smb-staging"));
    assert(repair_directory(root,".atmosphere-smb-staging")<0);
    assert(!unlinkat(root,".atmosphere-smb-staging",0));
    assert(!mkdirat(root,".atmosphere-smb-staging",0700));assert(!repair_directory(root,".atmosphere-smb-staging"));
    assert(!fstatat(root,".atmosphere-smb-staging",&st,0));assert((st.st_mode&0777)==0777);
    assert(!unlinkat(root,"homebrew/game",0));assert(!unlinkat(root,"homebrew",AT_REMOVEDIR));
    assert(!unlinkat(root,".atmosphere-smb-staging",AT_REMOVEDIR));close(root);assert(!rmdir(temp));
    puts("PASS: fixed directory allowlist, symlink rejection, idempotence, staging repair and file preservation");
}
