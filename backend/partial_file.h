#ifndef ATMOSPHERE_PARTIAL_FILE_H
#define ATMOSPHERE_PARTIAL_FILE_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
static inline bool partial_file_valid(const struct stat *file, const struct stat *path,
                                      dev_t device, uint64_t source_size) {
    if (!S_ISREG(file->st_mode) || file->st_dev != device || file->st_size < 0 ||
        (uint64_t)file->st_size > source_size) return false;
    if (file->st_nlink == 1) return true;
#ifdef ATMOSPHERE_NATIVE_APP
    /* The PS5 USB driver reports zero links even for a named regular file.
     * Require a fresh no-follow path lookup matching the open descriptor. */
    return file->st_nlink == 0 && path && S_ISREG(path->st_mode) &&
        path->st_nlink == 0 && path->st_dev == file->st_dev &&
        path->st_ino == file->st_ino && path->st_size == file->st_size;
#else
    (void)path;
    return false;
#endif
}
#endif
