#include "network.h"
#include "atmosphere.h"
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
Atmosphere atmosphere = {.mutex = PTHREAD_MUTEX_INITIALIZER,
               .changed = PTHREAD_COND_INITIALIZER,
               .port = ATMOSPHERE_HTTP_PORT,
               .state_fd = -1};
void copy_text(char *to, size_t cap, const char *s) {
    if (cap)
        snprintf(to, cap, "%s", s ? s : "");
}
const char *json_text(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}
int64_t json_int(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int64_t)v->valuedouble : 0;
}
static uint64_t json_identity(const cJSON *o, const char *key) {
    const char *s = json_text(o, key);
    return *s ? strtoumax(s, NULL, 10) : (uint64_t)json_int(o, key);
}
void random_hex(char *out, size_t n) {
    unsigned char b[32];
    if (n > 32 || RAND_bytes(b, (int)n) != 1) {
        fputs("Secure randomness unavailable\n", stderr);
        exit(1);
    }
    for (size_t i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02x", b[i]);
}
Release *release_find(const char *id) {
    for (size_t i = 0; i < atmosphere.release_count; i++)
        if (!strcmp(atmosphere.releases[i].id, id))
            return &atmosphere.releases[i];
    return NULL;
}
Job *job_find(const char *id) {
    for (size_t i = 0; i < atmosphere.job_count; i++)
        if (!strcmp(atmosphere.jobs[i].id, id))
            return &atmosphere.jobs[i];
    return NULL;
}
static cJSON *job_json(const Job *j, bool private_fields) {
    cJSON *o = cJSON_CreateObject();
#define S(k, f) cJSON_AddStringToObject(o, k, j->f)
    S("id", id);
    S("releaseId", release_id);
    S("storageId", storage_id);
    S("filename", filename);
    S("status", status);
    S("error", error);
    S("verification", verification);
    cJSON_AddNumberToObject(o, "received", (double)j->received);
    cJSON_AddNumberToObject(o, "total", (double)j->total);
    cJSON_AddNumberToObject(o, "speed", j->speed);
    cJSON_AddNumberToObject(o, "retryAt", (double)j->retry_at);
    cJSON_AddNumberToObject(o, "order", j->order);
    cJSON_AddStringToObject(o, "title", j->release.title);
    cJSON_AddStringToObject(o, "titleId", j->release.title_id);
    cJSON_AddStringToObject(o, "format", j->release.format);
    cJSON_AddStringToObject(o, "gameId", j->release.game_id);
    char path[700];
    snprintf(path, sizeof path, "%s/homebrew/%s", j->root, j->filename);
    cJSON_AddStringToObject(o, "path", path);
    if (private_fields) {
        cJSON_AddItemToObject(o, "release", release_json(&j->release));
        S("root", root);
        S("etag", etag);
        S("modified", modified);
        char device[32], inode[32];
        snprintf(device, sizeof device, "%ju", (uintmax_t)j->device);
        snprintf(inode, sizeof inode, "%ju", (uintmax_t)j->inode);
        cJSON_AddStringToObject(o, "device", device);
        cJSON_AddStringToObject(o, "inode", inode);
        cJSON_AddNumberToObject(o, "attempts", j->attempts);
    }
#undef S
    return o;
}
cJSON *jobs_json_locked(void) {
    cJSON *a = cJSON_CreateArray();
    /* Sort references only. The transfer worker retains a pointer to its fixed slot. */
    const Job *ordered[ATMOSPHERE_MAX_JOBS];
    size_t count = 0;
    for (size_t i = 0; i < atmosphere.job_count; i++) {
        const Job *j = &atmosphere.jobs[i];
        if (!j->id[0])
            continue;
        size_t p = count++;
        while (p && ordered[p - 1]->order > j->order) {
            ordered[p] = ordered[p - 1];
            p--;
        }
        ordered[p] = j;
    }
    for (size_t i = 0; i < count; i++)
        cJSON_AddItemToArray(a, job_json(ordered[i], false));
    return a;
}
int state_save_locked(void) {
    cJSON *o = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(o, "jobs"),
          *t = cJSON_AddArrayToObject(o, "tokens");
    cJSON_AddNumberToObject(o, "schema", 1);
    cJSON_AddStringToObject(o, "preferredStorage", atmosphere.preferred_storage);
    cJSON_AddItemToObject(o, "favorites", favorites_json_locked());
    cJSON *sources = cJSON_AddObjectToObject(o, "sources");
    cJSON_AddNumberToObject(sources, "enabled", atmosphere.sources.enabled);
    cJSON_AddNumberToObject(sources, "noticeVersion", atmosphere.sources.notice_version);
    cJSON_AddNumberToObject(sources, "acknowledgedAt", (double)atmosphere.sources.acknowledged_at);
    for (size_t i = 0; i < atmosphere.job_count; i++)
        if (atmosphere.jobs[i].id[0])
            cJSON_AddItemToArray(a, job_json(&atmosphere.jobs[i], true));
    for (size_t i = 0; i < atmosphere.token_count; i++)
        cJSON_AddItemToArray(t, cJSON_CreateString(atmosphere.tokens[i]));
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s)
        return -1;
    int fd = openat(atmosphere.state_fd, "state.tmp", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600),
        rc = -1;
    if (fd >= 0) {
        size_t n = strlen(s), p = 0;
        while (p < n) {
            ssize_t w = write(fd, s + p, n - p);
            if (w <= 0)
                break;
            p += (size_t)w;
        }
        if (p == n && fsync(fd) == 0)
            rc = 0;
        if (close(fd) != 0)
            rc = -1;
    }
    free(s);
    if (rc == 0)
        rc = renameat(atmosphere.state_fd, "state.tmp", atmosphere.state_fd, "state.json");
    if (rc == 0)
        fsync(atmosphere.state_fd);
    atmosphere.state_failed = rc != 0;
    return rc;
}
int state_load(void) {
    int fd = openat(atmosphere.state_fd, "state.json", O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return 0;
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 1 || st.st_size > 1024 * 1024) {
        close(fd);
        return -1;
    }
    size_t n = (size_t)st.st_size;
    char *s = calloc(n + 1, 1);
    if (!s) {
        close(fd);
        return -1;
    }
    size_t p = 0;
    while (p < n) {
        ssize_t got = read(fd, s + p, n - p);
        if (got <= 0)
            break;
        p += (size_t)got;
    }
    close(fd);
    cJSON *o = p == n ? cJSON_Parse(s) : NULL;
    free(s);
    if (!o || json_int(o, "schema") != 1) {
        cJSON_Delete(o);
        return -1;
    }
    const cJSON *x;
    copy_text(atmosphere.preferred_storage, sizeof atmosphere.preferred_storage,
              json_text(o, "preferredStorage"));
    const cJSON *sources = cJSON_GetObjectItemCaseSensitive(o, "sources");
    if (json_int(sources, "noticeVersion") == ATMOSPHERE_SOURCE_NOTICE_VERSION &&
        json_int(sources, "acknowledgedAt") > 0 && json_int(sources, "enabled") >= 0 &&
        json_int(sources, "enabled") <= (ATMOSPHERE_SOURCE_ARCHIVE | ATMOSPHERE_SOURCE_VIKINGFILE)) {
        atmosphere.sources.enabled = (unsigned)json_int(sources, "enabled");
        atmosphere.sources.notice_version = ATMOSPHERE_SOURCE_NOTICE_VERSION;
        atmosphere.sources.acknowledged_at = (time_t)json_int(sources, "acknowledgedAt");
    }
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(o, "tokens")) {
        if (cJSON_IsString(x) && strlen(x->valuestring) == 64 && atmosphere.token_count < 16)
            copy_text(atmosphere.tokens[atmosphere.token_count++], 65, x->valuestring);
    }
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(o, "favorites")) {
        if (!cJSON_IsString(x) || !*x->valuestring || strlen(x->valuestring) >= 64 ||
            atmosphere.favorite_count >= ATMOSPHERE_MAX_RELEASES)
            continue;
        bool duplicate = false;
        for (size_t i = 0; i < atmosphere.favorite_count; i++)
            if (!strcmp(atmosphere.favorites[i], x->valuestring))
                duplicate = true;
        if (!duplicate)
            copy_text(atmosphere.favorites[atmosphere.favorite_count++], 64, x->valuestring);
    }
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(o, "jobs")) {
        Release snapshot, *r = release_find(json_text(x, "releaseId"));
        const cJSON *saved = cJSON_GetObjectItemCaseSensitive(x, "release");
        if (saved) {
            /* A bad saved identity must never silently switch to the current file. */
            if (release_parse(saved, &snapshot, true) ||
                strcmp(snapshot.id, json_text(x, "releaseId"))) {
                cJSON_Delete(o);
                return -1;
            }
            r = &snapshot;
        }
        if (!r || atmosphere.job_count >= ATMOSPHERE_MAX_JOBS)
            continue;
        Job *j = &atmosphere.jobs[atmosphere.job_count++];
        j->release = *r;
#define R(k, f) copy_text(j->f, sizeof j->f, json_text(x, k))
        R("id", id);
        R("releaseId", release_id);
        R("storageId", storage_id);
        R("root", root);
        R("status", status);
        R("etag", etag);
        R("modified", modified);
        R("error", error);
        R("verification", verification);
#undef R
        copy_text(j->filename, sizeof j->filename, r->filename);
        j->device = (dev_t)json_identity(x, "device");
        j->inode = (ino_t)json_identity(x, "inode");
        j->received = json_int(x, "received");
        j->total = r->size;
        j->attempts = (unsigned)json_int(x, "attempts");
        int64_t order = json_int(x, "order");
        j->order = order > 0 && order < 2147483647 ? (unsigned)order : (unsigned)atmosphere.job_count;
        j->retry_at = (time_t)json_int(x, "retryAt");
        if (!strcmp(j->status, "downloading") || !strcmp(j->status, "verifying")) {
            copy_text(j->status, sizeof j->status, "paused");
            copy_text(j->error, sizeof j->error, "Interrupted. Resume to continue.");
        }
        if (!source_enabled_locked(r) &&
            (!strcmp(j->status, "queued") || !strcmp(j->status, "retrying") ||
             !strcmp(j->status, "paused"))) {
            copy_text(j->status, sizeof j->status, "paused");
            copy_text(j->error, sizeof j->error,
                      "Source disabled. Enable it in Sources before resuming.");
        }
    }
    cJSON_Delete(o);
    return 0;
}
