#include "atmosphere.h"
#include <ctype.h>
#include <curl/curl.h>
#include <math.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef ATMOSPHERE_NATIVE_APP
static void capacity_diagnostic(const char *name,const cJSON *data,const char *error) {
    char path[160];snprintf(path,sizeof path,"/app0/atmosphere-state/%s",name);
    char *json=data?cJSON_PrintUnformatted(data):NULL;
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);
    if(fd>=0){const char *text=json?json:error; if(text)write(fd,text,strlen(text));close(fd);}
    free(json);
}
#endif

/* ShadowMount's documented loopback API only. Mutations are explicit, capability
 * checked and revalidated in the worker. Browser paths/URLs are never forwarded. */
#define LIBRARY_LIMIT (8U * 1024U * 1024U)
#define LIBRARY_GAMES 4096
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static bool started, stopping, pending, busy;
static cJSON *games;
static time_t attempted, checked, updated;
static time_t refreshed;
static char status[24] = "idle", message[256], version[48];
static cJSON *capabilities, *storage, *storage_job, *command, *last_request, *action_result;
static bool storage_pending, storage_busy, storage_stale = true, monitor_job;
static time_t storage_attempted, storage_updated, job_poll_at;
static char storage_error[256];

static bool success(const cJSON *o);
static void read_storage(int port);
static void read_job(int port);
static void execute_command(cJSON *input);

static time_t monotonic_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec;
}

typedef struct { char *data; size_t length; } Buffer;
static bool is_stopping(void) {
    pthread_mutex_lock(&lock);
    bool stop = stopping;
    pthread_mutex_unlock(&lock);
    return stop;
}
static int progress(void *context, curl_off_t total, curl_off_t received,
                    curl_off_t upload_total, curl_off_t uploaded) {
    (void)context; (void)total; (void)received;
    (void)upload_total; (void)uploaded;
    return is_stopping();
}
static size_t receive(void *data, size_t size, size_t count, void *context) {
    Buffer *b = context;
    if (size && count > LIBRARY_LIMIT / size) return 0;
    size_t n = size * count;
    if (n > LIBRARY_LIMIT - b->length) return 0;
    char *next = realloc(b->data, b->length + n + 1);
    if (!next) return 0;
    b->data = next;
    memcpy(b->data + b->length, data, n);
    b->length += n;
    b->data[b->length] = 0;
    return n;
}
static cJSON *read_api(int port, const char *route, const char *body,
                       char *error, size_t cap) {
    if (is_stopping()) return NULL;
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/api/v1/%s", port, route);
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    Buffer buffer = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_PROXY, "");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 750L);
    /* app.db and drive availability checks can take several seconds on a console.
     * This is a cancellable background read, not an HTTP request-thread wait. */
    bool mutation = !strcmp(route, "scan") || !strcmp(route, "games/mount") ||
        !strcmp(route, "games/unmount") || !strcmp(route, "games/move") || !strcmp(route, "games/copy") || !strcmp(route, "games/delete");
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, mutation || !strcmp(route, "games/info") ||
        (!strcmp(route, "games") && strstr(body, "true")) ? 60000L :
        !strcmp(route, "games") ? 10000L : 3000L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buffer);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    cJSON *result = rc == CURLE_OK && (code == 200 || code == 202) && buffer.data
        ? cJSON_ParseWithLengthOpts(buffer.data, buffer.length + 1, NULL, true) : NULL;
    if (!result && error) {
        if (rc == CURLE_OPERATION_TIMEDOUT)
            copy_text(error, cap, "ShadowMount took too long to return its inventory. Refresh to try again.");
        else if (rc != CURLE_OK)
            snprintf(error, cap, "Could not read ShadowMount inventory: %s.", curl_easy_strerror(rc));
        else if (code != 200 && code != 202) {
            cJSON *detail = buffer.data ? cJSON_Parse(buffer.data) : NULL;
            snprintf(error, cap, "ShadowMount: %.180s (HTTP %ld).",
                *json_text(detail, "error") ? json_text(detail, "error") : "request failed", code);
            cJSON_Delete(detail);
        }
        else copy_text(error, cap, "ShadowMount returned invalid inventory JSON.");
    }
    free(buffer.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    return result;
}
static bool has(const cJSON *array, const char *value) {
    const cJSON *v;
    cJSON_ArrayForEach(v, array)
        if (cJSON_IsString(v) && !strcmp(v->valuestring, value)) return true;
    return false;
}
static bool number(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble >= 0 &&
        v->valuedouble <= 9007199254740991.0 && floor(v->valuedouble) == v->valuedouble;
}
static void identity(const char *a, const char *b, const char *c, char out[65]) {
    char data[12300];
    unsigned char digest[SHA256_DIGEST_LENGTH];
    snprintf(data, sizeof data, "%s\n%s\n%s", a, b, c);
    SHA256((const unsigned char *)data, strlen(data), digest);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
}
static void set_gate(bool value) {
    pthread_mutex_lock(&atmosphere.mutex);
    atmosphere.library_storage_busy = value;
    pthread_cond_signal(&atmosphere.changed);
    pthread_mutex_unlock(&atmosphere.mutex);
}
static bool success(const cJSON *o) {
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(o, "status");
    return cJSON_IsObject(o) && cJSON_IsNumber(s) && s->valuedouble == 0;
}
static int api_port(void) {
#ifdef ATMOSPHERE_TEST
    const char *test = getenv("ATMOSPHERE_TEST_LIBRARY_PORT");
    if (test) {
        char *end;
        long n = strtol(test, &end, 10);
        return !*end && n > 0 && n <= 65535 && n != atmosphere.port ? (int)n : 0;
    }
#endif
    /* Desktop previews never discover or connect to a real console. */
    if (atmosphere.desktop) return 0;
    int port = 10101;
    unsigned char *data = NULL;
    size_t length = 0;
    if (!read_regular_file("/data/shadowmount/config.ini", 16384, &data, &length)) {
        char *save = NULL;
        for (char *line = strtok_r((char *)data, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            while (isspace((unsigned char)*line)) line++;
            char key[64], value[64];
            if (sscanf(line, "%63[^=]=%63s", key, value) != 2) continue;
            size_t k = strlen(key);
            while (k && isspace((unsigned char)key[k - 1])) key[--k] = 0;
            if (!strcmp(key, "api_enabled") && !strcmp(value, "0")) { port = 0; break; }
            if (!strcmp(key, "api_port")) {
                char *end;
                long n = strtol(value, &end, 10);
                if (*end || n < 1 || n > 65535) { port = 0; break; }
                port = (int)n;
            }
        }
        free(data);
    }
    return port == atmosphere.port ? 0 : port;
}
int library_api_port(void){return api_port();}
bool library_rescan_after_copy(void) {
    int port=api_port();
    if(!port)return false;
    cJSON *v=read_api(port,"version","{}",NULL,0);
    bool supported=success(v) && json_int(v,"api_version")==1 &&
        has(cJSON_GetObjectItemCaseSensitive(v,"capabilities"),"rescan");
    cJSON_Delete(v);
    if(!supported)return false;
    cJSON *reply=read_api(port,"scan","{\"reset_attempts\":false}",NULL,0);
    bool queued=success(reply);
    cJSON_Delete(reply);
    return queued;
}
static bool text_field(const cJSON *g, const char *key, size_t max) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(g, key);
    if (!cJSON_IsString(v) || strlen(v->valuestring) > max) return false;
    for (const unsigned char *p = (const unsigned char *)v->valuestring; *p; p++)
        if (*p < 32 || *p == 127) return false;
    return true;
}
static bool title_id(const char *id) {
    if (strlen(id) != 9) return false;
    for (int i = 0; i < 9; i++)
        if (i < 4 ? !(id[i] >= 'A' && id[i] <= 'Z') : !isdigit((unsigned char)id[i]))
            return false;
    return true;
}
static const char *format(const cJSON *g) {
    const char *source = json_text(g, "source_type"), *image = json_text(g, "image_type");
    if (!strcmp(source, "pkg")) return "PKG";
    if (!strcmp(source, "folder")) return "Folder";
    if (!strcmp(image, "exfatfs")) return "exFAT";
    if (!strcmp(image, "pfsc")) return "FFPFSC";
    if (!strcmp(image, "pfs")) return "FFPFS";
    if (!strcmp(image, "ufs")) return "FFPKG";
    return "Image";
}
static void location(const char *path, char *out, size_t cap) {
    const char *p = !strncmp(path, "/mnt/usb", 8) ? path + 8 :
                    !strncmp(path, "/mnt/ext", 8) ? path + 8 : NULL;
    if (p && isdigit((unsigned char)p[0]) && p[1] == '/')
        snprintf(out, cap, "%s %c", path[5] == 'u' ? "USB" : "Extended storage", *p);
    else if (!strncmp(path, "/data/", 6) || !strncmp(path, "/user/", 6))
        copy_text(out, cap, "Console storage");
    else if (!strncmp(path, "/mnt/shadowmnt/", 15))
        copy_text(out, cap, "ShadowMount");
    else copy_text(out, cap, "Other location");
}
static cJSON *normalize(const cJSON *response, char *error, size_t cap) {
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(response, "games");
    const cJSON *count = cJSON_GetObjectItemCaseSensitive(response, "count");
    int n = cJSON_GetArraySize(list);
    if (!success(response) || !cJSON_IsArray(list) || n > LIBRARY_GAMES ||
        !cJSON_IsNumber(count) || count->valuedouble != n) {
        copy_text(error, cap, "ShadowMount returned an incomplete inventory list or count.");
        return NULL;
    }
    cJSON *out = cJSON_CreateArray(), *g;
    const char *field = "title ID";
    cJSON_ArrayForEach(g, list) {
        const char *id = json_text(g, "title_id"), *platform = json_text(g, "platform");
        const char *source = json_text(g, "source_type"), *path = json_text(g, "path");
        field = "title ID";
        if (!cJSON_IsObject(g) || !title_id(id)) goto invalid;
        const char *texts[] = {"title_name", "path", "runtime_path", "image_type"};
        const size_t limits[] = {512, 4096, 4096, 32};
        for (size_t i = 0; i < sizeof texts / sizeof *texts; i++) {
            field = texts[i];
            if (!text_field(g, field, limits[i])) goto invalid;
        }
        field = "source path";
        if (*path && path[0] != '/') goto invalid;
        field = "runtime path";
        if (*json_text(g, "runtime_path") && *json_text(g, "runtime_path") != '/') goto invalid;
        field = "platform";
        if (strcmp(platform, "ps5") && strcmp(platform, "ps4") && strcmp(platform, "unknown")) goto invalid;
        field = "source type";
        if (strcmp(source, "folder") && strcmp(source, "image") && strcmp(source, "pkg")) goto invalid;
        const char *flags[] = {"installed", "mounted", "source_available"};
        for (size_t i = 0; i < sizeof flags / sizeof *flags; i++) {
            field = flags[i];
            if (!cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(g, flags[i]))) goto invalid;
        }
        field = "duplicate title ID";
        cJSON *prior;
        cJSON_ArrayForEach(prior, out)
            if (!strcmp(json_text(prior, "titleId"), id)) goto invalid;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "titleId", id);
        cJSON_AddStringToObject(item, "title", *json_text(g, "title_name") ? json_text(g, "title_name") : id);
        cJSON_AddStringToObject(item, "platform", platform);
        cJSON_AddStringToObject(item, "format", format(g));
        cJSON_AddStringToObject(item, "path", path);
        cJSON_AddStringToObject(item, "runtimePath", json_text(g, "runtime_path"));
        char key[65];
        identity(id, path, format(g), key);
        cJSON_AddStringToObject(item, "sourceKey", key);
        cJSON_AddBoolToObject(item, "managed", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "managed")) && strcmp(source, "pkg"));
        cJSON_AddBoolToObject(item, "canManageSource", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "can_manage_source")) && strcmp(source, "pkg"));
        bool measured = number(g, "size_status") && json_int(g, "size_status") == 0 && number(g, "size_bytes");
        if (measured) cJSON_AddNumberToObject(item, "sizeBytes", (double)json_int(g, "size_bytes"));
        else cJSON_AddNullToObject(item, "sizeBytes");
        cJSON_AddStringToObject(item, "sizeStatus", measured ? "ready" : cJSON_HasObjectItem(g, "size_status") ? "unavailable" : "unknown");
        char where[64];
        location(path, where, sizeof where);
        cJSON_AddStringToObject(item, "location", where);
        cJSON_AddBoolToObject(item, "installed", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "installed")));
        cJSON_AddBoolToObject(item, "mounted", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "mounted")));
        cJSON_AddBoolToObject(item, "onDrive", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "source_available")));
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(g, "app_db_size_bytes");
        if (cJSON_IsNumber(size) && size->valuedouble > 0 && size->valuedouble <= 9007199254740991.0)
            cJSON_AddNumberToObject(item, "installedSizeBytes", size->valuedouble);
        else cJSON_AddNullToObject(item, "installedSizeBytes");
        /* Catalogue artwork is optional enrichment, independent of source selection.
         * Never trust/forward ShadowMount's supplied icon URL to a browser. */
        pthread_mutex_lock(&atmosphere.mutex);
        cJSON *release;
        cJSON_ArrayForEach(release, atmosphere.catalog)
            if (!strcmp(json_text(release, "titleId"), id)) {
                cJSON_AddStringToObject(item, "cover", json_text(release, "cover"));
                const char *fallback = json_text(release, "coverFallback");
                if (*fallback) cJSON_AddStringToObject(item, "coverFallback", fallback);
                break;
            }
        pthread_mutex_unlock(&atmosphere.mutex);
        cJSON_AddItemToArray(out, item);
    }
    return out;
invalid:
    snprintf(error, cap, "ShadowMount inventory entry %d has an unsupported %s.",
             cJSON_GetArraySize(out) + 1, field);
    cJSON_Delete(out);
    return NULL;
}
/* Only normalized, provider-reported locations can become copy/move targets. */
static bool safe_path(const char *p) {
    return p[0] == '/' && strlen(p) < 4096 && !strstr(p, "/../") &&
        !strstr(p, "/./") && !strstr(p, "//") && strcmp(p, "/") &&
        strcmp(p + (strlen(p) > 3 ? strlen(p) - 3 : 0), "/..") &&
        strcmp(p + (strlen(p) > 2 ? strlen(p) - 2 : 0), "/.");
}
static bool visible_drive(const char *p) {
    return !strcmp(p, "/user") ||
        (!strncmp(p, "/mnt/usb", 8) && isdigit((unsigned char)p[8]) && !p[9]) ||
        (!strncmp(p, "/mnt/ext", 8) && isdigit((unsigned char)p[8]) && !p[9]);
}
static cJSON *normalize_storage(const cJSON *response) {
    const cJSON *mounts = cJSON_GetObjectItemCaseSensitive(response, "mounts");
    const cJSON *destinations = cJSON_GetObjectItemCaseSensitive(response, "destinations");
    if (!success(response) || !cJSON_IsArray(mounts) || !cJSON_IsArray(destinations) ||
        !number(response, "count") || json_int(response, "count") != cJSON_GetArraySize(mounts) ||
        !number(response, "destination_count") || json_int(response, "destination_count") != cJSON_GetArraySize(destinations) ||
        cJSON_GetArraySize(mounts) > 64 || cJSON_GetArraySize(destinations) > 128) return NULL;
    cJSON *out = cJSON_CreateObject();
    for (int k = 0; k < 2; k++) {
        cJSON *list = cJSON_AddArrayToObject(out, k ? "destinations" : "drives");
        const cJSON *d;
        cJSON_ArrayForEach(d, (k ? destinations : mounts)) {
            const char *path = json_text(d, k ? "path" : "mount_point");
            /* The API also returns / and system partitions. Ignore them before
             * applying destination-path restrictions (which deliberately reject /). */
            if (!k && !visible_drive(path)) continue;
            if (!text_field(d, "source", 4096) || !text_field(d, "mount_point", 4096) ||
                !text_field(d, "filesystem", 64) || (k && !text_field(d, "path", 4096)) ||
                !safe_path(path) || !safe_path(json_text(d, "mount_point")) ||
                !number(d, "total_bytes") || !number(d, "available_bytes") || !number(d, "used_bytes") ||
                json_int(d, "available_bytes") > json_int(d, "total_bytes") ||
                json_int(d, "used_bytes") > json_int(d, "total_bytes") ||
                !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(d, "read_only"))) goto invalid;
            /* Capacity-bearing system partitions and runtime mounts are not user drives. */
            cJSON *item = cJSON_CreateObject();
            char id[65], where[64], label_path[4100];
            identity(path, json_text(d, "source"), json_text(d, "mount_point"), id);
            snprintf(label_path, sizeof label_path, "%s/", json_text(d, "mount_point"));
            location(label_path, where, sizeof where);
            cJSON_AddStringToObject(item, "id", id);
            cJSON_AddStringToObject(item, "path", path);
            cJSON_AddStringToObject(item, "mountPoint", json_text(d, "mount_point"));
            cJSON_AddStringToObject(item, "label", where);
            cJSON_AddStringToObject(item, "filesystem", json_text(d, "filesystem"));
            cJSON_AddBoolToObject(item, "readOnly", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d, "read_only")));
            cJSON_AddNumberToObject(item, "totalBytes", (double)json_int(d, "total_bytes"));
            cJSON_AddNumberToObject(item, "freeBytes", (double)json_int(d, "available_bytes"));
            cJSON_AddNumberToObject(item, "usedBytes", (double)json_int(d, "used_bytes"));
            cJSON_AddItemToArray(list, item);
        }
    }
    return out;
invalid:
    cJSON_Delete(out);
    return NULL;
}
static void read_storage(int port) {
    char error[256] = {0};
    cJSON *response = read_api(port, "storage", "{}", error, sizeof error);
    cJSON *next = response ? normalize_storage(response) : NULL;
#ifdef ATMOSPHERE_NATIVE_APP
    capacity_diagnostic("capacity-api.json",response,error);
#endif
    cJSON_Delete(response);
    pthread_mutex_lock(&lock);
    if (next) {
        cJSON_Delete(storage);
        storage = next;
        storage_updated = time(NULL);
    }
    storage_stale = !next;
    copy_text(storage_error, sizeof storage_error, next ? "" : *error ? error : "Could not read drive capacity from ShadowMount.");
    pthread_mutex_unlock(&lock);
}
static cJSON *normalize_job(const cJSON *r) {
    if (!success(r) || !number(r, "job_id") || !number(r, "total_bytes") ||
        !number(r, "processed_bytes") || !number(r, "result_status") ||
        !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(r, "active")) ||
        !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(r, "cancellable"))) return NULL;
    const char *texts[] = {"operation", "state", "title_id", "source", "destination", "result_error"};
    for (size_t i = 0; i < sizeof texts / sizeof *texts; i++)
        if (!text_field(r, texts[i], 4096)) return NULL;
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "id", (double)json_int(r, "job_id"));
    cJSON_AddStringToObject(out, "operation", json_text(r, "operation"));
    cJSON_AddStringToObject(out, "state", json_text(r, "state"));
    cJSON_AddStringToObject(out, "titleId", json_text(r, "title_id"));
    cJSON_AddStringToObject(out, "source", json_text(r, "source"));
    cJSON_AddStringToObject(out, "destination", json_text(r, "destination"));
    cJSON_AddStringToObject(out, "error", json_text(r, "result_error"));
    cJSON_AddNumberToObject(out, "result", (double)json_int(r, "result_status"));
    cJSON_AddNumberToObject(out, "totalBytes", (double)json_int(r, "total_bytes"));
    cJSON_AddNumberToObject(out, "processedBytes", (double)json_int(r, "processed_bytes"));
    cJSON_AddNumberToObject(out, "speed", number(r, "speed_bytes_per_second") ? (double)json_int(r, "speed_bytes_per_second") : 0);
    cJSON_AddBoolToObject(out, "active", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "active")));
    cJSON_AddBoolToObject(out, "cancellable", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "cancellable")));
    cJSON_AddBoolToObject(out, "cancelRequested", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "cancel_requested")));
    return out;
}
static void accept_job(cJSON *next) {
    bool active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(next, "active"));
    pthread_mutex_lock(&lock);
    bool ended = monitor_job && !active;
    cJSON_Delete(storage_job);
    storage_job = next;
    monitor_job = active;
    job_poll_at = monotonic_seconds() + 2;
    if (ended) { pending = true; storage_pending = true; }
    pthread_mutex_unlock(&lock);
    set_gate(active);
    if(ended)installed_refresh(NULL);
}
static void read_job(int port) {
    char error[256] = {0};
    cJSON *r = read_api(port, "games/storage/status", "{}", error, sizeof error);
    cJSON *next = r ? normalize_job(r) : NULL;
    cJSON_Delete(r);
    if (next) accept_job(next);
    else {
        /* An unknown result is not permission to start another operation. */
        pthread_mutex_lock(&lock);
        if (monitor_job) {
            copy_text(storage_error, sizeof storage_error,
                "Cannot confirm storage job status. Reconnect ShadowMount; the job may still be running.");
            storage_stale = true;
        }
        job_poll_at = monotonic_seconds() + 5;
        pthread_mutex_unlock(&lock);
    }
}
static const char *capability(const char *action) {
    return !strcmp(action, "scan") ? "rescan" : !strcmp(action, "mount") ? "mount_game" :
        !strcmp(action, "unmount") ? "unmount_game" : !strcmp(action, "copy") ? "copy_game_source" :
        !strcmp(action, "move") ? "move_game_source" : !strcmp(action, "cancel") ? "storage_job_cancel" :
        !strcmp(action, "delete") ? "delete_game_source" : !strcmp(action, "measure") ? "list_games" : "";
}
static bool moving_downloads(void) {
    bool active = false;
    pthread_mutex_lock(&atmosphere.mutex);
    active = atmosphere.smb_storage_busy;
    for (size_t i = 0; i < atmosphere.job_count; i++) {
        const char *s = atmosphere.jobs[i].status;
        if (!strcmp(s, "downloading") || !strcmp(s, "verifying") ||
            !strcmp(s, "queued") || !strcmp(s, "retrying")) active = true;
    }
    if (!active) atmosphere.library_storage_busy = true;
    pthread_mutex_unlock(&atmosphere.mutex);
    return active;
}
static void result_message(const char *state, const char *text) {
    pthread_mutex_lock(&lock);
    cJSON_ReplaceItemInObjectCaseSensitive(action_result, "state", cJSON_CreateString(state));
    cJSON_ReplaceItemInObjectCaseSensitive(action_result, "message", cJSON_CreateString(text));
    pthread_mutex_unlock(&lock);
}
static void preserve_sizes(cJSON *next) {
    cJSON *g, *old;
    cJSON_ArrayForEach(g, next) {
        if (strcmp(json_text(g, "sizeStatus"), "unknown")) continue;
        cJSON_ArrayForEach(old, games) {
            if (!strcmp(json_text(old, "sourceKey"), json_text(g, "sourceKey")) &&
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "onDrive"))) {
                const cJSON *value = cJSON_GetObjectItemCaseSensitive(old, "sizeBytes");
                if (cJSON_IsNumber(value)) {
                    cJSON_ReplaceItemInObjectCaseSensitive(g, "sizeBytes", cJSON_Duplicate(value, true));
                    cJSON_ReplaceItemInObjectCaseSensitive(g, "sizeStatus", cJSON_CreateString("ready"));
                }
                break;
            }
        }
    }
}
static void execute_command(cJSON *input) {
    const char *action = json_text(input, "action"), *id = json_text(input, "titleId");
    bool transfer = !strcmp(action, "copy") || !strcmp(action, "move"), deleting=!strcmp(action,"delete"), gate = false;
    char error[256] = {0}, ok[256] = "Library updated.";
    cJSON *v = NULL, *response = NULL, *rows = NULL, *body = NULL, *drives = NULL;
    int port = api_port();
    result_message("running", "Checking current game and drive status…");
    v = port ? read_api(port, "version", "{}", error, sizeof error) : NULL;
    if (!success(v) || !number(v, "api_version") || json_int(v, "api_version") != 1 ||
        !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(v, "capabilities")) ||
        !has(cJSON_GetObjectItemCaseSensitive(v, "capabilities"), capability(action))) {
        copy_text(error, sizeof error, "This action is unavailable in the running ShadowMount version."); goto done;
    }
    if (!strcmp(action, "measure")) {
        response = read_api(port, "games", "{\"include_size\":true}", error, sizeof error);
        rows = response ? normalize(response, error, sizeof error) : NULL;
        if (!rows) goto done;
        pthread_mutex_lock(&lock);
        cJSON_Delete(games); games = rows; rows = NULL;
        updated = checked = time(NULL); refreshed = monotonic_seconds();
        copy_text(status, sizeof status, "ready"); message[0] = 0;
        pthread_mutex_unlock(&lock);
        copy_text(ok, sizeof ok, "Game sizes updated. Unavailable sizes are shown separately.");
        goto succeeded;
    }
    if (transfer || deleting || !strcmp(action, "cancel")) {
        if (!has(cJSON_GetObjectItemCaseSensitive(v, "capabilities"), "storage_job_status")) {
            copy_text(error, sizeof error, "ShadowMount cannot report storage progress. Update it before managing files."); goto done;
        }
        response = read_api(port, "games/storage/status", "{}", error, sizeof error);
        cJSON *job = response ? normalize_job(response) : NULL;
        cJSON_Delete(response); response = NULL;
        if (!job) { copy_text(error, sizeof error, "Cannot confirm the current storage job. Refresh storage before trying again."); goto done; }
        bool active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job, "active"));
        bool can_cancel = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job, "cancellable"));
        double job_id = (double)json_int(job, "id");
        accept_job(job);
        if (!strcmp(action, "cancel")) {
            if (!active || !can_cancel || job_id != (double)json_int(input, "jobId")) {
                copy_text(error, sizeof error, "This job has finished or can no longer be cancelled."); goto done;
            }
            body = cJSON_CreateObject();
            cJSON_AddNumberToObject(body, "job_id", job_id);
        } else if (active) {
            copy_text(error, sizeof error, "A storage job is already running. Wait for it to finish."); goto done;
        }
    }
    if (strcmp(action, "scan") && strcmp(action, "cancel")) {
        response = read_api(port, "games", "{\"include_size\":false}", error, sizeof error);
        rows = response ? normalize(response, error, sizeof error) : NULL;
        cJSON_Delete(response); response = NULL;
        if (!rows) goto done;
        cJSON *g = NULL, *candidate;
        cJSON_ArrayForEach(candidate, rows)
            if (!strcmp(json_text(candidate, "titleId"), id)) { g = candidate; break; }
        if (!g || strcmp(json_text(g, "sourceKey"), json_text(input, "sourceKey"))) {
            copy_text(error, sizeof error, "This game's source changed. Refresh Library and select it again."); goto done;
        }
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "managed")) ||
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "onDrive")) ||
            ((transfer || deleting) && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "canManageSource")))) {
            copy_text(error, sizeof error, "This source is missing or does not support that action."); goto done;
        }
        bool mounted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "mounted"));
        if ((!strcmp(action, "mount") && mounted) || (!strcmp(action, "unmount") && !mounted)) {
            copy_text(ok, sizeof ok, mounted ? "Already mounted." : "Already unmounted."); goto succeeded;
        }
        body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "title_id", id);
        if (deleting) {
            const char *path=json_text(g,"path");
            bool local=!strncmp(path,"/data/",6) || (!strncmp(path,"/mnt/usb",8) && isdigit((unsigned char)path[8]) && path[9]=='/');
            if(!local || strstr(path,"/../") || !strcmp(id,"PPSA99005") || mounted) {
                copy_text(error,sizeof error,"Cannot delete this location or a mounted/running game. Close the game first.");goto done;
            }
            if(moving_downloads()){copy_text(error,sizeof error,"Wait for transfers to finish before deleting a game.");goto done;}
            gate=true;cJSON_AddBoolToObject(body,"confirm",true);
        }
        if (transfer) {
            if (mounted) { copy_text(error, sizeof error, "Unmount this game before copying or moving its source."); goto done; }
            if (moving_downloads()) { copy_text(error, sizeof error, "Pause active and queued downloads before copying or moving games."); goto done; }
            gate = true;
            response = read_api(port, "storage", "{}", error, sizeof error);
            drives = response ? normalize_storage(response) : NULL;
            cJSON_Delete(response); response = NULL;
            cJSON *destination = NULL;
            cJSON_ArrayForEach(candidate, cJSON_GetObjectItemCaseSensitive(drives, "destinations"))
                if (!strcmp(json_text(candidate, "id"), json_text(input, "destinationId"))) { destination = candidate; break; }
            if (!destination || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(destination, "readOnly"))) {
                copy_text(error, sizeof error, "The selected destination changed or is no longer writable. Refresh storage."); goto done;
            }
            const char *target = json_text(destination, "path"), *path = json_text(g, "path");
            const char *base = strrchr(path, '/');
            if (!base || !strcmp(path, target) ||
                (strlen(target) > strlen(path) && !strncmp(target, path, strlen(path)) && target[strlen(path)] == '/') ||
                ((size_t)(base - path) == strlen(target) && !strncmp(target, path, strlen(target)))) {
                copy_text(error, sizeof error, "Choose a different destination outside the source folder."); goto done;
            }
            char *encoded = cJSON_PrintUnformatted(body);
            response = read_api(port, "games/info", encoded, error, sizeof error);
            free(encoded);
            if (!success(response) || strcmp(json_text(response, "path"), path) ||
                !number(response, "size_status") || json_int(response, "size_status") || !number(response, "size_bytes")) {
                copy_text(error, sizeof error, "Could not verify this game's source size. Refresh Library before trying again."); goto done;
            }
            int64_t size = json_int(response, "size_bytes"), free_bytes = json_int(destination, "freeBytes");
            /* Reserve space for unfinished Atmosphere downloads on the destination filesystem. */
            Storage local[ATMOSPHERE_MAX_STORAGE]; size_t count = storage_list(local);
            uint64_t reserved = 0;
            const char *mount = json_text(destination, "mountPoint");
            struct stat destination_stat;
            bool local_identity = !stat(target, &destination_stat);
            pthread_mutex_lock(&atmosphere.mutex);
            for (size_t i = 0; i < count; i++)
                if (local_identity ? local[i].device == destination_stat.st_dev :
                    (!strncmp(local[i].root, mount, strlen(mount)) && local[i].root[strlen(mount)] == '/')) {
                    uint64_t n = storage_pending_locked(&local[i], NULL);
                    reserved = n > UINT64_MAX - reserved ? UINT64_MAX : reserved + n;
                }
            pthread_mutex_unlock(&atmosphere.mutex);
            if (reserved > (uint64_t)free_bytes || size > free_bytes - (int64_t)reserved ||
                free_bytes - (int64_t)reserved - size < 16 * 1024 * 1024) {
                copy_text(error, sizeof error, "Not enough free space after unfinished downloads on the destination drive."); goto done;
            }
            cJSON_Delete(response); response = NULL;
            cJSON_AddStringToObject(body, "destination_dir", target);
        }
    }
    if (!strcmp(action, "scan")) {
        body = cJSON_CreateObject(); cJSON_AddBoolToObject(body, "reset_attempts", false);
    }
    char route[64];
    snprintf(route, sizeof route, "%s%s", !strcmp(action, "scan") ? "" : !strcmp(action, "cancel") ? "games/storage/" : "games/", action);
    char *encoded = cJSON_PrintUnformatted(body);
    response = read_api(port, route, encoded, error, sizeof error);
    free(encoded);
    if (!success(response)) {
        if (!*error) copy_text(error, sizeof error, *json_text(response, "error") ? json_text(response, "error") : "The action could not be confirmed. Refresh before trying again.");
        if (transfer || deleting) {
            /* A timed-out POST may have started a job. Hold the gate until status is known. */
            pthread_mutex_lock(&lock); monitor_job = true; job_poll_at = 0; pthread_mutex_unlock(&lock);
            gate = false;
        }
        goto done;
    }
    if (transfer || deleting || !strcmp(action, "cancel")) {
        cJSON *job = normalize_job(response);
        if (!job) {
            copy_text(error, sizeof error, "Storage request sent, but its result is unconfirmed. Check storage progress.");
            pthread_mutex_lock(&lock); monitor_job = true; job_poll_at = 0; pthread_mutex_unlock(&lock);
            gate = false; goto done;
        }
        accept_job(job); gate = false;
        copy_text(ok, sizeof ok, !strcmp(action, "cancel") ? "Cancellation requested. Waiting for the storage job to stop." : "Storage operation started. Progress is available in Library and Storage.");
    } else if (!strcmp(action, "scan")) {
        copy_text(ok, sizeof ok, "Scan requested. ShadowMount may register or mount detected games. Refresh Library after it finishes.");
    } else {
        if (strcmp(json_text(response, "title_id"), id) ||
            !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(response, "mounted")) ||
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "mounted")) != !strcmp(action, "mount")) {
            copy_text(error, sizeof error, "The mount result could not be confirmed. Refresh Library before retrying."); goto done;
        }
        copy_text(ok, sizeof ok, !strcmp(action, "mount") ? "Game mounted." : "Game unmounted.");
    }
succeeded:
    result_message("complete", ok);
    pthread_mutex_lock(&lock); pending = true; storage_pending = true; pthread_mutex_unlock(&lock);
    goto cleanup;
done:
    result_message("error", *error ? error : "Could not complete the Library action. Refresh and try again.");
    pthread_mutex_lock(&lock); pending = true; pthread_mutex_unlock(&lock);
cleanup:
    if (gate) set_gate(false);
    cJSON_Delete(v); cJSON_Delete(response); cJSON_Delete(rows); cJSON_Delete(body); cJSON_Delete(drives);
}

static void *run(void *unused) {
    (void)unused;
    pthread_mutex_lock(&lock);
    while (!stopping) {
        while (!pending && !storage_pending && !command && !stopping &&
               !(monitor_job && monotonic_seconds() >= job_poll_at)) {
            struct timespec deadline = {.tv_sec = time(NULL) + 1, .tv_nsec = 0};
            pthread_cond_timedwait(&changed, &lock, &deadline);
        }
        if (stopping) break;
        if (command) {
            cJSON *input = command; command = NULL;
            pthread_mutex_unlock(&lock);
            execute_command(input);
            cJSON_Delete(input);
            pthread_mutex_lock(&lock);
            continue;
        }
        if (monitor_job && monotonic_seconds() >= job_poll_at) {
            pthread_mutex_unlock(&lock);
            read_job(api_port());
            pthread_mutex_lock(&lock);
            continue;
        }
        if (!pending && storage_pending) {
            storage_pending = false; storage_busy = true;
            bool can_read = has(capabilities, "storage_space");
            bool can_track = has(capabilities, "storage_job_status");
            pthread_mutex_unlock(&lock);
            if (can_read) read_storage(api_port());
#ifndef ATMOSPHERE_NATIVE_APP
            if (can_track) read_job(api_port());
#else
            (void)can_track;
#endif
            pthread_mutex_lock(&lock);
            if (!can_read) {
                storage_stale = true;
                copy_text(storage_error, sizeof storage_error, "Storage management needs a compatible ShadowMount version.");
            }
            storage_busy = false;
            continue;
        }
        pending = false;
        busy = true;
        pthread_mutex_unlock(&lock);
        const char *next_status = "unavailable";
        const char *error = "Run ShadowMount with its local HTTP API enabled to see installed games and games on your drives.";
        char next_version[48] = "";
        cJSON *next = NULL;
        cJSON *next_capabilities = NULL;
        int port = api_port();
        cJSON *v = port ? read_api(port, "version", "{}", NULL, 0) : NULL;
        char detail[256] = {0};
        if (success(v)) {
            const cJSON *api = cJSON_GetObjectItemCaseSensitive(v, "api_version");
            if (cJSON_IsNumber(api) && api->valuedouble == 1) {
                const cJSON *caps = cJSON_GetObjectItemCaseSensitive(v, "capabilities");
                next_capabilities = cJSON_IsArray(caps) && cJSON_GetArraySize(caps) <= 64 ? cJSON_Duplicate(caps, true) : cJSON_CreateArray();
                copy_text(next_version, sizeof next_version, json_text(v, "shadowmount_version"));
                cJSON *response = read_api(port, "games", "{\"include_size\":false}", detail, sizeof detail);
                next = response ? normalize(response, detail, sizeof detail) : NULL;
#ifdef ATMOSPHERE_NATIVE_APP
                capacity_diagnostic("installed-inventory.json",response,detail);
#endif
                cJSON_Delete(response);
                next_status = next ? "ready" : "error";
                error = next ? "" : *detail ? detail : "ShadowMount did not return a complete, supported inventory. Refresh to try again.";
            } else {
                next_status = "unsupported";
                error = "This ShadowMount API version is not supported. Library needs the v1 games API with installed-game status.";
            }
        }
        cJSON_Delete(v);
        pthread_mutex_lock(&lock);
        checked = time(NULL);
        if (next) {
            preserve_sizes(next);
            cJSON_Delete(games);
            games = next;
            updated = checked;
            refreshed = monotonic_seconds();
            copy_text(version, sizeof version, next_version);
            /* Discover an existing provider job after Atmosphere restarts, even if
             * the user stays in Discover instead of opening Storage. */
            if (!storage_attempted && has(next_capabilities, "storage_job_status")) {
                storage_pending = true;
                storage_attempted = monotonic_seconds();
            }
        }
        cJSON_Delete(capabilities);
        capabilities = next_capabilities;
        copy_text(status, sizeof status, next_status);
        copy_text(message, sizeof message, error);
        busy = false;
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}
int library_start(void) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, ATMOSPHERE_THREAD_STACK);
    pthread_mutex_lock(&lock);
    int rc = pthread_create(&worker, &attr, run, NULL);
    started = !rc;
    pthread_mutex_unlock(&lock);
    pthread_attr_destroy(&attr);
    return rc;
}
void library_stop(void) {
    pthread_mutex_lock(&lock);
    stopping = true;
    pthread_cond_signal(&changed);
    pthread_mutex_unlock(&lock);
    if (started) pthread_join(worker, NULL);
    cJSON_Delete(games);
    games = NULL;
    cJSON_Delete(capabilities); cJSON_Delete(storage); cJSON_Delete(storage_job);
    cJSON_Delete(command); cJSON_Delete(last_request); cJSON_Delete(action_result);
}
cJSON *library_snapshot(bool refresh) {
    pthread_mutex_lock(&lock);
    time_t now = monotonic_seconds();
    if (started && !stopping && !busy && !pending &&
        (!attempted || now - attempted >= (refresh ? 5 : 30))) {
        pending = true;
        attempted = now;
        pthread_cond_signal(&changed);
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "status", started ? status : "unavailable");
    cJSON_AddStringToObject(out, "message", started ? message : "Library is unavailable. Restart Atmosphere to try again.");
    cJSON_AddStringToObject(out, "provider", "ShadowMount");
    cJSON_AddStringToObject(out, "providerVersion", version);
    cJSON_AddBoolToObject(out, "busy", busy || pending);
    cJSON_AddBoolToObject(out, "stale", updated && (strcmp(status, "ready") || now - refreshed >= 30));
    cJSON_AddNumberToObject(out, "checkedAt", (double)checked);
    cJSON_AddNumberToObject(out, "updatedAt", (double)updated);
    cJSON_AddNumberToObject(out, "refreshAfter", attempted && now - attempted < 5 ? 5 - (now - attempted) : 0);
    cJSON_AddItemToObject(out, "games", games ? cJSON_Duplicate(games, true) : cJSON_CreateArray());
    cJSON_AddItemToObject(out, "capabilities", capabilities ? cJSON_Duplicate(capabilities, true) : cJSON_CreateArray());
    cJSON_AddItemToObject(out, "action", action_result ? cJSON_Duplicate(action_result, true) : cJSON_CreateNull());
    cJSON_AddItemToObject(out, "storageJob", storage_job ? cJSON_Duplicate(storage_job, true) : cJSON_CreateNull());
    cJSON_AddStringToObject(out, "storageError", storage_error);
    cJSON_AddBoolToObject(out, "storageBusy", monitor_job);
    pthread_mutex_unlock(&lock);
    return out;
}

cJSON *library_storage_snapshot(bool refresh) {
    cJSON_Delete(library_snapshot(false));
    pthread_mutex_lock(&lock);
    time_t now = monotonic_seconds();
    if (started && !stopping && !storage_busy && !storage_pending &&
        (!storage_attempted || now - storage_attempted >= (refresh ? 5 : 30))) {
        storage_pending = true; storage_attempted = now;
        pthread_cond_signal(&changed);
    }
    cJSON *out = storage ? cJSON_Duplicate(storage, true) : cJSON_CreateObject();
    if (!storage) { cJSON_AddArrayToObject(out, "drives"); cJSON_AddArrayToObject(out, "destinations"); }
    cJSON_AddBoolToObject(out, "busy", storage_busy || storage_pending);
    cJSON_AddBoolToObject(out, "stale", storage_stale || (storage_attempted && now - storage_attempted >= 30));
    cJSON_AddNumberToObject(out, "updatedAt", (double)storage_updated);
    cJSON_AddStringToObject(out, "error", storage_error);
#ifdef ATMOSPHERE_NATIVE_APP
    static time_t diagnosed;
    if(!diagnosed || now-diagnosed>=10) {
        diagnosed=now;
        cJSON_AddBoolToObject(out,"workerStarted",started);
        cJSON_AddStringToObject(out,"providerStatus",status);
        cJSON_AddStringToObject(out,"providerMessage",message);
        capacity_diagnostic("capacity-status.json",out,"");
    }
#endif
    pthread_mutex_unlock(&lock);
    return out;
}
int library_action(const cJSON *input, char *error, size_t cap) {
    const char *action = json_text(input, "action"), *id = json_text(input, "titleId");
    const char *request = json_text(input, "requestId");
    if (!*capability(action) || !text_field(input, "requestId", 64) || strlen(request) < 8 ||
        (strcmp(action, "scan") && strcmp(action, "measure") && strcmp(action, "cancel") &&
         (!title_id(id) || !text_field(input, "sourceKey", 64) || strlen(json_text(input, "sourceKey")) != 64)) ||
        ((!strcmp(action, "copy") || !strcmp(action, "move")) &&
         (!text_field(input, "destinationId", 64) || strlen(json_text(input, "destinationId")) != 64)) ||
        (!strcmp(action, "cancel") && (!number(input, "jobId") || json_int(input, "jobId") <= 0))) {
        copy_text(error, cap, "Expected a supported action and its current Library identifiers."); return 400;
    }
    if ((!strcmp(action, "scan") || !strcmp(action, "move") || !strcmp(action, "delete") || !strcmp(action, "unmount")) &&
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input, "confirmed"))) {
        copy_text(error, cap, "Confirm this action first."); return 400;
    }
    pthread_mutex_lock(&lock);
    int code = 202;
    if (!started || stopping) { code = 503; copy_text(error, cap, "Library is unavailable."); }
    else if (!strcmp(request, json_text(last_request, "requestId"))) {
        if (!cJSON_Compare(input, last_request, true)) { code = 409; copy_text(error, cap, "This request ID was already used for another action."); }
    } else if (command || !strcmp(json_text(action_result, "state"), "running") ||
               !strcmp(json_text(action_result, "state"), "queued") || (monitor_job && strcmp(action, "cancel"))) {
        code = 409; copy_text(error, cap, "Another Library operation is in progress. Wait for it to finish.");
    } else if (strcmp(status, "ready") || !has(capabilities, capability(action))) {
        code = 409; copy_text(error, cap, "Refresh Library with a compatible ShadowMount version before using this action.");
    } else {
        command = cJSON_Duplicate(input, true);
        cJSON_Delete(last_request); last_request = cJSON_Duplicate(input, true);
        cJSON_Delete(action_result); action_result = cJSON_CreateObject();
        cJSON_AddStringToObject(action_result, "id", request);
        cJSON_AddStringToObject(action_result, "action", action);
        cJSON_AddStringToObject(action_result, "titleId", id);
        cJSON_AddStringToObject(action_result, "state", "queued");
        cJSON_AddStringToObject(action_result, "message", "Waiting to start…");
        pthread_cond_signal(&changed);
    }
    pthread_mutex_unlock(&lock);
    return code;
}
