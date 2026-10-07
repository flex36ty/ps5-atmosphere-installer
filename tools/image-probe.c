/* Probe local or guest SMB images with the exact reader used by Atmosphere. */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include "image_metadata.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
typedef struct { struct smb2_context *s; struct smb2fh *f; int fd; } Input;
static int read_at(void *context, uint64_t offset, void *out, size_t length) {
    Input *in = context; unsigned char *data = out;
    while (length) {
        unsigned want = length > 65536 ? 65536 : (unsigned)length;
        ssize_t n = in->s ? smb2_pread(in->s, in->f, data, want, offset) : pread(in->fd, data, want, (off_t)offset);
        if (n <= 0) return -1;
        length -= (size_t)n; offset += (uint64_t)n; data += n;
    }
    return 0;
}
static int save(const char *prefix, const char *extension, const void *data, size_t size) {
    if (!data) return 0;
    char path[1024]; if (snprintf(path, sizeof path, "%s.%s", prefix, extension) >= (int)sizeof path) return -1;
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    int rc = fwrite(data, 1, size, f) == size ? 0 : -1;
    if (fclose(f)) rc = -1; return rc;
}
int main(int argc, char **argv) {
    Input in = {.fd = -1}; uint64_t size; const char *output;
    if (argc == 4 && !strcmp(argv[1], "--local")) {
        in.fd = open(argv[2], O_RDONLY); struct stat st;
        if (in.fd < 0 || fstat(in.fd, &st)) return 2;
        size = (uint64_t)st.st_size; output = argv[3];
    } else if (argc == 5) {
        in.s = smb2_init_context(); if (!in.s) return 2;
        smb2_set_timeout(in.s, 15); smb2_set_version(in.s, SMB2_VERSION_ANY2);
        smb2_set_user(in.s, ""); smb2_set_password(in.s, "");
        if (smb2_connect_share(in.s, argv[1], argv[2], "")) return 2;
        in.f = smb2_open(in.s, argv[3], O_RDONLY); struct smb2_stat_64 st;
        if (!in.f || smb2_fstat(in.s, in.f, &st)) return 2;
        size = st.smb2_size; output = argv[4];
    } else { fprintf(stderr, "Usage: image-probe server share path output-prefix | --local image output-prefix\n"); return 2; }
    ImageSource source = {.context = &in, .size = size, .read_at = read_at}; ImageMetadata result;
    int rc = image_metadata_read(&source, &result);
    printf("%s; image=%llu bytes; read=%llu bytes; param=%zu; icon=%zu\n", result.status,
           (unsigned long long)size, (unsigned long long)result.bytes_read, result.param_size, result.icon_size);
    if (save(output, "json", result.param, result.param_size) || save(output, "png", result.icon, result.icon_size)) rc = -1;
    image_metadata_free(&result);
    if (in.s) { smb2_close(in.s, in.f); smb2_disconnect_share(in.s); smb2_destroy_context(in.s); }
    if (in.fd >= 0) close(in.fd);
    return rc ? 1 : 0;
}
