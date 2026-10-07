/* OpenSSL uses dladdr only to discover an optional external engine directory.
 * Atmosphere statically includes TLS and ships no dynamically loaded engines.
 * Report no address information instead of using the payload loader resolver.
 */
#include <dlfcn.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
int dladdr(const void *address, Dl_info *info) {
    (void)address;
    if (info) memset(info, 0, sizeof(*info));
    return 0;
}

/* SDK optional interface/terminal and multi-message socket helpers reference
 * generic payload syscalls. Native titles must fail these unsupported routes
 * explicitly instead of issuing an illegal instruction from the PRX. */
long syscall(long number, ...) { (void)number; errno=ENOSYS; return -1; }
long __syscall(long number, ...) { (void)number; errno=ENOSYS; return -1; }
int ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout,
          const sigset_t *mask) {
    if(mask) {errno=ENOTSUP;return -1;}
    int ms=-1;
    if(timeout) {
        if(timeout->tv_sec<0 || timeout->tv_nsec<0 || timeout->tv_nsec>=1000000000) {
            errno=EINVAL;return -1;
        }
        long long value=(long long)timeout->tv_sec*1000+(timeout->tv_nsec+999999)/1000000;
        ms=value>INT_MAX?INT_MAX:(int)value;
    }
    return poll(fds,count,ms);
}
