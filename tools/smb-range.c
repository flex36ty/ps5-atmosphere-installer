/* Read-only diagnostic: server share source offset count local-output. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
int main(int argc, char **argv) {
    if (argc != 7) return 2;
    uint64_t offset = strtoull(argv[4], NULL, 0);
    unsigned count = (unsigned)strtoul(argv[5], NULL, 0);
    if (!count || count > 1048576) return 2;
    struct smb2_context *s = smb2_init_context();
    if (!s) return 1;
    smb2_set_timeout(s, 15); smb2_set_version(s, SMB2_VERSION_ANY2);
    smb2_set_user(s, ""); smb2_set_password(s, "");
    if (smb2_connect_share(s, argv[1], argv[2], "")) { fprintf(stderr, "%s\n", smb2_get_error(s)); return 1; }
    struct smb2fh *f = smb2_open(s, argv[3], O_RDONLY);
    if (!f) return 1;
    unsigned char *buf = malloc(count);
    unsigned done = 0;
    while (done < count) { int n = smb2_pread(s, f, buf + done, count - done, offset + done); if (n <= 0) return 1; done += (unsigned)n; }
    FILE *out = fopen(argv[6], "wb");
    if (!out || fwrite(buf, 1, count, out) != count || fclose(out)) return 1;
    smb2_close(s, f); smb2_disconnect_share(s); smb2_destroy_context(s); free(buf);
    printf("Read %u bytes at offset %llu. Source unchanged.\n", count, (unsigned long long)offset);
    return 0;
}
