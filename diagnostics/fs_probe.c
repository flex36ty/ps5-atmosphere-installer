/* Console regression checks. Invalid directory descriptors cannot mutate files. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

void atmosphere_trace(const char *);
void atmosphere_trace_number(const char *, long);

static int failed_as_expected(const char *name, int result) {
    int error = errno;
    atmosphere_trace_number(name, result);
    atmosphere_trace_number("filesystem probe errno", error);
    return result != -1 || error != EBADF;
}

int atmosphere_fs_probe(int directory) {
    struct stat st;
    int failed = 0;
    failed |= failed_as_expected("openat invalid fd", openat(-1, "unused", O_RDONLY));
    failed |= failed_as_expected("mkdirat invalid fd", mkdirat(-1, "unused", 0700));
    failed |= failed_as_expected("fstatat invalid fd", fstatat(-1, "unused", &st, 0));
    failed |= failed_as_expected("linkat invalid fd", linkat(-1, "unused", -1, "unused", 0));
    failed |= failed_as_expected("renameat invalid fd", renameat(-1, "unused", -1, "unused"));
    failed |= failed_as_expected("unlinkat invalid fd", unlinkat(-1, "unused", 0));
    int fd = openat(directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) {
        failed = 1;
    } else {
        if (fstat(fd, &st) || !S_ISDIR(st.st_mode))
            failed = 1;
        close(fd);
    }
    if (fstatat(directory, ".", &st, AT_SYMLINK_NOFOLLOW) || !S_ISDIR(st.st_mode))
        failed = 1;
    int missing = openat(directory, "", O_RDONLY | O_NOFOLLOW);
    if (missing != -1 || errno != ENOENT)
        failed = 1;
    atmosphere_trace(failed ? "filesystem probe FAILED" : "filesystem probe PASSED");
    return failed;
}
