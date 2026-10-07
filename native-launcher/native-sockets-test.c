#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/socket.h>
static int blocked, mode, calls, option_error;
static int connects;
int __wrap_connect(int,const struct sockaddr *,socklen_t);
int __real_connect(int fd,const struct sockaddr *address,socklen_t length) {
    (void)fd;(void)address;(void)length;connects++;errno=EINPROGRESS;return -1;
}
int __wrap_fcntl(int,int,...);
int __real_fcntl(int fd,int cmd,...) {
    (void)fd;(void)cmd;
    if(blocked){errno=blocked;return -1;}return 23;
}
int getsockopt(int fd,int level,int opt,void *out,socklen_t *size) {
    (void)size;assert(level==SOL_SOCKET);calls++;
    if(fd==8){errno=ENOTSOCK;return -1;}
    *(int *)out=opt==SO_TYPE?SOCK_STREAM:mode;return 0;
}
int setsockopt(int fd,int level,int opt,const void *value,socklen_t size) {
    (void)fd;assert(level==SOL_SOCKET && opt==0x1200 && size==sizeof(int));calls++;
    if(option_error){errno=EIO;return -1;}mode=*(const int *)value;return 0;
}
int main(void) {
    assert(__wrap_fcntl(7,F_GETFL)==23 && calls==0);
    blocked=EBADF;assert(__wrap_fcntl(7,F_GETFL)==-1 && errno==EBADF && calls==0);
    blocked=EINVAL;
    assert(__wrap_fcntl(7,F_GETFL)==O_RDWR);
    assert(__wrap_fcntl(7,F_SETFL,O_RDWR|O_NONBLOCK)==0 && mode==1);
    assert(__wrap_fcntl(7,F_GETFL)==(O_RDWR|O_NONBLOCK));
    assert(__wrap_fcntl(7,F_SETFL,O_RDWR)==0 && mode==0);
    assert(__wrap_fcntl(7,F_SETFD,FD_CLOEXEC)==0);
    assert(__wrap_fcntl(8,F_GETFL)==-1 && errno==EINVAL);
    option_error=1;assert(__wrap_fcntl(7,F_SETFL,O_NONBLOCK)==-1 && errno==EIO);
    assert(__wrap_connect(7,NULL,0)==-1 && connects==0 && errno==EIO);
    option_error=0;assert(__wrap_connect(7,NULL,0)==-1 && connects==1 && mode==1 && errno==EINPROGRESS);
    puts("PASS: native socket nonblocking fallback, file passthrough and error preservation");
}
