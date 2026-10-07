#include <assert.h>
#include <stdio.h>
#include "../backend/partial_file.h"
int main(void) {
    struct stat file={0};
    file.st_mode=S_IFREG|0600;file.st_nlink=1;file.st_dev=7;file.st_ino=10;file.st_size=5;
    assert(partial_file_valid(&file,NULL,7,5));
    assert(!partial_file_valid(&file,NULL,8,5));
    assert(!partial_file_valid(&file,NULL,7,4));
    file.st_nlink=2;assert(!partial_file_valid(&file,&file,7,5));
    file.st_nlink=0;
    assert(!partial_file_valid(&file,NULL,7,5));
    struct stat path=file;
#ifdef ATMOSPHERE_NATIVE_APP
    assert(partial_file_valid(&file,&path,7,5));
#else
    assert(!partial_file_valid(&file,&path,7,5));
#endif
    path.st_ino++;assert(!partial_file_valid(&file,&path,7,5));
    path=file;path.st_dev++;assert(!partial_file_valid(&file,&path,7,5));
    path=file;path.st_mode=S_IFLNK|0777;assert(!partial_file_valid(&file,&path,7,5));
    path=file;path.st_nlink=2;assert(!partial_file_valid(&file,&path,7,5));
    file.st_size=-1;assert(!partial_file_valid(&file,&file,7,5));
    puts("Partial-file safety checks passed.");
}
