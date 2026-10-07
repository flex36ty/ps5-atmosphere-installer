#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#endif
#include "install.h"
#include "launcher.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const unsigned char owner[] = "Atmosphere ORBT00001 launcher v1\n";
static const char *title_id = "ORBT00001";
static char last_error[192];

const char *atmosphere_launcher_last_error(void) {
    return last_error;
}

void atmosphere_launcher_record_error(const char *step, int system_error, int platform_error) {
    snprintf(last_error, sizeof last_error, "%s: errno=%d, platform=0x%08x", step, system_error,
             (unsigned)platform_error);
}

/* 1 matches, 0 missing, -1 unsafe/different/unreadable. Never follow symlinks. */
static int matches(int parent, const char *name, const unsigned char *data, size_t length) {
    int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    struct stat st;
    int result = -1;
    if (fstat(fd, &st))
        goto done;
    if (!S_ISREG(st.st_mode) || st.st_size != (off_t)length) {
        errno = EINVAL;
        goto done;
    }
    unsigned char buffer[512];
    size_t offset = 0;
    while (offset < length) {
        size_t wanted = length - offset;
        if (wanted > sizeof buffer)
            wanted = sizeof buffer;
        ssize_t n = read(fd, buffer, wanted);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            goto done;
        if (!n || memcmp(buffer, data + offset, (size_t)n)) {
            errno = EINVAL;
            goto done;
        }
        offset += (size_t)n;
    }
    result = 1;
done: {
    int saved_error = errno;
    close(fd);
    errno = saved_error;
}
    return result;
}

static int ensure_file(int parent, const char *name, const unsigned char *data, size_t length) {
    int existing = matches(parent, name, data, length);
    if (existing)
        return existing == 1 ? 0 : -1;
    int fd = openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0)
        return -1;
    size_t offset = 0;
    int result = -1;
    while (offset < length) {
        ssize_t n = write(fd, data + offset, length - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = EIO;
            goto done;
        }
        offset += (size_t)n;
    }
    result = fsync(fd);
done: {
    int saved_error = errno;
    if (close(fd) && result == 0)
        result = -1;
    else
        errno = saved_error;
}
    /* A partial write is preserved and rejected on retry, never overwritten. */
    return result;
}

/* Replaces an owned regular file (or creates it) with a complete copy, never through a symlink. */
static int replace_file(int parent, const char *name, const unsigned char *data, size_t length) {
    struct stat st;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    char temp[64];
    snprintf(temp, sizeof temp, "%s.atmosphere-new", name);
    unlinkat(parent, temp, 0);
    if (ensure_file(parent, temp, data, length))
        return -1;
    if (renameat(parent, temp, parent, name)) {
        unlinkat(parent, temp, 0);
        return -1;
    }
    return 0;
}

/* Writes a small state record only when its content changes. */
static int record(int parent, const char *name, const char *text) {
    int current = matches(parent, name, (const unsigned char *)text, strlen(text));
    return current == 1 ? 0 : replace_file(parent, name, (const unsigned char *)text, strlen(text));
}

/* Identifies this build's manifest and icon, so a newer build can update an installed icon. */
static void fingerprint(char out[18]) {
    uint64_t hash = 1469598103934665603ULL;
    const unsigned char *parts[] = {launcher_manifest, launcher_icon};
    size_t lengths[] = {sizeof launcher_manifest, sizeof launcher_icon};
    for (size_t p = 0; p < 2; p++)
        for (size_t i = 0; i < lengths[p]; i++)
            hash = (hash ^ parts[p][i]) * 1099511628211ULL;
    snprintf(out, 18, "%016llx\n", (unsigned long long)hash);
}

static int open_dir(int parent, const char *name, bool create) {
    if (create && mkdirat(parent, name, 0755) && errno != EEXIST)
        return -1;
    return openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
}

static int absent(int parent, const char *name) {
    struct stat st;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) == 0)
        return 0;
    return errno == ENOENT ? 1 : -1;
}

int atmosphere_launcher_ensure_at(int state_fd, const char *user_parent, int (*prepare)(void),
                             int (*register_title)(void)) {
    int up = -1, user = -1, us = -1, result = -1;
    const char *step = "open-app-parent";
    last_error[0] = '\0';
    errno = 0;
    up = open(user_parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (up < 0)
        goto done;
    step = "verify-ownership-record";
    int owned = matches(state_fd, "launcher-owned-v1", owner, sizeof owner - 1);
    if (owned < 0)
        goto done;
    step = "check-existing-title";
    int missing = absent(up, title_id);
    if (missing < 0)
        goto done;
    if (!owned && !missing) {
        errno = EEXIST;
        goto done;
    }
    step = "verify-ready-record";
    int ready = matches(state_fd, "launcher-ready-v1", owner, sizeof owner - 1);
    if (ready < 0)
        goto done;
    if (ready && !owned) {
        errno = EINVAL;
        goto done;
    }
    if (ready && missing) {
        /* Removing the home-screen app leaves Atmosphere's state behind. Invalidate
         * its old registration before rebuilding, so a failed repair retries. */
        step = "clear-deleted-launcher-record";
        if (unlinkat(state_fd, "launcher-ready-v1", 0))
            goto done;
        step = "sync-deleted-launcher-record";
        if (fsync(state_fd))
            goto done;
        ready = 0;
    }
    if (!ready) {
        step = "prepare-app-registration";
        errno = 0;
        if (prepare())
            goto done;
        step = "write-ownership-record";
        if (ensure_file(state_fd, "launcher-owned-v1", owner, sizeof owner - 1))
            goto done;
        step = "sync-ownership-directory";
        if (fsync(state_fd))
            goto done;
    }
    step = "open-title-directory";
    user = open_dir(up, title_id, !ready);
    if (user < 0)
        goto done;
    step = "open-sce-sys-directory";
    us = open_dir(user, "sce_sys", !ready);
    if (us < 0)
        goto done;
    struct {
        int fd;
        const char *name;
        const unsigned char *data;
        size_t length;
    } files[] = {{us, "param.json", launcher_manifest, sizeof launcher_manifest},
                 {us, "icon0.png", launcher_icon, sizeof launcher_icon}};
    char build[18];
    fingerprint(build);
    step = "verify-files-record";
    int current =
        ready ? matches(state_fd, "launcher-files-v1", (const unsigned char *)build, strlen(build))
              : 1;
    if (ready && current != 1) {
        /* Installed by an older build: replace the owned files, then register the icon again.
         * The record is written last, so a failed update is retried on the next start. */
        step = "prepare-app-update";
        errno = 0;
        if (prepare())
            goto done;
        for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
            step = i ? "update-icon" : "update-manifest";
            if (matches(files[i].fd, files[i].name, files[i].data, files[i].length) != 1 &&
                replace_file(files[i].fd, files[i].name, files[i].data, files[i].length))
                goto done;
        }
        step = "register-updated-title";
        errno = 0;
        if (register_title())
            goto done;
        step = "write-files-record";
        if (record(state_fd, "launcher-files-v1", build) || fsync(state_fd))
            goto done;
        result = 1;
        goto done;
    }
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        step = i ? "verify-or-write-icon" : "verify-or-write-manifest";
        if (ready ? matches(files[i].fd, files[i].name, files[i].data, files[i].length) != 1
                  : ensure_file(files[i].fd, files[i].name, files[i].data, files[i].length)) {
            puts("Atmosphere launcher: app files differ, are missing, or cannot be written; preserved.");
            goto done;
        }
    }
    if (ready) {
        result = 0;
        goto done;
    }
    step = "register-title";
    errno = 0;
    if (register_title())
        goto done;
    step = "write-ready-record";
    if (ensure_file(state_fd, "launcher-ready-v1", owner, sizeof owner - 1))
        goto done;
    step = "write-files-record";
    if (record(state_fd, "launcher-files-v1", build))
        goto done;
    step = "sync-ready-directory";
    if (fsync(state_fd))
        goto done;
    result = 1;
done:
    if (result < 0 && !last_error[0])
        atmosphere_launcher_record_error(step, errno, 0);
    if (us >= 0)
        close(us);
    if (user >= 0)
        close(user);
    if (up >= 0)
        close(up);
    if (result < 0)
        printf("Atmosphere launcher is not ready: %s. Server remains available.\n", last_error);
    return result;
}
