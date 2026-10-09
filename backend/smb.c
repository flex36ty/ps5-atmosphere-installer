/* SMB2/3 source library. Network I/O is confined to one worker. Source paths
 * never become local paths: local components are opened relative to verified
 * destination descriptors, without following symlinks. */
#include "atmosphere.h"
#include "image_metadata.h"
#include "partial_file.h"
#include "remote_source.h"
#include "copy_destinations.h"
#include "local_export.h"
#include "game_details.h"
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#define SMB_PATH 1024
#define SMB_FILES 100000
#define SMB_GAMES 1024
#define SMB_CHUNK 65536
#define COPY_READ (1024U * 1024U)
#define COPY_DEPTH 4U
#define ARTWORK_BUDGET (64U * 1024 * 1024)
#define METADATA_READER_VERSION 6
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static bool started, stopping, busy, pause_requested, cancel_requested;
static int pending; /* 1: scan, 2: download, 3: upload installed game */
static cJSON *settings, *games, *job;
static cJSON *sources;
static cJSON *scan_paths,*destination_preferences;
static void load_scan_paths(void){
    unsigned char *data=NULL;size_t length;
    cJSON_Delete(scan_paths);scan_paths=cJSON_CreateArray();
    if(!read_regular_file("/data/shadowmount/config.ini",256*1024,&data,&length)){
        cJSON_Delete(scan_paths);scan_paths=destination_scanpaths((char*)data);free(data);
    }
}
static char active_source[24] = "default";
static char password[256], message[256];
static bool remember;
static size_t artwork_bytes;
static unsigned revision;
static double transfer_speed, speed_updated;
static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}
static const char *state_name = "smb-state.json";
static const char *source_protocol(const cJSON *cfg) {
    const char *p=json_text(cfg,"protocol");return !strcmp(p,"ftp")||!strcmp(p,"webdav")||!strcmp(p,"webdavs")?p:"smb";
}

static bool component(const char *s) {
    if (!*s || !strcmp(s, ".") || !strcmp(s, "..") || strlen(s) > 240) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 32 || strchr("/\\:*?\"<>|", *p)) return false;
    return s[strlen(s)-1] != '.' && s[strlen(s)-1] != ' ';
}
static bool relative(const char *s) {
    if (strlen(s) >= SMB_PATH) return false;
    if (!*s) return true;
    char path[SMB_PATH]; copy_text(path, sizeof path, s);
    if (*s == '/' || s[strlen(s)-1] == '/' || strstr(s, "//")) return false;
    char *save, *part = strtok_r(path, "/", &save);
    for (; part; part = strtok_r(NULL, "/", &save)) if (!component(part)) return false;
    return true;
}
static bool join(char *out, const char *a, const char *b) {
    return snprintf(out, SMB_PATH, "%s%s%s", a, *a && *b ? "/" : "", b) < SMB_PATH;
}
static void set_text(cJSON *o, const char *key, const char *s) {
    cJSON_DeleteItemFromObjectCaseSensitive(o, key);
    cJSON_AddStringToObject(o, key, s);
}
static void set_number(cJSON *o, const char *key, double value) {
    cJSON_DeleteItemFromObjectCaseSensitive(o, key);
    cJSON_AddNumberToObject(o, key, value);
}
static cJSON *drive_destinations(const Storage *drive){
    cJSON *options=destination_folders(scan_paths,drive->root,drive->external,relative),*p;
    cJSON_ArrayForEach(p,sources){
        const cJSON *cfg=!strcmp(json_text(p,"id"),active_source)?settings:cJSON_GetObjectItemCaseSensitive(p,"settings");
        const char *folder=json_text(cfg,"destinationFolder");
        if(*folder&&relative(folder))destination_add(options,folder,json_text(p,"id"));
    }
    return options;
}
static void set_identity(cJSON *o, const char *key, uint64_t value) {
    char text[32]; snprintf(text, sizeof text, "%llu", (unsigned long long)value); set_text(o, key, text);
}
static bool halted(void) {
    pthread_mutex_lock(&lock);
    bool result = stopping || pause_requested || cancel_requested;
    pthread_mutex_unlock(&lock);
    return result;
}
static int write_all(int fd, const void *data, size_t length) {
    const unsigned char *p = data;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        length -= (size_t)n; p += n;
    }
    return 0;
}
/* Caller holds lock. Password is opt-in, private on POSIX filesystems, and
 * never included in HTTP responses or source URLs. */
static cJSON *source_by_id(const char *id) {
    cJSON *p; cJSON_ArrayForEach(p,sources) if(!strcmp(json_text(p,"id"),id))return p;
    return NULL;
}
static bool source_enabled(const cJSON *p) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(p,"enabled");
    return cJSON_IsBool(v)?cJSON_IsTrue(v):!strcmp(json_text(p,"id"),active_source);
}
static cJSON *capture_source(void) {
    cJSON *p = cJSON_CreateObject();
    cJSON *cfg = cJSON_Duplicate(settings, true), *list = cJSON_Duplicate(games, true);
    if (!p || !cfg || !list) { cJSON_Delete(p); cJSON_Delete(cfg); cJSON_Delete(list); return NULL; }
    cJSON_AddStringToObject(p, "id", active_source);
    cJSON_AddBoolToObject(p,"enabled",!source_by_id(active_source)||source_enabled(source_by_id(active_source)));
    cJSON_AddItemToObject(p, "settings", cfg);
    cJSON_AddItemToObject(p, "games", list);
    cJSON_AddBoolToObject(p, "remember", remember);
    cJSON_AddStringToObject(p, "password", password);
    return p;
}
static void sync_source(void) {
    cJSON *p;int i=0;
    cJSON_ArrayForEach(p,sources){if(!strcmp(json_text(p,"id"),active_source)){cJSON_ReplaceItemInArray(sources,i,capture_source());return;}i++;}
}
static void load_source(const cJSON *p) {
    cJSON_Delete(settings); cJSON_Delete(games);
    settings = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(p, "settings"), true);
    if (!cJSON_IsObject(settings)) { cJSON_Delete(settings); settings = cJSON_CreateObject(); }
    games = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(p, "games"), true);
    if (!cJSON_IsArray(games)) { cJSON_Delete(games); games = cJSON_CreateArray(); }
    copy_text(active_source, sizeof active_source, json_text(p, "id"));
    copy_text(password, sizeof password, json_text(p, "password"));
    remember = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "remember"));
}
static int save_locked(void) {
    sync_source();
    cJSON *o = cJSON_CreateObject();
    cJSON *cfg = cJSON_Duplicate(settings, true), *list = cJSON_Duplicate(games, true);
    if (!o || !cfg || !list) { cJSON_Delete(o); cJSON_Delete(cfg); cJSON_Delete(list); return -1; }
    cJSON_AddItemToObject(o, "settings", cfg);
    cJSON_AddItemToObject(o,"destinationPreferences",cJSON_Duplicate(destination_preferences,true));
    cJSON_AddItemToObject(o, "games", list);
    if (job) cJSON_AddItemToObject(o, "job", cJSON_Duplicate(job, true));
    cJSON_AddBoolToObject(o, "remember", remember);
    if (remember) cJSON_AddStringToObject(o, "password", password);
    cJSON *saved_sources = cJSON_CreateArray(), *p;
    if (!saved_sources) { cJSON_Delete(o); return -1; }
    cJSON_ArrayForEach(p, sources) {
        cJSON *copy = !strcmp(json_text(p, "id"), active_source) ? capture_source() : cJSON_Duplicate(p, true);
        if (!copy) { cJSON_Delete(saved_sources); cJSON_Delete(o); return -1; }
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(copy, "remember"))) cJSON_DeleteItemFromObjectCaseSensitive(copy, "password");
        cJSON_AddItemToArray(saved_sources, copy);
    }
    cJSON_AddItemToObject(o, "sources", saved_sources);
    cJSON_AddStringToObject(o, "activeSourceId", active_source);
    char *data = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    if (!data) return -1;
    int fd = openat(atmosphere.state_fd, "smb-state.tmp", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    int rc = fd < 0 ? -1 : write_all(fd, data, strlen(data));
    if (fd >= 0) { if (fsync(fd)) rc = -1; close(fd); }
    free(data);
    if (!rc) rc = renameat(atmosphere.state_fd, "smb-state.tmp", atmosphere.state_fd, state_name);
    if (!rc) rc = fsync(atmosphere.state_fd);
    return rc;
}
static void connection_status(const char *text) {
    pthread_mutex_lock(&lock);
    copy_text(message, sizeof message, text);
    pthread_mutex_unlock(&lock);
}
struct connection_result { bool done; int status; };
static void connection_done(struct smb2_context *s, int status, void *data, void *opaque) {
    (void)s; (void)data;
    struct connection_result *result = opaque;
    result->status = status; result->done = true;
}
static struct smb2_context *connect_smb(const cJSON *cfg, const char *secret, char *error, size_t cap) {
    connection_status("Initializing SMB session...");
    struct smb2_context *s = smb2_init_context();
    if (!s) { copy_text(error, cap, "Cannot initialize SMB connection."); return NULL; }
    smb2_set_timeout(s, 15);
    /* Anonymous shares have no signing key. SMB2.1 avoids SMB3.1.1's
     * mandatory tree-connect signing in libsmb2 for guest sessions. */
    if (!*json_text(cfg, "username")) smb2_set_version(s, SMB2_VERSION_ANY2);
    smb2_set_security_mode(s, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_user(s, json_text(cfg, "username"));
    smb2_set_password(s, secret);
    smb2_set_domain(s, json_text(cfg, "domain"));
    connection_status("Resolving SMB server and opening connection...");
    struct connection_result result = {0};
    double deadline = monotonic_seconds() + 20;
    char address[300];snprintf(address,sizeof address,"%s%s%s",json_text(cfg,"server"),*json_text(cfg,"port")?":":"",json_text(cfg,"port"));
    int rc = smb2_connect_share_async(s, address, json_text(cfg, "share"),
                                     json_text(cfg, "username"), connection_done, &result);
    if (!rc) connection_status("Negotiating SMB guest/session access...");
    while (!rc && !result.done) {
        if (halted()) { copy_text(error, cap, "SMB connection cancelled."); rc = -1; break; }
        if (monotonic_seconds() >= deadline) {
            copy_text(error, cap, "SMB connection timed out after 20 seconds."); rc = -1; break;
        }
        struct pollfd fd = {.fd = smb2_get_fd(s), .events = smb2_which_events(s)};
        int ready = poll(&fd, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { snprintf(error, cap, "SMB socket poll failed: %s", strerror(errno)); rc = -1; break; }
        if (ready > 0) rc = smb2_service(s, fd.revents);
    }
    if (rc || result.status) {
        if (!*error)
        snprintf(error, cap, "SMB connection failed: %.210s", smb2_get_error(s));
        smb2_destroy_context(s); return NULL;
    }
    return s;
}
static RemoteSource *connect_source(const cJSON *cfg,const char *secret,char *error,size_t cap) {
    if(!strcmp(source_protocol(cfg),"webdav")||!strcmp(source_protocol(cfg),"webdavs")){
        connection_status("Connecting to WebDAV server...");
        RemoteSource *s=remote_webdav(json_text(cfg,"server"),(unsigned)strtoul(json_text(cfg,"port"),NULL,10),json_text(cfg,"username"),secret,!strcmp(source_protocol(cfg),"webdavs"),halted);
        if(!s)copy_text(error,cap,"Cannot initialize WebDAV connection.");return s;
    }
    if(!strcmp(json_text(cfg,"protocol"),"ftp")) {
        connection_status("Connecting to FTP server...");
        unsigned port=(unsigned)strtoul(json_text(cfg,"port"),NULL,10);
        RemoteSource *s=remote_ftp(json_text(cfg,"server"),port,json_text(cfg,"username"),secret,halted);
        if(!s)copy_text(error,cap,"Cannot initialize FTP connection.");
        return s;
    }
    struct smb2_context *s=connect_smb(cfg,secret,error,cap);return s?remote_smb(s):NULL;
}
static unsigned char *remote_small(RemoteSource *s, const char *path, size_t max, size_t *len) {
    struct smb2_stat_64 st;
    if (remote_stat(s, path, &st) || st.smb2_type != SMB2_TYPE_FILE || st.smb2_size > max) return NULL;
    RemoteFile *f = remote_open(s, path, O_RDONLY);
    if (!f) return NULL;
    unsigned char *data = calloc(1, (size_t)st.smb2_size + 1);
    size_t offset = 0;
    while (data && offset < st.smb2_size && !halted()) {
        int n = remote_pread(s, f, data + offset, (uint32_t)(st.smb2_size - offset), offset);
        if (n <= 0) break;
        offset += (size_t)n;
    }
    remote_close(s, f);
    if (!data || offset != st.smb2_size) { free(data); return NULL; }
    *len = offset; return data;
}
static bool regular(RemoteSource *s, const char *path) {
    struct smb2_stat_64 st;
    return !remote_stat(s, path, &st) && st.smb2_type == SMB2_TYPE_FILE &&
           !(st.smb2_attributes & SMB2_FILE_ATTRIBUTE_REPARSE_POINT);
}
static const char *image_format(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext) return NULL;
    if (!strcasecmp(ext, ".ffpfsc") || !strcasecmp(ext, ".ffpfs")) return "FFPFSC";
    if (!strcasecmp(ext, ".exfat") || !strcasecmp(ext, ".img")) return "exFAT";
    return NULL;
}
static bool apply_param(cJSON *g, const unsigned char *data) {
    return game_apply_param(g,data);
}
static void apply_art(cJSON *g, const unsigned char *data, size_t length, bool background) {
#ifdef ATMOSPHERE_NATIVE_APP
    if (!data || length < 8 || length > (background ? 16U : 4U) * 1024 * 1024 || memcmp(data, "\x89PNG\r\n\x1a\n", 8)) return;
    unsigned char digest[32]; unsigned digest_length = 0;
    if (!EVP_Digest(data, length, digest, &digest_length, EVP_sha256(), NULL) || digest_length != 32) return;
    char name[80], path[1200];
    strcpy(name, background ? "scene-" : "cover-");
    for (unsigned i=0;i<32;i++) snprintf(name+6+i*2,3,"%02x",digest[i]);
    strcat(name,".png");
    int fd = openat(atmosphere.state_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        int rc = write_all(fd, data, length);
        if (fsync(fd)) rc = -1;
        close(fd);
        if (rc) { unlinkat(atmosphere.state_fd, name, 0); return; }
    } else if (errno != EEXIST) return;
    snprintf(path, sizeof path, "%s/%s", atmosphere.state_dir, name);
    set_text(g,background ? "background" : "cover",path);
#else
    size_t encoded = 24 + 4 * ((length + 2) / 3);
    if (data && length >= 8 && length <= 4 * 1024 * 1024 && encoded <= ARTWORK_BUDGET - artwork_bytes &&
        !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) {
        char *url = malloc(encoded);
        if (url) {
            strcpy(url, "data:image/png;base64,");
            EVP_EncodeBlock((unsigned char *)url + 22, data, (int)length);
            set_text(g, background ? "background" : "cover", url); artwork_bytes += strlen(url); free(url);
        }
    }
#endif
}
typedef struct { RemoteSource *s; RemoteFile *file; } ImageInput;
static int image_read_at(void *context, uint64_t offset, void *out, size_t length) {
    ImageInput *input = context; unsigned char *data = out;
    while (length) {
        if (halted()) return -1;
        uint32_t want = length > SMB_CHUNK ? SMB_CHUNK : (uint32_t)length;
        int n = remote_pread(input->s, input->file, data, want, offset);
        if (n <= 0) return -1;
        data += n; length -= (size_t)n; offset += (uint64_t)n;
    }
    return 0;
}
static bool cached_metadata(cJSON *g) {
    cJSON *old;
    cJSON_ArrayForEach(old, games) {
        if (strcmp(json_text(old, "path"), json_text(g, "path")) ||
            json_int(old, "size") != json_int(g, "size") ||
            json_int(old, "mtime") != json_int(g, "mtime") ||
            json_int(old, "mtimeNs") != json_int(g, "mtimeNs") ||
            json_int(old, "metadataReader") != METADATA_READER_VERSION) continue;
        if (strcmp(json_text(old,"metadataStatus"),"Embedded metadata")) continue;
        const char *keys[] = {"title", "titleId", "cover", "metadataStatus", "metadataReader", "minimumFirmware", "backportFiles", "region"};
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
            const cJSON *value = cJSON_GetObjectItemCaseSensitive(old, keys[i]);
            if (!value) continue;
            if (!strcmp(keys[i], "cover")) {
                size_t length = strlen(json_text(old, "cover"));
                if (length > ARTWORK_BUDGET - artwork_bytes) continue;
                artwork_bytes += length;
            }
            cJSON_DeleteItemFromObjectCaseSensitive(g, keys[i]);
            cJSON_AddItemToObject(g, keys[i], cJSON_Duplicate(value, true));
        }
        cJSON_AddBoolToObject(g, "metadataCached", true);
        return true;
    }
    return false;
}
static void metadata(RemoteSource *s, cJSON *g, bool folder) {
    const char *source = json_text(g, "path");
    bool title_found = false;
    if (!folder && cached_metadata(g)) return;
    if (!folder) {
        ImageInput input = {.s = s, .file = remote_open(s, source, O_RDONLY)};
        struct smb2_stat_64 before, after;
        if (input.file && !remote_fstat(s, input.file, &before)) {
            ImageSource image = {.context = &input, .size = before.smb2_size, .read_at = image_read_at, .skip_background=1};
            ImageMetadata result;
            int rc = image_metadata_read(&image, &result);
            bool unchanged = !remote_fstat(s, input.file, &after) && before.smb2_size == after.smb2_size &&
                before.smb2_mtime == after.smb2_mtime && before.smb2_mtime_nsec == after.smb2_mtime_nsec;
            if (unchanged && !rc) {
                title_found = apply_param(g, result.param);
                cJSON_AddBoolToObject(g,"backportFiles",result.backport_files!=0);
                apply_art(g, result.icon, result.icon_size, false);
                set_number(g, "metadataReader", METADATA_READER_VERSION);
            }
            set_text(g, "metadataStatus", unchanged ? result.status : "Image changed during metadata scan");
            set_number(g, "metadataBytesRead", (double)result.bytes_read);
            image_metadata_free(&result);
        }
        if (input.file) remote_close(s, input.file);
    }
    /* Folder metadata and optional sidecars fill any gaps. Source files are
     * never modified; successful embedded results live in the local cache. */
    char path[SMB_PATH]; size_t length = 0;
    if(folder){
        bool backport=false;struct smb2_stat_64 st;
        const char *names[]={"fakelib","fakelib2"};
        for(unsigned i=0;i<2;i++)if(snprintf(path,sizeof path,"%s/%s",source,names[i])<(int)sizeof path && !remote_stat(s,path,&st) && st.smb2_type==SMB2_TYPE_DIRECTORY)backport=true;
        cJSON_AddBoolToObject(g,"backportFiles",backport);
    }
    int n = snprintf(path, sizeof path, "%s%s", source, folder ? "/sce_sys/param.json" : ".json");
    unsigned char *data = !title_found && n < (int)sizeof path ? remote_small(s, path, 65536, &length) : NULL;
    if (data) { apply_param(g, data); free(data); }
    n = snprintf(path, sizeof path, "%s%s", source, folder ? "/sce_sys/icon0.png" : ".png");
    data = !*json_text(g, "cover") && n < (int)sizeof path && artwork_bytes < ARTWORK_BUDGET ?
           remote_small(s, path, 4 * 1024 * 1024, &length) : NULL;
    if (data) { apply_art(g, data, length, false); free(data); }

}
static int discover(RemoteSource *s, const char *path, unsigned depth,
                    unsigned *visited, cJSON *out) {
    if (halted() || depth > 12 || ++*visited > 10000) return -1;
    RemoteDir *dir = remote_opendir(s, path);
    if (!dir) return -1;
    int rc = 0; struct smb2dirent *entry;
    while ((entry = remote_readdir(s, dir))) {
        if (!strcmp(entry->name, ".") || !strcmp(entry->name, "..")) continue;
        if (!component(entry->name) || entry->name[0] == '.' ||
            entry->st.smb2_attributes & SMB2_FILE_ATTRIBUTE_REPARSE_POINT) continue;
        char child[SMB_PATH], marker[SMB_PATH];
        if (!join(child, path, entry->name)) { rc = -1; break; }
        bool folder = entry->st.smb2_type == SMB2_TYPE_DIRECTORY;
        const char *format = folder ? NULL : image_format(entry->name);
        if (folder) {
            if (!join(marker, child, "sce_sys/param.json")) { rc = -1; break; }
            if (regular(s, marker)) format = "Folder";
            else if (join(marker, child, "eboot.bin") && regular(s, marker)) format = "Folder";
        }
        if (format) {
            if (cJSON_GetArraySize(out) >= SMB_GAMES) { rc = -1; break; }
            cJSON *g = cJSON_CreateObject();
            if (!g) { rc = -1; break; }
            char id[24]; random_hex(id, 8);
            cJSON_AddStringToObject(g, "id", id);
            cJSON_AddStringToObject(g, "path", child);
            cJSON_AddStringToObject(g, "filename", entry->name);
            cJSON_AddStringToObject(g, "title", entry->name);
            cJSON_AddStringToObject(g, "format", format);
            cJSON_AddBoolToObject(g, "folder", folder);
            /* Preserve first discovery across rescans; legacy entries remain
             * undated rather than inventing a historical addition date. */
            double added = (double)time(NULL);
            cJSON *previous;
            cJSON_ArrayForEach(previous, games) {
                if (!strcmp(json_text(previous, "path"), child)) {
                    added = (double)json_int(previous, "addedAt");
                    break;
                }
            }
            cJSON_AddNumberToObject(g, "addedAt", added);
            if (!folder) {
                cJSON_AddNumberToObject(g, "size", (double)entry->st.smb2_size);
                cJSON_AddNumberToObject(g, "mtime", (double)entry->st.smb2_mtime);
                cJSON_AddNumberToObject(g, "mtimeNs", (double)entry->st.smb2_mtime_nsec);
            }
            pthread_mutex_lock(&lock);
            snprintf(message, sizeof message, "Reading game %d: %.180s", cJSON_GetArraySize(out) + 1, entry->name);
            pthread_mutex_unlock(&lock);
            metadata(s, g, folder);
            if (!*json_text(g,"path") || !*json_text(g,"title")) { cJSON_Delete(g); rc = -1; break; }
            cJSON_AddItemToArray(out, g);
        } else if (folder && discover(s, child, depth + 1, visited, out)) { rc = -1; break; }
        if (halted()) { rc = -1; break; }
    }
    remote_closedir(s, dir); return rc;
}
/* Freeze a manifest before copying; resume validates the exact same source
 * files, including timestamps. New files are never silently mixed into it. */
static int manifest(RemoteSource *s, const char *base, const char *rel,
                    cJSON *files, uint64_t *total, unsigned depth) {
    char path[SMB_PATH]; struct smb2_stat_64 st;
    if (halted() || depth > 32 || !join(path, base, rel) || remote_stat(s, path, &st) ||
        st.smb2_attributes & SMB2_FILE_ATTRIBUTE_REPARSE_POINT ||
        cJSON_GetArraySize(files) >= SMB_FILES) return -1;
    if (st.smb2_type != SMB2_TYPE_FILE && st.smb2_type != SMB2_TYPE_DIRECTORY) return -1;
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "path", rel);
    cJSON_AddBoolToObject(f, "directory", st.smb2_type == SMB2_TYPE_DIRECTORY);
    cJSON_AddNumberToObject(f, "size", (double)st.smb2_size);
    cJSON_AddNumberToObject(f, "mtime", (double)st.smb2_mtime);
    cJSON_AddNumberToObject(f, "mtimeNs", (double)st.smb2_mtime_nsec);
    cJSON_AddItemToArray(files, f);
    if (st.smb2_type == SMB2_TYPE_FILE) {
        if (st.smb2_size > INT64_MAX - *total) return -1;
        *total += st.smb2_size; return 0;
    }
    RemoteDir *dir = remote_opendir(s, path);
    if (!dir) return -1;
    int rc = 0; struct smb2dirent *entry;
    while ((entry = remote_readdir(s, dir))) {
        if (!strcmp(entry->name, ".") || !strcmp(entry->name, "..")) continue;
        char child[SMB_PATH];
        if (!component(entry->name) || !join(child, rel, entry->name) ||
            manifest(s, base, child, files, total, depth + 1)) { rc = -1; break; }
    }
    remote_closedir(s, dir); return rc;
}
static int directory_at(int parent, const char *name, dev_t device) {
    if (mkdirat(parent, name, 0700) && errno != EEXIST) return -1;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || st.st_dev != device)) { close(fd); fd = -1; }
    return fd;
}
static int local_parent(int root, const char *path, char *leaf, dev_t device) {
    char text[SMB_PATH]; copy_text(text, sizeof text, path);
    int fd = dup(root);
    char *part = text, *slash;
    while (fd >= 0 && (slash = strchr(part, '/'))) {
        *slash = 0;
        if (!component(part)) { close(fd); return -1; }
        int next = directory_at(fd, part, device); close(fd); fd = next; part = slash + 1;
    }
    if (!component(part)) { if (fd >= 0) close(fd); return -1; }
    copy_text(leaf, 256, part); return fd;
}
static bool same_file(const struct smb2_stat_64 *st, const cJSON *f) {
    return st->smb2_type == SMB2_TYPE_FILE && !(st->smb2_attributes & SMB2_FILE_ATTRIBUTE_REPARSE_POINT) &&
        st->smb2_size == (uint64_t)json_int(f, "size") &&
        st->smb2_mtime == (uint64_t)json_int(f, "mtime") &&
        st->smb2_mtime_nsec == (uint64_t)json_int(f, "mtimeNs");
}
typedef struct { bool ready; int status; uint32_t size; } CopyRead;
#include "copy_pipeline.h"
static void copy_read_done(struct smb2_context *s, int status, void *data, void *opaque) {
    (void)s; (void)data; CopyRead *r = opaque; r->status = status; r->ready = true;
}
/* Keep callback state alive until every request completes or context destruction
 * cancels the outstanding requests. Short reads (including credit limits) are
 * filled after the batch drains, so no source bytes can be skipped. */
static int smb_copy_read_batch(struct smb2_context *s, struct smb2fh *file, unsigned char *data,
                           uint32_t chunk, uint64_t offset, uint32_t length, bool *destroyed) {
    CopyRead reads[COPY_DEPTH] = {0}; unsigned count = 0; uint32_t pos = 0;
    while (pos < length) {
        CopyRead *r = &reads[count++]; r->size = length - pos < chunk ? length - pos : chunk;
        if (smb2_pread_async(s, file, data + pos, r->size, offset + pos, copy_read_done, r)) goto abort;
        pos += r->size;
    }
    double began = monotonic_seconds();
    for (;;) {
        bool ready = true;
        for (unsigned i = 0; i < count; i++) if (!reads[i].ready) ready = false;
        if (ready) break;
        if (halted() || monotonic_seconds() - began > 20) goto abort;
        struct pollfd p = {.fd = smb2_get_fd(s), .events = (short)smb2_which_events(s)};
        int rc = poll(&p, 1, 100);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0 || (rc && smb2_service(s, p.revents) < 0)) goto abort;
    }
    pos = 0;
    for (unsigned i = 0; i < count; i++) {
        int got = reads[i].status;
        if (got <= 0 || (uint32_t)got > reads[i].size) return -1;
        while ((uint32_t)got < reads[i].size) {
            if (halted()) return -1;
            int n = smb2_pread(s, file, data + pos + got, reads[i].size - (uint32_t)got, offset + pos + (uint32_t)got);
            if (n <= 0 || (uint32_t)n > reads[i].size - (uint32_t)got) return -1;
            got += n;
        }
        pos += reads[i].size;
    }
    return (int)length;
abort:
    smb2_destroy_context(s); *destroyed = true; return -1;
}
static int copy_read_batch(RemoteSource *s,RemoteFile *file,unsigned char *data,uint32_t chunk,uint64_t offset,uint32_t length,bool *destroyed) {
    if(!remote_smb_context(s))return remote_pread(s,file,data,length,offset);
    int rc=smb_copy_read_batch(remote_smb_context(s),remote_smb_file(file),data,chunk,offset,length,destroyed);
    if(*destroyed)remote_forget_smb(s);
    return rc;
}
typedef struct {RemoteSource *s;RemoteFile *file;uint32_t chunk;bool *destroyed;double seconds;} CopyReader;
static int copy_fetch(void *opaque,unsigned char *data,uint32_t length,uint64_t offset) {
    CopyReader *r=opaque;double start=monotonic_seconds();
    int n=copy_read_batch(r->s,r->file,data,r->chunk,offset,length,r->destroyed);
    r->seconds+=monotonic_seconds()-start;return n;
}
static int copy_file(RemoteSource *s, const char *remote, int parent, const char *name,
                     const cJSON *f, uint64_t *done, Job *target, char *err, bool *destroyed) {
    RemoteFile *in = remote_open(s, remote, O_RDONLY);
    struct smb2_stat_64 before, after;
    if (!in) { strcpy(err, "Cannot open source file."); return -1; }
    int out = -1, rc = -1;
    uint32_t chunk = remote_max_read(s);
    if (!chunk || chunk > COPY_READ) chunk = COPY_READ;
    uint32_t capacity = chunk * COPY_DEPTH;
    unsigned char *data = NULL, *old = NULL, *ahead=NULL;
    CopyPipeline pipeline;bool pipelined=false,used_pipeline=false;
    CopyReader reader={s,in,chunk,destroyed,0};
    double started=monotonic_seconds(),wait_seconds=0,write_seconds=0,hash_seconds=0,verify_started=0,verify_seconds=0;
    uint64_t copied=0;
    /* Native titles can have a smaller heap than payload builds. Keep the
     * pipeline, but reduce each batch when two full-size buffers do not fit. */
    for (;;) {
        data=malloc(capacity);old=malloc(capacity);
        if(data && old)break;
        free(data);free(old);data=old=NULL;
        if(capacity<=SMB_CHUNK)break;
        capacity/=2;
    }
    EVP_MD_CTX *digest = EVP_MD_CTX_new(), *verify = EVP_MD_CTX_new();
    if (!data || !old || !digest || !verify) { strcpy(err,"Not enough memory for transfer buffers or verification."); goto end; }
    if (remote_fstat(s, in, &before) || !same_file(&before, f)) {
        strcpy(err, "Source changed since this copy began. Start a new copy."); goto end;
    }
    out = openat(parent, name, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat st;
    if (out < 0) {
        int error=errno;
        snprintf(err,256,"Cannot open partial file: %s (errno %d)",strerror(error),error); goto end;
    }
    if (fstat(out,&st)) {
        int error=errno;
        snprintf(err,256,"Cannot inspect partial file: %s (errno %d)",strerror(error),error); goto end;
    }
    struct stat path_st;
    const struct stat *named_file=NULL;
#ifdef ATMOSPHERE_NATIVE_APP
    if(st.st_nlink==0 && !fstatat(parent,name,&path_st,AT_SYMLINK_NOFOLLOW))named_file=&path_st;
#endif
    if (!partial_file_valid(&st,named_file,target->device,before.smb2_size)) {
        snprintf(err,256,"Partial file rejected: mode=%o links=%llu device=%llu expected=%llu size=%lld source=%llu",
                 (unsigned)st.st_mode,(unsigned long long)st.st_nlink,
                 (unsigned long long)st.st_dev,(unsigned long long)target->device,
                 (long long)st.st_size,(unsigned long long)before.smb2_size); goto end;
    }
    if (!EVP_DigestInit_ex(digest, EVP_sha256(), NULL)) { strcpy(err,"Cannot initialize SHA-256 transfer verification."); goto end; }
    /* The reader exclusively owns the remote context until joined. Allocation
     * or thread failure falls back to the original synchronous transfer. */
    /* Overlap SMB reads with local writes/hash work on the console. Other
     * native protocols keep their existing path; timings record actual use. */
    if((atmosphere.desktop || !strcmp(remote_protocol(s),"smb")) && before.smb2_size>capacity) {
        ahead=malloc(capacity);
        if(ahead && !copy_pipeline_start(&pipeline,data,ahead,capacity,before.smb2_size,copy_fetch,&reader))pipelined=used_pipeline=true;
    }
    uint64_t offset = 0;
    double speed_start = monotonic_seconds();
    uint64_t speed_bytes = 0;
    double storage_checked = -1, progress_updated = 0;
    while (offset < before.smb2_size) {
        if (halted()) goto end;
        uint64_t remaining = before.smb2_size - offset;
        unsigned char *block=data;double tick=monotonic_seconds();
        int n = pipelined?copy_pipeline_take(&pipeline,&block):copy_fetch(&reader,block,remaining < capacity ? (uint32_t)remaining : capacity,offset);
        wait_seconds+=monotonic_seconds()-tick;
        if (n <= 0) { strcpy(err, "Server read failed. Check the connection and resume."); goto end; }
        /* Compare every existing byte before trusting an interrupted copy. */
        size_t prefix = offset < (uint64_t)st.st_size ? (size_t)((uint64_t)st.st_size - offset) : 0;
        if (prefix > (size_t)n) prefix = (size_t)n;
        if (prefix && (pread(out, old, prefix, (off_t)offset) != (ssize_t)prefix || memcmp(old, block, prefix))) {
            strcpy(err, "Partial file differs from the source. Start a new copy."); goto end;
        }
        if ((size_t)n > prefix) {
            double check_time = monotonic_seconds();
            if (storage_checked < 0 || check_time - storage_checked >= 1) {
                if (!storage_matches(target) || !storage_has_space(out,(uint64_t)n + 1048576)) {
                    strcpy(err, "Destination disconnected, full, or not writable. Reconnect and resume."); goto end;
                }
                storage_checked = check_time;
            }
            tick=monotonic_seconds();
            if (lseek(out, (off_t)(offset + prefix), SEEK_SET) < 0 ||
                write_all(out, block + prefix, (size_t)n - prefix)) {
                strcpy(err, "Destination disconnected, full, or not writable. Reconnect and resume."); goto end;
            }
            write_seconds+=monotonic_seconds()-tick;
        }
        tick=monotonic_seconds();
        if (!EVP_DigestUpdate(digest, block, (size_t)n)) goto end;
        hash_seconds+=monotonic_seconds()-tick;
        if(pipelined)copy_pipeline_release(&pipeline);
        offset += (uint64_t)n; *done += (uint64_t)n;
        copied=offset;
        speed_bytes += (uint64_t)n;
        double now = monotonic_seconds();
        if (now - progress_updated >= 0.1 || offset == before.smb2_size) {
        progress_updated = now;
        pthread_mutex_lock(&lock);
        set_number(job, "received", (double)*done);
        set_text(job, "phase", prefix == (size_t)n ? "Checking partial copy" : "Transferring");
        if (now - speed_start >= 0.5) {
            transfer_speed = (double)speed_bytes / (now - speed_start);
            speed_updated = now; speed_start = now; speed_bytes = 0;
        }
        pthread_mutex_unlock(&lock);
        }
    }
    if(pipelined){copy_pipeline_finish(&pipeline);pipelined=false;}
    verify_started=monotonic_seconds();
    pthread_mutex_lock(&lock); transfer_speed = 0; set_text(job, "phase", "Verifying"); pthread_mutex_unlock(&lock);
    if (remote_fstat(s, in, &after) || !same_file(&after, f) || fsync(out)) {
        strcpy(err, "Source changed or destination could not be flushed."); goto end;
    }
    if (!EVP_DigestInit_ex(verify, EVP_sha256(), NULL) || lseek(out, 0, SEEK_SET) < 0) goto end;
    uint64_t checked = 0;
    while (checked < before.smb2_size) {
        if (halted()) goto end;
        ssize_t n = read(out, data, capacity);
        if (n <= 0 || !EVP_DigestUpdate(verify, data, (size_t)n)) goto end;
        checked += (uint64_t)n;
    }
    unsigned char a[32], b[32]; unsigned alen, blen;
    if (!EVP_DigestFinal_ex(digest, a, &alen) || !EVP_DigestFinal_ex(verify, b, &blen) ||
        alen != blen || memcmp(a, b, alen)) { strcpy(err, "Destination SHA-256 verification failed."); goto end; }
    rc = 0;
end:
    if(pipelined)copy_pipeline_finish(&pipeline);
    if(verify_started)verify_seconds=monotonic_seconds()-verify_started;
    /* Persist diagnostics with the job as well: native log creation can fail. */
    pthread_mutex_lock(&lock);
    cJSON *timing=cJSON_CreateObject();
    if(timing){
        cJSON_AddStringToObject(timing,"protocol",remote_protocol(s));
        cJSON_AddBoolToObject(timing,"readAhead",used_pipeline);
        cJSON_AddNumberToObject(timing,"bufferBytes",capacity);
        cJSON_AddNumberToObject(timing,"bytes",(double)copied);
        cJSON_AddNumberToObject(timing,"elapsedSeconds",monotonic_seconds()-started);
        cJSON_AddNumberToObject(timing,"remoteSeconds",reader.seconds);
        cJSON_AddNumberToObject(timing,"waitSeconds",wait_seconds);
        cJSON_AddNumberToObject(timing,"writeSeconds",write_seconds);
        cJSON_AddNumberToObject(timing,"hashSeconds",hash_seconds);
        cJSON_AddNumberToObject(timing,"verifySeconds",verify_seconds);
        cJSON_AddNumberToObject(timing,"result",rc);
        cJSON_DeleteItemFromObjectCaseSensitive(job,"transferProfile");
        cJSON_AddItemToObject(job,"transferProfile",timing);
    }
    pthread_mutex_unlock(&lock);
    /* One bounded record per file, no server paths or credentials. */
    char profile[512];int length=snprintf(profile,sizeof profile,"protocol=%s pipeline=%d buffer=%u bytes=%llu elapsed=%.3f remote=%.3f wait=%.3f write=%.3f hash=%.3f verify=%.3f result=%d\n",
        remote_protocol(s),used_pipeline,capacity,(unsigned long long)copied,monotonic_seconds()-started,reader.seconds,wait_seconds,write_seconds,hash_seconds,verify_seconds,rc);
    int logfd=openat(atmosphere.state_fd,"transfer-profile.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
    if(logfd>=0){struct stat logst;if(!fstat(logfd,&logst)&&S_ISREG(logst.st_mode)&&logst.st_size<65536)(void)write(logfd,profile,(size_t)length);close(logfd);}
    if (out >= 0) close(out);
    remote_close(s, in);
    free(data); free(old); free(ahead); EVP_MD_CTX_free(digest); EVP_MD_CTX_free(verify);
    return rc;
}
int storage_repair_permissions(const char *name,char *error,size_t cap);
static int copy_game(RemoteSource *s, cJSON *work, char *err, bool *destroyed) {
    Job target = {0};
    copy_text(target.storage_id, sizeof target.storage_id, json_text(work, "storageId"));
    copy_text(target.root, sizeof target.root, json_text(work, "root"));
    target.device = (dev_t)strtoull(json_text(work, "device"), NULL, 10);
    target.inode = (ino_t)strtoull(json_text(work, "inode"), NULL, 10);
    const char *base = json_text(work, "path"), *name = json_text(work, "filename");
    bool folder = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(work, "folder"));
    if (!relative(base) || !component(name)) { strcpy(err, "Invalid source path."); return -1; }
    cJSON *files = cJSON_GetObjectItemCaseSensitive(work, "files");
    if (!files) {
        files = cJSON_CreateArray(); uint64_t total = 0;
        if (manifest(s, base, "", files, &total, 0)) {
            cJSON_Delete(files); strcpy(err, "Cannot enumerate all source files (unsafe name, unreadable folder, or scan limit)."); return -1;
        }
        cJSON_AddItemToObject(work, "files", files); set_number(work, "total", (double)total);
        pthread_mutex_lock(&lock);
        cJSON_DeleteItemFromObjectCaseSensitive(job, "files");
        cJSON_AddItemToObject(job, "files", cJSON_Duplicate(files, true));
        set_number(job, "total", (double)total);
        int saved = save_locked(); pthread_mutex_unlock(&lock);
        if (saved) { strcpy(err, "Cannot save the transfer manifest."); return -1; }
    }
    int dest = -1, root = -1, stage = -1, item = -1, content = -1, rc = -1;
    if (!storage_matches(&target)) { strcpy(err, "Original destination is unavailable."); goto end; }
    struct stat st;
    root = open(target.root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (root < 0) { snprintf(err, 256, "Cannot access destination: %s", strerror(errno)); goto end; }
    if (fstat(root, &st) || st.st_dev != target.device || st.st_ino != target.inode) {
        strcpy(err, "Destination changed or is unavailable."); goto end;
    }
    const char *destination = json_text(work, "destinationFolder");
    if (!*destination && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(work, "usbRoot"))) destination = "homebrew";
    if (!relative(destination)) goto end;
    char destination_path[SMB_PATH], leaf[256];
    if (!join(destination_path, destination, "placeholder")) goto end;
    dest = local_parent(root, destination_path, leaf, target.device);
    if(dest<0 && (errno==EACCES||errno==EPERM) && !strcmp(target.root,"/data") && !strcmp(destination,"homebrew")) {
        pthread_mutex_lock(&lock);set_text(job,"phase","Repairing internal folder permissions");pthread_mutex_unlock(&lock);
        if(storage_repair_permissions("homebrew",err,256))goto end;
        dest=local_parent(root,destination_path,leaf,target.device);
    }
    if (dest < 0) { snprintf(err, 256, "Cannot create/open destination folder: %s", strerror(errno)); goto end; }
    if (!fstatat(dest, name, &st, AT_SYMLINK_NOFOLLOW) || errno != ENOENT) {
        strcpy(err, "Destination name already exists. Existing games are never replaced."); goto end;
    }
    /* Preserve partial transfers from the original application name. */
    struct stat stage_status;
    if(fstatat(root,".atmosphere-smb-staging",&stage_status,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT)
        (void)renameat(root,".orbit-smb-staging",root,".atmosphere-smb-staging");
    stage = directory_at(root, ".atmosphere-smb-staging", target.device);
    if(stage<0 && (errno==EACCES||errno==EPERM) && !strcmp(target.root,"/data")) {
        pthread_mutex_lock(&lock);set_text(job,"phase","Repairing internal staging permissions");pthread_mutex_unlock(&lock);
        if(storage_repair_permissions(".atmosphere-smb-staging",err,256))goto end;
        stage=directory_at(root,".atmosphere-smb-staging",target.device);
    }
    if (stage < 0) { snprintf(err, 256, "Cannot write destination staging folder: %s", strerror(errno)); goto end; }
    item = directory_at(stage, json_text(work, "id"), target.device);
    if (item < 0) { snprintf(err,256,"Cannot open transfer staging item: %s (errno %d)",strerror(errno),errno); goto end; }
    content = folder ? directory_at(item, "content", target.device) : dup(item);
    if (content < 0) { snprintf(err,256,"Cannot open staging content: %s (errno %d)",strerror(errno),errno); goto end; }
    uint64_t done = 0; cJSON *f;
    /* Conservative check: reserve enough space for the complete manifest on a
     * new transfer; resumed files are checked incrementally before each write. */
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(work, "resuming")) &&
        !storage_has_space(dest,(uint64_t)json_int(work, "total") + 1048576)) {
        strcpy(err, "Not enough free space for this game."); goto end;
    }
    cJSON_ArrayForEach(f, files) {
        const char *rel = json_text(f, "path"); char remote[SMB_PATH], leaf[256];
        if (halted() || !relative(rel) || !join(remote, base, rel)) goto end;
        if (!*rel && folder) continue;
        int parent = local_parent(content, folder ? rel : "content", leaf, target.device);
        if (parent < 0) { strcpy(err, "Cannot create a safe destination folder."); goto end; }
        int result;
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f, "directory"))) {
            result = directory_at(parent, leaf, target.device);
            if (result >= 0) { close(result); result = 0; }
        } else result = copy_file(s, remote, parent, leaf, f, &done, &target, err, destroyed);
        if (fsync(parent)) result = -1;
        close(parent);
        if (result) goto end;
    }
    if (halted() || !storage_matches(&target)) goto end;
    /* Reserve the final name exclusively, including on exFAT (no hardlinks).
     * An unfinished game is outside homebrew until this final rename. */
    if (folder) {
        if (mkdirat(dest, name, 0755)) { strcpy(err, "Destination name is already in use."); goto end; }
    } else {
        int reserve = openat(dest, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (reserve < 0) { strcpy(err, "Destination name is already in use."); goto end; }
        close(reserve);
    }
    if (renameat(item, "content", dest, name)) {
        unlinkat(dest, name, folder ? AT_REMOVEDIR : 0);
        strcpy(err, "Cannot finalize the copy."); goto end;
    }
    fsync(dest); unlinkat(stage, json_text(work, "id"), AT_REMOVEDIR); rc = 0;
end:
    if (content >= 0) close(content);
    if (item >= 0) close(item);
    if (stage >= 0) close(stage);
    if (root >= 0) close(root);
    if (dest >= 0) close(dest);
    if (rc && !*err) strcpy(err, "Copy interrupted. Partial data is kept for resume.");
    return rc;
}
static char scan_selection[24];
static void scan_servers(void) {
    pthread_mutex_lock(&lock);
    sync_source();
    char selected[24];copy_text(selected,sizeof selected,active_source);
    copy_text(scan_selection,sizeof scan_selection,active_source);
    cJSON *profiles=cJSON_Duplicate(sources,true);
    pthread_mutex_unlock(&lock);
    unsigned completed=0,failed=0,total=0;cJSON *p;char last_error[256]={0};
    cJSON_ArrayForEach(p,profiles){
        if(!source_enabled(p)||halted())continue;
        pthread_mutex_lock(&lock);load_source(p);pthread_mutex_unlock(&lock);
        char err[256]={0};cJSON *cfg=cJSON_GetObjectItemCaseSensitive(p,"settings");
        RemoteSource *s=connect_source(cfg,json_text(p,"password"),err,sizeof err);
        cJSON *found=cJSON_CreateArray();unsigned visited=0;artwork_bytes=0;
        int rc=s&&found?discover(s,json_text(cfg,"folder"),0,&visited,found):-1;
        if(rc)copy_text(last_error,sizeof last_error,*err?err:s?remote_error(s):"Cannot connect to server.");
        if(s)remote_destroy(s);
        pthread_mutex_lock(&lock);
        if(!rc){cJSON_Delete(games);games=found;found=NULL;completed++;revision++;}
        else failed++;
        total+=(unsigned)cJSON_GetArraySize(games);
        sync_source();
        pthread_mutex_unlock(&lock);
        cJSON_Delete(found);
        if(!rc)installed_refresh(json_text(cfg,"destinationFolder"));
    }
    cJSON_Delete(profiles);
    pthread_mutex_lock(&lock);
    load_source(source_by_id(selected));
    scan_selection[0]=0;
    snprintf(message,sizeof message,"Scan complete: %u games, %u servers refreshed, %u unavailable (cached games retained).",total,completed,failed);
    if(failed)snprintf(message,sizeof message,"Scan complete: %u games; %u servers unavailable, cache retained. %.150s",total,failed,last_error);
    busy=false;
    if(save_locked())copy_text(message,sizeof message,"Scan finished, but server state could not be saved.");
    pthread_mutex_unlock(&lock);
}
static void export_progress(uint64_t done,uint64_t total,const char *phase,const char *path){
    pthread_mutex_lock(&lock);
    static uint64_t previous;
    double now=monotonic_seconds();if(!done)previous=0;
    if(now-speed_updated>.25){transfer_speed=done>=previous?(done-previous)/(now-speed_updated):0;speed_updated=now;previous=done;}
    set_number(job,"received",(double)done);set_number(job,"total",(double)total);
    bool path_changed=strcmp(json_text(job,"remotePath"),path)!=0;
    set_text(job,"phase",phase);set_text(job,"remotePath",path);
    if(path_changed)save_locked();
    pthread_mutex_unlock(&lock);
}
static void *worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!pending && !stopping) pthread_cond_wait(&changed, &lock);
        if (stopping) { pthread_mutex_unlock(&lock); break; }
        int action = pending; pending = 0;
        if(action==1){pthread_mutex_unlock(&lock);scan_servers();continue;}
        cJSON *cfg = cJSON_Duplicate(settings, true), *work = cJSON_Duplicate(job, true);
        char secret[256]; copy_text(secret, sizeof secret, password);
        pthread_mutex_unlock(&lock);
        char err[256] = {0}; cJSON *found = NULL;
        pthread_mutex_lock(&lock);
        copy_text(message, sizeof message, "Connecting to server...");
        pthread_mutex_unlock(&lock);
        RemoteSource *s = connect_source(cfg, secret, err, sizeof err); memset(secret, 0, sizeof secret);
        int rc = -1; bool destroyed = false;
        if (!s) { if (!*err) strcpy(err, "Cannot connect to server."); }
        else if (action == 1) {
            pthread_mutex_lock(&lock);
            copy_text(message, sizeof message, "Connected. Reading server folder...");
            pthread_mutex_unlock(&lock);
            found = cJSON_CreateArray(); unsigned visited = 0; artwork_bytes = 0;
            rc = found ? discover(s, json_text(cfg, "folder"), 0, &visited, found) : -1;
            if (rc) snprintf(err, sizeof err, "Scan incomplete: %.210s", remote_error(s));
        } else if(action==3)rc=local_export(s,work,json_text(cfg,"folder"),halted,export_progress,err);
        else rc = copy_game(s, work, err, &destroyed);
        if (s) { remote_destroy(s); }
        /* The final rename and verification have completed before a rescan.
         * API availability must never turn a successful copy into a failure. */
        bool scan_queued=false;
        if(action==2 && !rc)scan_queued=library_rescan_after_copy();
        if(!rc)installed_refresh(json_text(cfg,"destinationFolder"));
        bool skipped_verification=action==3&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(work,"skipVerification"));
        cJSON_Delete(cfg); cJSON_Delete(work);
        pthread_mutex_lock(&lock);
        if (action == 1 && !rc) { cJSON_Delete(games); games = found; found = NULL; revision++; }
        if (action == 2 || action == 3) {
            transfer_speed = 0;
            set_text(job, "status", !rc ? "complete" : cancel_requested ? "cancelled" :
                     action==2&&(pause_requested || stopping) ? "paused" : "error");
            set_text(job, "error", !rc ? "" : err);
            if (!rc) {
                set_text(job, "verification", skipped_verification?"skipped":"sha256");
                set_text(job,"phase",action==3?(skipped_verification?"Backup complete; verification skipped":"Server backup verified; refresh to list it"):scan_queued?"Verified; ShadowMount rescan requested":"Verified; ShadowMount rescan unavailable");
                set_text(job,"rescan",scan_queued?"requested":"unavailable");
            }
            pthread_mutex_lock(&atmosphere.mutex); atmosphere.smb_storage_busy = false;
            pthread_cond_signal(&atmosphere.changed); pthread_mutex_unlock(&atmosphere.mutex);
        }
        if (action == 1 && !rc) snprintf(message, sizeof message, "Scan complete: %d games found.", cJSON_GetArraySize(games));
        else copy_text(message, sizeof message, rc ? err : skipped_verification?"Copy complete. Verification skipped.":"Copy complete. SHA-256 verified.");
        busy = false;
        if (save_locked()) copy_text(message, sizeof message, "Operation finished, but server state could not be saved.");
        pthread_mutex_unlock(&lock); cJSON_Delete(found);
    }
    return NULL;
}
cJSON *smb_snapshot(bool include_games) {
    pthread_mutex_lock(&lock);
    cJSON *o = cJSON_CreateObject();
    cJSON *destinations=cJSON_AddArrayToObject(o,"destinations");
    Storage targets[ATMOSPHERE_MAX_STORAGE];size_t target_count=storage_list(targets);
    for(size_t i=0;i<target_count;i++){
        cJSON *d=cJSON_CreateObject();cJSON_AddStringToObject(d,"storageId",targets[i].id);
        cJSON_AddStringToObject(d,"root",targets[i].root);
        cJSON_AddStringToObject(d,"selected",json_text(destination_preferences,targets[i].root));
        cJSON_AddBoolToObject(d,"hasSelection",cJSON_IsString(cJSON_GetObjectItemCaseSensitive(destination_preferences,targets[i].root)));
        cJSON_AddItemToObject(d,"folders",drive_destinations(&targets[i]));cJSON_AddItemToArray(destinations,d);
    }
    cJSON *list = cJSON_CreateArray(), *p;
    cJSON_AddItemToObject(o,"installed",installed_snapshot());
    cJSON_ArrayForEach(p, sources) {
        const cJSON *cfg = !strcmp(json_text(p, "id"), active_source) ? settings : cJSON_GetObjectItemCaseSensitive(p, "settings");
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", json_text(p, "id"));
        cJSON_AddStringToObject(item, "name", json_text(cfg, "name"));
        cJSON_AddStringToObject(item, "server", json_text(cfg, "server"));
        cJSON_AddStringToObject(item, "share", json_text(cfg, "share"));
        cJSON_AddStringToObject(item,"protocol",json_text(cfg,"protocol"));
        cJSON_AddStringToObject(item,"port",json_text(cfg,"port"));
        cJSON_AddStringToObject(item,"folder",json_text(cfg,"folder"));
        cJSON_AddStringToObject(item,"username",json_text(cfg,"username"));
        cJSON_AddStringToObject(item,"domain",json_text(cfg,"domain"));
        cJSON_AddStringToObject(item,"destinationFolder",json_text(cfg,"destinationFolder"));
        cJSON_AddBoolToObject(item,"remember",!strcmp(json_text(p,"id"),active_source)?remember:cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"remember")));
        cJSON_AddBoolToObject(item,"enabled",source_enabled(p));
        cJSON_AddItemToArray(list, item);
    }
    cJSON_AddItemToObject(o, "sources", list);
    cJSON *view=*scan_selection?source_by_id(scan_selection):NULL;
    cJSON_AddStringToObject(o, "activeSourceId", view?scan_selection:active_source);
    cJSON_AddItemToObject(o, "settings", cJSON_Duplicate(view?cJSON_GetObjectItemCaseSensitive(view,"settings"):settings, true));
    if (include_games) {
        cJSON *all=cJSON_CreateArray();
        cJSON_ArrayForEach(p,sources){
            if(!source_enabled(p))continue;
            bool current=!strcmp(json_text(p,"id"),active_source);
            const cJSON *cfg=current?settings:cJSON_GetObjectItemCaseSensitive(p,"settings");
            cJSON *g,*catalog=current?games:cJSON_GetObjectItemCaseSensitive(p,"games");
            cJSON_ArrayForEach(g,catalog){cJSON *copy=cJSON_Duplicate(g,true);
                set_text(copy,"sourceId",json_text(p,"id"));set_text(copy,"sourceName",*json_text(cfg,"name")?json_text(cfg,"name"):json_text(cfg,"server"));
                set_text(copy,"sourceProtocol",source_protocol(cfg));cJSON_AddItemToArray(all,copy);
            }
        }
        cJSON *installed_games=cJSON_CreateArray(),*local;cJSON_ArrayForEach(local,cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(o,"installed"),"games")){
            const char *id=json_text(local,"titleId");if(!strcmp(id,"PPSA99005"))continue;
            bool present=false;cJSON *existing;cJSON_ArrayForEach(existing,installed_games)if(!strcmp(json_text(existing,"titleId"),id)){present=true;break;}
            if(present)continue;
            cJSON *copy=cJSON_Duplicate(local,true);set_text(copy,"id",*json_text(local,"localId")?json_text(local,"localId"):id);
            set_text(copy,"title",*json_text(local,"title")?json_text(local,"title"):id);
            set_text(copy,"sourceId","local");set_text(copy,"sourceName",json_text(local,"location"));set_text(copy,"sourceProtocol","local");
            cJSON_ArrayForEach(existing,all)if(!strcmp(json_text(existing,"titleId"),id)){
                const char *keys[]={"cover","minimumFirmware","region","backportFiles"};
                for(size_t k=0;k<sizeof keys/sizeof *keys;k++){const cJSON *value=cJSON_GetObjectItemCaseSensitive(existing,keys[k]);if(value&&!cJSON_HasObjectItem(copy,keys[k]))cJSON_AddItemToObject(copy,keys[k],cJSON_Duplicate(value,true));}
                break;
            }
            cJSON_AddBoolToObject(copy,"localOnly",true);cJSON_AddItemToArray(installed_games,copy);
        }
        cJSON_AddItemToObject(o,"installedGames",installed_games);
        cJSON_AddItemToObject(o,"games",all);
    }
    cJSON_AddNumberToObject(o, "revision", revision);
    if (job) {
        cJSON *j = cJSON_Duplicate(job, true);
        set_number(j, "speedBytesPerSecond", busy && !strcmp(json_text(job, "status"), "copying") && monotonic_seconds() - speed_updated < 3 ? transfer_speed : 0);
        cJSON_DeleteItemFromObjectCaseSensitive(j, "files");
        cJSON_AddItemToObject(o, "job", j);
    }
    cJSON_AddBoolToObject(o, "busy", busy);
    cJSON_AddBoolToObject(o, "available", started);
    cJSON_AddBoolToObject(o, "remember", view?cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(view,"remember")):remember);
    cJSON_AddBoolToObject(o, "hasPassword", view?*json_text(view,"password")!=0:*password != 0);
    const cJSON *installed=cJSON_GetObjectItemCaseSensitive(o,"installed");
    bool local_done=!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(installed,"sourceChecking"))&&!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(installed,"checking"));
    cJSON_AddStringToObject(o, "message", local_done&&!strcmp(message,"Refreshing installed games...")?"Installed games refreshed.":message);
    pthread_mutex_unlock(&lock); return o;
}
int smb_action(const cJSON *input, char *error, size_t cap) {
    const char *action = json_text(input, "action");
    int code = 202;
    pthread_mutex_lock(&lock);
    if (!started || stopping) { code = 503; copy_text(error, cap, "Server worker is unavailable."); goto end; }
    if (!strcmp(action, "pause") || !strcmp(action, "cancel")) {
        if (!job || strcmp(json_text(job, "status"), "copying")) { code = 409; goto end; }
        if(!strcmp(action,"pause")&&!strcmp(json_text(job,"direction"),"upload")){code=409;copy_text(error,cap,"Server backups can be cancelled, then restarted as a new backup.");goto end;}
        pause_requested = !strcmp(action, "pause"); cancel_requested = !strcmp(action, "cancel"); goto end;
    }
    if (busy) { code = 409; copy_text(error, cap, "Wait for the server operation to finish."); goto end; }
    if(!strcmp(action,"refreshInstalled")){installed_refresh(NULL);copy_text(message,sizeof message,"Refreshing installed games...");goto end;}
    if(!strcmp(action,"resume")&&!strcmp(json_text(job,"direction"),"upload")){code=409;copy_text(error,cap,"Select the installed game and start a new server backup.");goto end;}
    if(!strcmp(action,"export")){
        if(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input,"confirmed"))){code=400;copy_text(error,cap,"Confirm copying this installed game to the server.");goto end;}
        sync_source();cJSON *profile=source_by_id(json_text(input,"sourceId"));
        const cJSON *cfg=cJSON_GetObjectItemCaseSensitive(profile,"settings");
        if(!profile||!*json_text(cfg,"server")||(!strcmp(source_protocol(cfg),"webdav")||!strcmp(source_protocol(cfg),"webdavs"))){code=400;copy_text(error,cap,"Choose a configured SMB or FTP server.");goto end;}
        cJSON *inventory=installed_snapshot(),*candidate,*next=NULL;
        cJSON_ArrayForEach(candidate,cJSON_GetObjectItemCaseSensitive(inventory,"games"))
            if(*json_text(input,"localId")&&!strcmp(json_text(candidate,"localId"),json_text(input,"localId"))&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(candidate,"canExport")))next=cJSON_Duplicate(candidate,true);
        cJSON_Delete(inventory);
        if(!next){code=409;copy_text(error,cap,"No readable installed source. Refresh the local library first.");goto end;}
        pthread_mutex_lock(&atmosphere.mutex);bool active=atmosphere.library_storage_busy;
        for(size_t i=0;i<atmosphere.job_count;i++)if(!strcmp(atmosphere.jobs[i].status,"downloading")||!strcmp(atmosphere.jobs[i].status,"verifying"))active=true;
        if(!active)atmosphere.smb_storage_busy=true;pthread_mutex_unlock(&atmosphere.mutex);
        if(active){cJSON_Delete(next);code=409;copy_text(error,cap,"Wait for the storage operation to finish.");goto end;}
        load_source(profile);char id[24];random_hex(id,8);set_text(next,"id",id);set_text(next,"sourceId",active_source);
        set_text(next,"direction","upload");set_text(next,"status","copying");set_text(next,"phase","Preparing server backup");set_number(next,"received",0);
        cJSON_AddBoolToObject(next,"skipVerification",cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input,"skipVerification")));
        cJSON *old=job;job=next;
        if(save_locked()){job=old;cJSON_Delete(next);code=503;copy_text(error,cap,"Cannot save the backup job.");pthread_mutex_lock(&atmosphere.mutex);atmosphere.smb_storage_busy=false;pthread_mutex_unlock(&atmosphere.mutex);goto end;}
        cJSON_Delete(old);pending=3;busy=true;pause_requested=cancel_requested=false;transfer_speed=0;speed_updated=monotonic_seconds();
        copy_text(message,sizeof message,"Preparing server backup...");pthread_cond_signal(&changed);goto end;
    }
    if(!strcmp(action,"duplicateSource")){
        if(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input,"confirmed"))){code=400;copy_text(error,cap,"Confirm duplicating this server first.");goto end;}
        if(cJSON_GetArraySize(sources)>=8){code=400;copy_text(error,cap,"Up to eight servers are supported.");goto end;}
        sync_source();cJSON *original=source_by_id(json_text(input,"sourceId"));
        if(!original){code=404;copy_text(error,cap,"Server not found.");goto end;}
        cJSON *copy=cJSON_Duplicate(original,true);if(!copy){code=503;goto end;}
        char previous[24],id[24],name[256];copy_text(previous,sizeof previous,active_source);random_hex(id,8);
        set_text(copy,"id",id);cJSON_DeleteItemFromObjectCaseSensitive(copy,"enabled");cJSON_AddBoolToObject(copy,"enabled",false);
        cJSON_DeleteItemFromObjectCaseSensitive(copy,"games");cJSON_AddArrayToObject(copy,"games");
        cJSON *cfg=cJSON_GetObjectItemCaseSensitive(copy,"settings");
        snprintf(name,sizeof name,"%.240s (copy)",*json_text(cfg,"name")?json_text(cfg,"name"):json_text(cfg,"server"));set_text(cfg,"name",name);
        cJSON_AddItemToArray(sources,copy);load_source(copy);
        if(save_locked()){
            load_source(source_by_id(previous));cJSON_DeleteItemFromArray(sources,cJSON_GetArraySize(sources)-1);
            code=503;copy_text(error,cap,"Cannot save the copied server.");
        }else{revision++;copy_text(message,sizeof message,"Server copied. Edit its settings, then activate it.");}
        goto end;
    }
    if (!strcmp(action, "deleteSource")) {
        int index = 0, found = -1; cJSON *p;
        cJSON_ArrayForEach(p, sources) {
            if (!strcmp(json_text(p, "id"), json_text(input, "sourceId"))) { found = index; break; }
            index++;
        }
        if (found < 0) { code = 404; copy_text(error, cap, "server not found."); goto end; }
        cJSON *previous = capture_source();
        if (!previous) { code = 503; copy_text(error, cap, "Cannot prepare source removal."); goto end; }
        cJSON *removed = cJSON_DetachItemFromArray(sources, found);
        bool active = !strcmp(json_text(removed, "id"), active_source);
        if (active) load_source(cJSON_GetArrayItem(sources, 0));
        if (save_locked()) {
            cJSON_InsertItemInArray(sources, found, removed); load_source(previous);
            code = 503; copy_text(error, cap, "Cannot save servers.");
        } else { cJSON_Delete(removed); revision++; copy_text(message, sizeof message, "Source removed. Game files were not deleted."); }
        cJSON_Delete(previous); goto end;
    }
    if (!strcmp(action,"deactivateSource")) {
        cJSON *target=source_by_id(json_text(input,"sourceId"));
        if(!target){code=404;goto end;}
        bool prior=source_enabled(target);
        cJSON_DeleteItemFromObjectCaseSensitive(target,"enabled");cJSON_AddBoolToObject(target,"enabled",false);
        if(save_locked()){target=source_by_id(json_text(input,"sourceId"));cJSON_ReplaceItemInObjectCaseSensitive(target,"enabled",cJSON_CreateBool(prior));code=503;}
        else{revision++;copy_text(message,sizeof message,"Server deactivated. Other active servers remain in the library.");}
        goto end;

    }
    if (!strcmp(action, "addSource") || !strcmp(action, "selectSource")) {
        bool adding = !strcmp(action, "addSource");
        if (adding && cJSON_GetArraySize(sources) >= 8) { code = 400; copy_text(error, cap, "Up to eight servers are supported."); goto end; }
        cJSON *target = NULL, *p; int current_index = -1, index = 0;
        cJSON_ArrayForEach(p, sources) {
            if (!strcmp(json_text(p, "id"), active_source)) current_index = index;
            if (!strcmp(json_text(p, "id"), json_text(input, "sourceId"))) target = p;
            index++;
        }
        if (!adding && !target) { code = 404; copy_text(error, cap, "server not found."); goto end; }
        if (!adding && !strcmp(json_text(target, "id"), active_source)) {
            cJSON_DeleteItemFromObjectCaseSensitive(target,"enabled");cJSON_AddBoolToObject(target,"enabled",true);
            if(save_locked())code=503;else revision++;goto end;
        }
        cJSON *previous = capture_source();
        if (current_index >= 0) cJSON_ReplaceItemInArray(sources, current_index, cJSON_Duplicate(previous, true));
        if (adding) {
            target = cJSON_CreateObject(); char id[24]; random_hex(id, 8);
            cJSON_AddStringToObject(target, "id", id);
            cJSON_AddBoolToObject(target,"enabled",true);
            cJSON_AddItemToObject(target, "settings", cJSON_CreateObject());
            cJSON_AddItemToObject(target, "games", cJSON_CreateArray());
            cJSON_AddItemToArray(sources, target);
        }
        cJSON_DeleteItemFromObjectCaseSensitive(target,"enabled");cJSON_AddBoolToObject(target,"enabled",true);
        load_source(target);
        if (save_locked()) {
            load_source(previous);
            if (adding) cJSON_DeleteItemFromArray(sources, cJSON_GetArraySize(sources) - 1);
            code = 503; copy_text(error, cap, "Cannot save servers.");
        } else { revision++; message[0] = 0; }
        cJSON_Delete(previous); goto end;
    }
    if (!strcmp(action, "configure")) {
        if (!cJSON_GetArraySize(sources) || !*active_source) { code = 400; copy_text(error, cap, "Activate or add a server first."); goto end; }
        const char *server = json_text(input, "server"), *share = json_text(input, "share");
        const char *protocol=json_text(input,"protocol"),*port=json_text(input,"port");
        bool ftp=!strcmp(protocol,"ftp"),dav=!strcmp(protocol,"webdav")||!strcmp(protocol,"webdavs");
        if((*protocol && strcmp(protocol,"smb") && !ftp && !dav) || strlen(port)>5 || (*port && (strspn(port,"0123456789")!=strlen(port) || strtoul(port,NULL,10)<1 || strtoul(port,NULL,10)>65535))) {
            code=400;copy_text(error,cap,"Choose SMB, FTP or WebDAV and a port from 1 to 65535.");goto end;
        }
        const char *folder = json_text(input, "folder");
        char destination[SMB_PATH];copy_text(destination,sizeof destination,json_text(input,"destinationFolder"));
        if(!*destination){
            const char *editing=json_text(input,"sourceId");const cJSON *profile=source_by_id(editing);
            const cJSON *cfg=*editing&&strcmp(editing,active_source)?cJSON_GetObjectItemCaseSensitive(profile,"settings"):settings;
            copy_text(destination,sizeof destination,*json_text(cfg,"destinationFolder")?json_text(cfg,"destinationFolder"):"homebrew");
        }
        if (!*server || strlen(server) > 253 || strpbrk(server, "/\\@?# \t\r\n") ||
            (!ftp && !dav && !component(share)) || !relative(folder) || !relative(destination) || strlen(destination) > 240 ||
            !strncmp(destination, ".atmosphere-smb-staging", sizeof ".atmosphere-smb-staging"-1) || strlen(json_text(input, "username")) > 255 ||
            strlen(json_text(input, "domain")) > 255 || strlen(json_text(input, "password")) > 255) {
            code = 400; copy_text(error, cap, "Enter a server, an SMB share when applicable, and a folder without a leading slash or '..'."); goto end;
        }
        const char *edit_id=json_text(input,"sourceId");
        if(*edit_id){
            sync_source();cJSON *target=source_by_id(edit_id);
            if(!target){code=404;copy_text(error,cap,"Server not found.");goto end;}
            load_source(target);
        }
        cJSON *old = settings; settings = cJSON_CreateObject();
        const char *keys[] = {"server", "share", "folder", "username", "domain", "protocol", "port"};
        for (size_t i = 0; i < 7; i++) cJSON_AddStringToObject(settings, keys[i], json_text(input, keys[i]));
        cJSON_AddStringToObject(settings, "destinationFolder", destination);
        cJSON_AddStringToObject(settings, "name", json_text(input, "name"));
        char previous[256]; copy_text(previous, sizeof previous, password); bool prior = remember;
        /* Omitted password keeps the current one; an explicit empty string clears it. */
        if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(input, "password")))
            copy_text(password, sizeof password, json_text(input, "password"));
        else if (strcmp(json_text(old, "server"), server) ||
                 strcmp(json_text(old, "username"), json_text(input, "username")) ||
                 strcmp(source_protocol(old),source_protocol(input)) || strcmp(json_text(old,"port"),port))
            password[0] = 0;
        remember = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input, "remember"));
        bool source_changed = strcmp(json_text(old, "server"), server) || strcmp(json_text(old, "share"), share) ||
                              strcmp(json_text(old, "folder"), folder) || strcmp(source_protocol(old),source_protocol(input)) || strcmp(json_text(old,"port"),port);
        cJSON *old_games = games;
        if (source_changed) games = cJSON_CreateArray();
        if (save_locked()) {
            cJSON_Delete(settings); settings = old; copy_text(password, sizeof password, previous); remember = prior;
            if (source_changed) { cJSON_Delete(games); games = old_games; }
            code = 503; copy_text(error, cap, "Cannot save server settings.");
        } else { cJSON_Delete(old); if (source_changed) cJSON_Delete(old_games); revision++; }
        memset(previous, 0, sizeof previous); goto end;
    }
    if(!strcmp(action,"copy")||!strcmp(action,"resume")){
        const char *id=!strcmp(action,"resume")?json_text(job,"sourceId"):json_text(input,"sourceId");
        if(*id){sync_source();cJSON *p=source_by_id(id);if(!p||!source_enabled(p)){code=409;copy_text(error,cap,"Activate the game's server first.");goto end;}load_source(p);}
    }
    if (strcmp(action,"scan") && !*json_text(settings, "server")) { code = 400; copy_text(error, cap, "Configure a server first."); goto end; }
    if (!strcmp(action, "scan")) { load_scan_paths();installed_refresh(json_text(settings,"destinationFolder")); pending = 1; busy = true; }
    else if (!strcmp(action, "copy") || !strcmp(action, "resume")) {
        bool resume = !strcmp(action, "resume");
        if (resume && (!job || !strcmp(json_text(job, "status"), "complete") ||
            strcmp(json_text(job, "server"), json_text(settings, "server")) ||
            strcmp(json_text(job, "share"), json_text(settings, "share")) ||
            strcmp(source_protocol(job),source_protocol(settings)) ||
            strcmp(json_text(job,"port"),json_text(settings,"port")))) {
            code = 409; copy_text(error, cap, "Reconnect to the original server and protocol before resuming."); goto end;
        }
        pthread_mutex_lock(&atmosphere.mutex);
        bool active = atmosphere.library_storage_busy;
        for (size_t i = 0; i < atmosphere.job_count; i++)
            if (!strcmp(atmosphere.jobs[i].status, "downloading") || !strcmp(atmosphere.jobs[i].status, "verifying")) active = true;
        if (!active) atmosphere.smb_storage_busy = true;
        pthread_mutex_unlock(&atmosphere.mutex);
        if (active) { code = 409; copy_text(error, cap, "Wait for the current download or library storage operation."); goto end; }
        cJSON *next = NULL;
        if (resume) { next = cJSON_Duplicate(job, true); cJSON_DeleteItemFromObjectCaseSensitive(next, "resuming"); cJSON_AddBoolToObject(next, "resuming", true); }
        else {
            cJSON *g = NULL, *candidate;
            cJSON_ArrayForEach(candidate, games)
                if (!strcmp(json_text(candidate, "id"), json_text(input, "gameId"))) g = candidate;
            Storage drives[ATMOSPHERE_MAX_STORAGE], *drive = NULL; size_t count = storage_list(drives);
            for (size_t i = 0; i < count; i++) if (!strcmp(drives[i].id, json_text(input, "storageId"))) drive = &drives[i];
            bool usb_root = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input, "usbRoot"));
            const cJSON *requested=cJSON_GetObjectItemCaseSensitive(input,"destinationFolder");
            const char *folder=cJSON_IsString(requested)?requested->valuestring:usb_root?"":json_text(settings,"destinationFolder");
            if(!cJSON_IsString(requested)&&!usb_root&&!*folder)folder="homebrew";
            bool allowed=!cJSON_IsString(requested);
            if(drive&&cJSON_IsString(requested)){
                cJSON *options=drive_destinations(drive),*option;
                cJSON_ArrayForEach(option,options)if(!strcmp(json_text(option,"folder"),folder)&&
                    (!*json_text(option,"sourceId")||!strcmp(json_text(option,"sourceId"),active_source)))allowed=true;
                cJSON_Delete(options);usb_root=!*folder;
            }
            if (!g || !drive) { code = 400; copy_text(error, cap, "Choose a scanned game and connected destination."); }
            else if(!allowed||!relative(folder)){code=400;copy_text(error,cap,"Choose a listed destination folder for this drive.");}
            else if(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input,"allowDuplicate")) && installed_match(json_text(g,"titleId"))) {
                code=409;copy_text(error,cap,"This title ID is already installed. Confirm Copy anyway to create another copy.");
            }
            else if (usb_root && !drive->external && !atmosphere.desktop) { code = 400; copy_text(error, cap, "Root placement is only available for USB storage."); }
            else {
                next = cJSON_Duplicate(g, true); char id[24]; random_hex(id, 8); set_text(next, "id", id);
                set_text(next,"sourceId",active_source);
                set_text(next, "server", json_text(settings, "server")); set_text(next, "share", json_text(settings, "share"));
                set_text(next,"protocol",json_text(settings,"protocol"));set_text(next,"port",json_text(settings,"port"));
                set_text(next, "root", drive->root); set_text(next, "storageId", drive->id);
                set_text(next, "destinationFolder", folder);
                cJSON_AddBoolToObject(next, "usbRoot", usb_root);
                set_identity(next, "device", (uint64_t)drive->device); set_identity(next, "inode", (uint64_t)drive->inode);
                cJSON_DeleteItemFromObjectCaseSensitive(next, "cover");
            }
        }
        if (next) {
            cJSON *old = job; job = next; transfer_speed = 0; set_text(job, "phase", "Preparing"); set_text(job, "status", "copying"); set_text(job, "error", ""); set_number(job, "received", 0);
            cJSON *old_preferences=cJSON_Duplicate(destination_preferences,true);
            if(!resume)set_text(destination_preferences,json_text(job,"root"),json_text(job,"destinationFolder"));
            if (save_locked()) { cJSON_Delete(job); job = old;cJSON_Delete(destination_preferences);destination_preferences=old_preferences;old_preferences=NULL;code = 503; copy_text(error, cap, "Cannot save the copy job."); }
            else { cJSON_Delete(old); pending = 2; busy = true;
                installed_refresh(json_text(job,"destinationFolder"));
            }
            cJSON_Delete(old_preferences);
        }
        if (!busy) { pthread_mutex_lock(&atmosphere.mutex); atmosphere.smb_storage_busy = false; pthread_mutex_unlock(&atmosphere.mutex); }
    } else { code = 400; copy_text(error, cap, "Unknown server action."); }
    if (busy) { pause_requested = cancel_requested = false; copy_text(message, sizeof message, pending == 1 ? "Scanning share…" : "Copying and verifying…"); pthread_cond_signal(&changed); }
end:
    if (code >= 400 && !*error) copy_text(error, cap, "This server action is not available right now.");
    pthread_mutex_unlock(&lock); return code;
}
int smb_start(void) {
    bool saved_empty_sources = false;
    settings = cJSON_CreateObject(); games = cJSON_CreateArray(); sources = cJSON_CreateArray();
    destination_preferences=cJSON_CreateObject();load_scan_paths();
    char path[1024]; snprintf(path, sizeof path, "%s/%s", atmosphere.state_dir, state_name);
    unsigned char *data = NULL; size_t length;
    if (!read_regular_file(path, 256 * 1024 * 1024, &data, &length)) {
        cJSON *saved = cJSON_Parse((char *)data); free(data);
        cJSON *prefs=cJSON_GetObjectItemCaseSensitive(saved,"destinationPreferences");
        if(cJSON_IsObject(prefs)){cJSON_Delete(destination_preferences);destination_preferences=cJSON_Duplicate(prefs,true);}
        cJSON *cfg = cJSON_GetObjectItemCaseSensitive(saved, "settings"), *list = cJSON_GetObjectItemCaseSensitive(saved, "games");
        if (cJSON_IsObject(cfg)) {
            cJSON_Delete(settings); settings = cJSON_Duplicate(cfg, true);
            cJSON_Delete(games); games = cJSON_IsArray(list) ? cJSON_Duplicate(list, true) : cJSON_CreateArray();
            cJSON *j = cJSON_GetObjectItemCaseSensitive(saved, "job");
            if (cJSON_IsObject(j)) { job = cJSON_Duplicate(j, true); if (!strcmp(json_text(job, "status"), "copying")) {
                bool upload=!strcmp(json_text(job,"direction"),"upload");set_text(job,"status",upload?"error":"paused");
                if(upload){set_text(job,"phase","Server backup interrupted");set_text(job,"error","Local original kept. Start a new backup; any partial server folder remains hidden.");}
            } }
            remember = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(saved, "remember"));
            if (remember) copy_text(password, sizeof password, json_text(saved, "password"));
        }
        cJSON *profiles = cJSON_GetObjectItemCaseSensitive(saved, "sources");
        saved_empty_sources = cJSON_IsArray(profiles) && cJSON_GetArraySize(profiles) == 0;
        if (saved_empty_sources) load_source(NULL);
        if (cJSON_IsArray(profiles) && cJSON_GetArraySize(profiles) > 0 && cJSON_GetArraySize(profiles) <= 8) {
            cJSON *p;
            cJSON_ArrayForEach(p, profiles) {
                if (!*json_text(p, "id") || !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(p, "settings"))) continue;
                cJSON *copy = cJSON_Duplicate(p, true);
                if (!copy) continue;
                if (!cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(copy, "games"))) {
                    cJSON_DeleteItemFromObjectCaseSensitive(copy, "games");
                    cJSON_AddArrayToObject(copy, "games");
                }
                if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(copy, "remember"))) cJSON_DeleteItemFromObjectCaseSensitive(copy, "password");
                cJSON_AddItemToArray(sources, copy);
            }
            cJSON_ArrayForEach(p, sources) if (!strcmp(json_text(p, "id"), json_text(saved, "activeSourceId"))) { load_source(p); break; }
            if(!*json_text(saved,"activeSourceId"))load_source(NULL);
        }
        cJSON_Delete(saved);
    }
    if (!cJSON_GetArraySize(sources) && !saved_empty_sources) cJSON_AddItemToArray(sources, capture_source());
    cJSON *migration;cJSON_ArrayForEach(migration,sources)if(!cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(migration,"enabled")))cJSON_AddBoolToObject(migration,"enabled",!strcmp(json_text(migration,"id"),active_source));
    installed_start();
    installed_refresh("etaHEN/games");
    cJSON *pref;cJSON_ArrayForEach(pref,destination_preferences)if(cJSON_IsString(pref)&&relative(pref->valuestring))installed_refresh(pref->valuestring);
    cJSON *profile;cJSON_ArrayForEach(profile,sources)installed_refresh(json_text(cJSON_GetObjectItemCaseSensitive(profile,"settings"),"destinationFolder"));
    installed_refresh(json_text(settings,"destinationFolder"));
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) return -1;
    int rc = pthread_attr_setstacksize(&attr, ATMOSPHERE_THREAD_STACK);
    if (!rc) rc = pthread_create(&thread, &attr, worker, NULL);
    pthread_attr_destroy(&attr); started = !rc; return rc;
}
void smb_stop(void) {
    pthread_mutex_lock(&lock); stopping = true; pthread_cond_signal(&changed); pthread_mutex_unlock(&lock);
    if (started) pthread_join(thread, NULL);
    installed_stop();
    cJSON_Delete(settings); cJSON_Delete(games); cJSON_Delete(job); cJSON_Delete(sources); memset(password, 0, sizeof password);
}
