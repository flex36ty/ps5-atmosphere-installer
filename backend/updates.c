/* Explicit, paired-device updates from Atmosphere's fixed release feed. Nothing runs automatically. */
#include "ca.h"
#include "atmosphere.h"
#include <curl/curl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define FEED "https://raw.githubusercontent.com/saawant12/orbit-store-ps5/main/payloads.json"
#define RELEASE_BASE "https://github.com/saawant12/orbit-store-ps5/releases/download/"
#define MAX_ELF (64 * 1024 * 1024)
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t thread;
    bool started, stop, busy;
    char action[16], phase[24], error[256];
    char version[64], url[512], checksum[65];
    time_t checked, retry_at, check_after;
    size_t received;
    bool saved;
} update = {.mutex = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};

typedef struct {
    unsigned char *data;
    size_t size, limit;
    long retry;
    bool asset;
} Buffer;

static bool available(void) {
#ifdef ATMOSPHERE_TEST
    return atmosphere.autoboot;
#else
    return atmosphere.autoboot && !atmosphere.desktop;
#endif
}
static bool interrupted(void) {
    pthread_mutex_lock(&update.mutex);
    bool stop = update.stop;
    pthread_mutex_unlock(&update.mutex);
    return stop;
}
static size_t collect(char *data, size_t a, size_t b, void *context) {
    Buffer *buffer = context;
    size_t n = a * b;
    if (n > buffer->limit - buffer->size || interrupted())
        return 0;
    unsigned char *next = realloc(buffer->data, buffer->size + n + 1);
    if (!next)
        return 0;
    buffer->data = next;
    memcpy(next + buffer->size, data, n);
    buffer->size += n;
    next[buffer->size] = 0;
    if (buffer->asset) {
        pthread_mutex_lock(&update.mutex);
        update.received = buffer->size;
        pthread_mutex_unlock(&update.mutex);
    }
    return n;
}
static size_t headers(char *data, size_t a, size_t b, void *context) {
    Buffer *buffer = context;
    size_t n = a * b;
    if (n > 12 && !strncasecmp(data, "Retry-After:", 12)) {
        char value[100];
        size_t len = n - 12 < sizeof value - 1 ? n - 12 : sizeof value - 1;
        memcpy(value, data + 12, len);
        value[len] = 0;
        char *end;
        long seconds = strtol(value, &end, 10);
        if (end == value) {
            time_t date = curl_getdate(value, NULL);
            seconds = date > time(NULL) ? date - time(NULL) : 0;
        }
        if (seconds > buffer->retry)
            buffer->retry = seconds;
    }
    return n;
}
static int progress(void *ctx, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)ctx;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return interrupted();
}
static bool test_url(const char *url) {
#ifdef ATMOSPHERE_TEST
    const char *base = getenv("ATMOSPHERE_TEST_UPDATE_BASE");
    return base && !strncmp(base, "http://127.0.0.1:", 17) && !strncmp(url, base, strlen(base)) &&
           url[strlen(base)] == '/';
#else
    (void)url;
    return false;
#endif
}
static bool permitted_url(const char *url, bool asset) {
    return test_url(url) ||
           (!asset ? !strcmp(url, FEED)
                   : !strncmp(url, RELEASE_BASE, strlen(RELEASE_BASE)) ||
                         !strncmp(url, "https://release-assets.githubusercontent.com/", 45));
}
static int fetch(const char *url, Buffer *buffer, char *error, size_t cap) {
    char current[4096];
    copy_text(current, sizeof current, url);
    for (unsigned redirects = 0; redirects <= 4; redirects++) {
        if (!permitted_url(current, buffer->asset)) {
            copy_text(error, cap, "The release redirected outside Atmosphere's allowed download hosts.");
            return -1;
        }
        CURL *curl = curl_easy_init();
        if (!curl)
            return -1;
        struct curl_blob ca = {(void *)atmosphere_ca, sizeof atmosphere_ca - 1, CURL_BLOB_COPY};
        buffer->size = 0;
        curl_easy_setopt(curl, CURLOPT_URL, current);
        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, test_url(current) ? "http" : "https");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Atmosphere-Store/" ATMOSPHERE_VERSION);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headers);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, buffer);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        CURLcode result = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        char *location = NULL;
        curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
        bool redirect = result == CURLE_OK && status >= 300 && status < 400 && location;
        if (redirect && strlen(location) < sizeof current)
            copy_text(current, sizeof current, location);
        else
            redirect = false;
        curl_easy_cleanup(curl);
        if (status == 403 || status == 429 || status == 503) {
            pthread_mutex_lock(&update.mutex);
            update.retry_at = time(NULL) + (buffer->retry > 60 ? buffer->retry : 60);
            pthread_mutex_unlock(&update.mutex);
        }
        if (redirect)
            continue;
        if (result == CURLE_OK && status == 200)
            return 0;
        snprintf(error, cap, "Release download failed (HTTP %ld, %s). Try again later.", status,
                 curl_easy_strerror(result));
        return -1;
    }
    copy_text(error, cap, "Too many release redirects.");
    return -1;
}
/* Releases use major.minor.patch or major.minor.patch-beta.number. */
static bool version_parts(const char *version, unsigned *parts) {
    if (*version == 'v')
        version++;
    parts[3] = 1;
    parts[4] = 0;
    for (int i = 0; i < 5; i++) {
        if (i == 3) {
            if (!*version)
                return true;
            if (strncmp(version, "-beta.", 6))
                return false;
            version += 6;
            parts[3] = 0;
            continue;
        }
        unsigned digits = 0, value = 0;
        while (*version >= '0' && *version <= '9') {
            if (++digits > 6)
                return false;
            value = value * 10 + (unsigned)(*version++ - '0');
        }
        if (!digits)
            return false;
        parts[i] = value;
        if (i < 2 && *version++ != '.')
            return false;
    }
    if (*version)
        return false;
    return true;
}
static int compare_version(const char *a, const char *b) {
    unsigned x[5], y[5];
    if (!version_parts(a, x) || !version_parts(b, y))
        return -2;
    for (int i = 0; i < 5; i++)
        if (x[i] != y[i])
            return x[i] > y[i] ? 1 : -1;
    return 0;
}
static int check(char *error, size_t cap) {
    const char *url = FEED;
#ifdef ATMOSPHERE_TEST
    char fixture[512];
    const char *base = getenv("ATMOSPHERE_TEST_UPDATE_BASE");
    if (base) {
        snprintf(fixture, sizeof fixture, "%s/feed", base);
        url = fixture;
    }
#endif
    Buffer buffer = {.limit = 65536};
    int rc = fetch(url, &buffer, error, cap);
    cJSON *feed = rc ? NULL : cJSON_Parse((char *)buffer.data);
    free(buffer.data);
    if (rc)
        return rc;
    cJSON *payloads = cJSON_GetObjectItemCaseSensitive(feed, "payloads"), *entry, *selected = NULL;
    cJSON_ArrayForEach(entry, payloads) {
        if (!strcmp(json_text(entry, "filename"), "atmosphere.elf")) {
            if (selected) {
                selected = NULL;
                break;
            }
            selected = entry;
        }
    }
    const char *version = json_text(selected, "version"), *asset = json_text(selected, "url"),
               *hash = json_text(selected, "checksum");
    char expected[512];
    snprintf(expected, sizeof expected, "%s%s/atmosphere.elf", RELEASE_BASE, version);
    if (!selected || !cJSON_IsArray(payloads) || compare_version(version, ATMOSPHERE_VERSION) < 0 ||
        (strcmp(asset, expected) && !test_url(asset)) || strlen(hash) != 64 ||
        strspn(hash, "0123456789abcdef") != 64) {
        cJSON_Delete(feed);
        copy_text(error, cap,
                  "No compatible Atmosphere release found. An older or invalid release will not replace "
                  "this build.");
        return -1;
    }
    pthread_mutex_lock(&update.mutex);
    copy_text(update.version, sizeof update.version, *version == 'v' ? version + 1 : version);
    copy_text(update.url, sizeof update.url, asset);
    copy_text(update.checksum, sizeof update.checksum, hash);
    update.checked = time(NULL);
    pthread_mutex_unlock(&update.mutex);
    cJSON_Delete(feed);
    return 0;
}
static int install(char *error, size_t cap) {
    Buffer buffer = {.limit = MAX_ELF, .asset = true};
    int rc = fetch(update.url, &buffer, error, cap);
    if (!rc) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned size = 0;
        char hex[65];
        if (!EVP_Digest(buffer.data, buffer.size, digest, &size, EVP_sha256(), NULL) || size != 32)
            rc = -1;
        else {
            for (unsigned i = 0; i < 32; i++)
                snprintf(hex + i * 2, 3, "%02x", digest[i]);
            /* ELF64, little endian, x86-64, executable/shared object. */
            rc = strcmp(hex, update.checksum) || buffer.size < 64 ||
                 memcmp(buffer.data, "\177ELF\2\1\1", 7) ||
                 (buffer.data[16] != 2 && buffer.data[16] != 3) || buffer.data[17] ||
                 buffer.data[18] != 62 || buffer.data[19];
        }
        if (rc)
            copy_text(
                error, cap,
                "The release failed SHA-256 or ELF validation. The saved copy was not replaced.");
    }
    if (!rc && interrupted())
        rc = -1;
    if (!rc) {
        pthread_mutex_lock(&update.mutex);
        copy_text(update.phase, sizeof update.phase, "saving");
        pthread_mutex_unlock(&update.mutex);
        rc = autoboot_install(buffer.data, buffer.size, update.version) < 0 ? -1 : 0;
        if (rc)
            copy_text(error, cap,
                      "Could not refresh every saved Atmosphere copy. Your running session is "
                      "unchanged; retry reinstall before restarting.");
    }
    free(buffer.data);
    return rc;
}
static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&update.mutex);
    while (!update.stop) {
        while (!update.action[0] && !update.stop)
            pthread_cond_wait(&update.changed, &update.mutex);
        if (update.stop)
            break;
        bool checking = !strcmp(update.action, "check");
        update.action[0] = 0;
        pthread_mutex_unlock(&update.mutex);
        char error[256] = {0};
        int rc = checking ? check(error, sizeof error) : install(error, sizeof error);
        pthread_mutex_lock(&update.mutex);
        copy_text(update.error, sizeof update.error,
                  rc ? (*error ? error : "Update interrupted. Try again.") : "");
        copy_text(update.phase, sizeof update.phase, rc ? "error" : checking ? "checked" : "saved");
        if (!checking && !rc)
            update.saved = true;
        update.busy = false;
    }
    pthread_mutex_unlock(&update.mutex);
    return NULL;
}
int updates_start(void) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr))
        return -1;
    int rc = pthread_attr_setstacksize(&attr, ATMOSPHERE_THREAD_STACK);
    if (!rc)
        rc = pthread_create(&update.thread, &attr, worker, NULL);
    pthread_attr_destroy(&attr);
    pthread_mutex_lock(&update.mutex);
    update.started = !rc;
    if (rc)
        copy_text(update.error, sizeof update.error,
                  "Could not start the update worker. Restart Atmosphere to retry.");
    pthread_mutex_unlock(&update.mutex);
    return rc ? -1 : 0;
}
void updates_stop(void) {
    pthread_mutex_lock(&update.mutex);
    update.stop = true;
    pthread_cond_signal(&update.changed);
    bool started = update.started;
    pthread_mutex_unlock(&update.mutex);
    if (started)
        pthread_join(update.thread, NULL);
}
cJSON *updates_status(void) {
    cJSON *o = cJSON_CreateObject();
    char saved[64] = {0};
    if (atmosphere.autoboot)
        autoboot_saved_version(saved, sizeof saved);
    pthread_mutex_lock(&update.mutex);
    cJSON_AddBoolToObject(o, "available", available() && update.started);
    cJSON_AddStringToObject(o, "runningVersion", ATMOSPHERE_VERSION);
    cJSON_AddStringToObject(o, "savedVersion", saved);
    cJSON_AddStringToObject(o, "latestVersion", update.version);
    cJSON_AddStringToObject(o, "checksum", update.checksum);
    cJSON_AddStringToObject(o, "phase", *update.phase ? update.phase : "idle");
    cJSON_AddStringToObject(o, "error", update.error);
    cJSON_AddBoolToObject(o, "busy", update.busy);
    cJSON_AddBoolToObject(o, "restartRequired",
                          update.saved || (*saved && strcmp(saved, ATMOSPHERE_VERSION)));
    cJSON_AddNumberToObject(o, "received", (double)update.received);
    cJSON_AddNumberToObject(o, "retryAt", (double)update.retry_at);
    cJSON_AddNumberToObject(o, "checkedAt", (double)update.checked);
    cJSON_AddNumberToObject(o, "checkAfter", (double)update.check_after);
    pthread_mutex_unlock(&update.mutex);
    return o;
}
int updates_action(const cJSON *input, char *error, size_t cap) {
    const char *action = json_text(input, "action");
    bool checking = !strcmp(action, "check"), installing = !strcmp(action, "install"),
         stopping = !strcmp(action, "stop");
    int code = 202;
    pthread_mutex_lock(&update.mutex);
    if (!checking && !installing && !stopping) {
        code = 400;
        copy_text(error, cap, "Unknown update action.");
    } else if (!available() || !update.started) {
        code = 409;
        copy_text(error, cap, "Updates are available from Atmosphere running on your PS5.");
    } else if (update.busy || update.stop) {
        code = 409;
        copy_text(error, cap, "An Atmosphere update is in progress. Wait for it to finish.");
    } else if ((!stopping && update.retry_at > time(NULL)) ||
               (checking && update.check_after > time(NULL))) {
        code = 429;
        copy_text(error, cap, "Please wait before checking or downloading again.");
    } else if (stopping && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input, "confirmed"))) {
        code = 400;
        copy_text(error, cap, "Confirm stopping Atmosphere and pausing downloads first.");
    } else if (installing && (!update.checked || time(NULL) - update.checked > 900 ||
                              strcmp(json_text(input, "version"), update.version) ||
                              strcmp(json_text(input, "checksum"), update.checksum))) {
        code = 409;
        copy_text(error, cap, "Check for updates again before installing this release.");
    } else if (stopping) {
        /* Persist the paused queue before acknowledging shutdown. */
        pthread_mutex_lock(&atmosphere.mutex);
        char previous[ATMOSPHERE_MAX_JOBS][24];
        bool paused[ATMOSPHERE_MAX_JOBS];
        for (size_t i = 0; i < atmosphere.job_count; i++) {
            Job *job = &atmosphere.jobs[i];
            copy_text(previous[i], sizeof previous[i], job->status);
            paused[i] = job->pause;
            if (!strcmp(job->status, "queued") || !strcmp(job->status, "retrying"))
                copy_text(job->status, sizeof job->status, "paused");
            if (!strcmp(job->status, "downloading") || !strcmp(job->status, "verifying"))
                job->pause = true;
        }
        if (state_save_locked()) {
            for (size_t i = 0; i < atmosphere.job_count; i++) {
                copy_text(atmosphere.jobs[i].status, sizeof atmosphere.jobs[i].status, previous[i]);
                atmosphere.jobs[i].pause = paused[i];
            }
            code = 503;
            copy_text(error, cap, "Could not save the queue. Atmosphere has not been stopped.");
        } else {
            atmosphere.stop = true;
            pthread_cond_signal(&atmosphere.changed);
            update.stop = true;
            copy_text(update.phase, sizeof update.phase, "stopping");
            pthread_cond_signal(&update.changed);
            atmosphere_request_stop();
        }
        pthread_mutex_unlock(&atmosphere.mutex);
    } else {
        update.busy = true;
        update.error[0] = 0;
        update.received = 0;
        if (checking) {
            update.checked = 0;
            update.version[0] = 0;
            update.checksum[0] = 0;
        }
        if (checking)
            update.check_after = time(NULL) + 60;
        else
            update.retry_at = time(NULL) + 60;
        copy_text(update.action, sizeof update.action, action);
        copy_text(update.phase, sizeof update.phase, checking ? "checking" : "downloading");
        pthread_cond_signal(&update.changed);
    }
    pthread_mutex_unlock(&update.mutex);
    return code;
}
