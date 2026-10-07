/* Native PS5 titles reject socket fcntl with EINVAL. Translate socket flag
 * operations to SO_NBIO (0x1200), as documented by the native-app boilerplate:
 * https://github.com/blackbearreloaded/ps5-native-app-boilerplate/blob/main/examples/update-check/console_curl.c
 * Normal file-descriptor operations still go through libkernel.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/socket.h>
int __real_fcntl(int fd,int command,...);
int __real_connect(int fd,const struct sockaddr *address,socklen_t length);
int __wrap_connect(int fd,const struct sockaddr *address,socklen_t length) {
    // libsmb2 ignores failures from its fcntl setup. Fail closed here rather
    // than enter a blocking connect and lose timeout/cancel processing.
    int enabled=1;
    if(setsockopt(fd,SOL_SOCKET,0x1200,&enabled,sizeof(enabled))<0)return -1;
    return __real_connect(fd,address,length);
}
int __wrap_fcntl(int fd,int command,...) {
    int value=0, result;
    va_list ap;va_start(ap,command);
    switch(command) {
        case F_GETFL:case F_GETFD:
            result=__real_fcntl(fd,command);break;
        case F_SETFL:case F_SETFD:case F_DUPFD:
            value=va_arg(ap,int);result=__real_fcntl(fd,command,value);break;
        default:
            result=__real_fcntl(fd,command,va_arg(ap,void *));break;
    }
    va_end(ap);
    if(result>=0 || errno!=EINVAL)return result;
    if(command!=F_GETFL && command!=F_SETFL && command!=F_GETFD && command!=F_SETFD)return result;
    int type=0;socklen_t size=sizeof(type);
    if(getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size)<0){errno=EINVAL;return -1;}
    if(command==F_GETFD || command==F_SETFD)return 0; // Native backend never execs.
    int nonblocking=(value&O_NONBLOCK)!=0;
    if(command==F_SETFL)return setsockopt(fd,SOL_SOCKET,0x1200,&nonblocking,sizeof(nonblocking));
    size=sizeof(nonblocking);
    if(getsockopt(fd,SOL_SOCKET,0x1200,&nonblocking,&size)<0)return -1;
    return O_RDWR|(nonblocking?O_NONBLOCK:0);
}
