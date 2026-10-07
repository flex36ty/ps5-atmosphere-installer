#include "ca.h"
#include "atmosphere.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
_Static_assert(sizeof(off_t) >= 8, "Atmosphere requires 64-bit file offsets");
typedef struct {
    Job *job;
    Release *release;
    int fd;
    long status;
    int64_t length, start, end, total, offset, written;
    char etag[256], modified[128], type[128], error[256];
    time_t retry_at, last_save, last_storage;
    bool accepted, probe;
    double started;
} Transfer;
static double seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static void header_value(char *out, size_t cap, const char *s, size_t n) {
    while (n && (*s == ' ' || *s == '\t')) {
        s++;
        n--;
    }
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' '))
        n--;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
}
static bool controlled(Transfer *t) {
    bool stop;
    pthread_mutex_lock(&atmosphere.mutex);
    stop = atmosphere.stop || t->job->pause || t->job->cancel;
    pthread_mutex_unlock(&atmosphere.mutex);
    return stop;
}
static size_t headers(char *s, size_t a, size_t b, void *arg) {
    Transfer *t = arg;
    size_t n = a * b;
    if (n > 5 && !strncmp(s, "HTTP/", 5)) {
        char line[100];
        header_value(line, sizeof line, s, n);
        char *p = strchr(line, ' ');
        t->status = p ? strtol(p + 1, NULL, 10) : 0;
        t->length = t->start = t->end = t->total = -1;
        /* libcurl calls us for every response in a redirect chain. None of the
         * previous response's error or retry metadata describes the new one. */
        t->etag[0] = t->modified[0] = t->type[0] = t->error[0] = 0;
        t->retry_at = 0;
        t->accepted = false;
    } else if (n > 15 && !strncasecmp(s, "Content-Length:", 15)) {
        t->length = strtoll(s + 15, NULL, 10);
    } else if (n > 14 && !strncasecmp(s, "Content-Range:", 14)) {
        long long x, y, z;
        if (sscanf(s + 14, " bytes %lld-%lld/%lld", &x, &y, &z) == 3) {
            t->start = x;
            t->end = y;
            t->total = z;
        }
    } else if (n > 5 && !strncasecmp(s, "ETag:", 5)) {
        header_value(t->etag, sizeof t->etag, s + 5, n - 5);
    } else if (n > 14 && !strncasecmp(s, "Last-Modified:", 14)) {
        header_value(t->modified, sizeof t->modified, s + 14, n - 14);
    } else if (n > 13 && !strncasecmp(s, "Content-Type:", 13)) {
        header_value(t->type, sizeof t->type, s + 13, n - 13);
    } else if (n > 12 && !strncasecmp(s, "Retry-After:", 12)) {
        char v[100];
        header_value(v, sizeof v, s + 12, n - 12);
        char *end = NULL;
        long long wait = strtoll(v, &end, 10);
        time_t now = time(NULL);
        t->retry_at = (end && !*end && wait >= 0 && wait < 31536000) ? now + (time_t)wait
                                                                     : curl_getdate(v, NULL);
    } else if ((n == 2 && s[0] == '\r') || (n == 1 && s[0] == '\n')) {
        /* Redirects commonly have an HTML body. Validate the file response,
         * after libcurl follows Location, rather than rejecting that page. */
        if (t->probe || (t->status >= 100 && t->status < 200) ||
            (t->status >= 300 && t->status < 400))
            return n;
        if (t->status == 200 && t->offset == 0 && t->length == t->release->size)
            t->accepted = true;
        if (t->status == 206 && t->start == t->offset && t->total == t->release->size &&
            t->end == t->release->size - 1 && t->length == t->release->size - t->offset)
            t->accepted = true;
        if (t->accepted &&
            ((t->job->etag[0] && strcmp(t->job->etag, t->etag)) ||
             (!t->job->etag[0] && t->job->modified[0] && strcmp(t->job->modified, t->modified)))) {
            t->accepted = false;
            copy_text(t->error, sizeof t->error,
                      "Source changed during the request. Partial file preserved.");
        }
        if (strcmp(t->release->id, "atmosphere-network-check") &&
            (strstr(t->type, "text/") || strstr(t->type, "json"))) {
            t->accepted = false;
            copy_text(t->error, sizeof t->error,
                      "Provider returned a page instead of a downloadable file.");
        }
    }
    return n;
}
static size_t body(char *data, size_t a, size_t b, void *arg) {
    Transfer *t = arg;
    size_t n = a * b;
    if (t->status >= 300 && t->status < 400)
        return n;
    if (!t->accepted) {
        if (!t->error[0])
            copy_text(t->error, sizeof t->error,
                      "Invalid HTTP range or file size. Partial file preserved.");
        return 0;
    }
    if (controlled(t))
        return 0;
    if (t->offset + t->written + (int64_t)n > t->release->size) {
        copy_text(t->error, sizeof t->error, "Provider sent more bytes than expected.");
        return 0;
    }
    time_t now = time(NULL);
    if (now != t->last_storage) {
        t->last_storage = now;
        if (!storage_matches(t->job)) {
            copy_text(t->error, sizeof t->error,
                      "Destination drive disconnected. Reconnect the original drive.");
            return 0;
        }
    }
    size_t p = 0;
    while (p < n) {
        ssize_t w = write(t->fd, data + p, n - p);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            snprintf(t->error, sizeof t->error, "Destination write failed: %s", strerror(errno));
            return 0;
        }
        p += (size_t)w;
        t->written += w;
    }
    pthread_mutex_lock(&atmosphere.mutex);
    t->job->received = t->offset + t->written;
    double elapsed = seconds() - t->started;
    t->job->speed = elapsed > 0 ? (double)t->written / elapsed : 0;
    if (now != t->last_save) {
        t->last_save = now;
        if (state_save_locked())
            copy_text(t->error, sizeof t->error, "Queue state could not be saved.");
    }
    pthread_mutex_unlock(&atmosphere.mutex);
    return t->error[0] ? 0 : n;
}
static int progress(void *arg, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    (void)dt;
    (void)dn;
    (void)ut;
    (void)un;
    return controlled(arg) ? 1 : 0;
}
static CURL *request(Transfer *t) {
    CURL *c = curl_easy_init();
    if (!c)
        return NULL;
    struct curl_blob ca = {(void *)atmosphere_ca, sizeof atmosphere_ca - 1, CURL_BLOB_COPY};
    curl_easy_setopt(c, CURLOPT_URL, t->release->url);
    curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef ATMOSPHERE_TEST
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "AtmosphereStore/0.1");
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, headers);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, t);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, t);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, t);
    return c;
}
static bool checksum(Transfer *t) {
    if (!t->release->sha256[0])
        return true;
    if (lseek(t->fd, 0, SEEK_SET) < 0)
        return false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
        return false;
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;
    static unsigned char block[65536]; /* One worker thread: kept off its stack. */
    unsigned char digest[32] = {0};
    unsigned length = 0;
    while (ok) {
        ssize_t n = read(t->fd, block, sizeof block);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ok = false;
            break;
        }
        if (!n)
            break;
        if (controlled(t)) {
            ok = false;
            break;
        }
        ok = EVP_DigestUpdate(ctx, block, (size_t)n) == 1;
    }
    if (ok)
        ok = EVP_DigestFinal_ex(ctx, digest, &length) == 1;
    EVP_MD_CTX_free(ctx);
    char hex[65];
    for (unsigned i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return ok && length == 32 && !strcmp(hex, t->release->sha256);
}
static bool retryable(long status, CURLcode rc) {
    return status == 429 || status == 503 || status == 502 || status == 504 ||
           rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST ||
           rc == CURLE_OPERATION_TIMEDOUT || rc == CURLE_RECV_ERROR || rc == CURLE_PARTIAL_FILE;
}
static void run_job(Job *j) {
    Release *r = &j->release;
    Transfer t = {.job = j,
                  .release = r,
                  .fd = -1,
                  .length = -1,
                  .start = -1,
                  .end = -1,
                  .total = -1,
                  .probe = true};
    int dir = -1;
    char partial[192];
    snprintf(partial, sizeof partial, "%s.part", j->filename);
    CURLcode rc = CURLE_OK;
    CURL *c = NULL;
    struct curl_slist *conditions = NULL;
    bool done = false;
    dir = storage_open(j);
    if (dir < 0) {
        copy_text(t.error, sizeof t.error, "Destination is missing or not writable.");
        goto finish;
    }
    struct stat st;
    if (fstatat(dir, j->filename, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        copy_text(t.error, sizeof t.error,
                  "Destination file already exists. It will not be overwritten.");
        goto finish;
    }
    t.fd = openat(dir, partial, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (t.fd < 0 || fstat(t.fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        copy_text(t.error, sizeof t.error, "Cannot open a safe partial file.");
        goto finish;
    }
    t.offset = st.st_size;
    if (t.offset < 0 || t.offset > r->size) {
        copy_text(t.error, sizeof t.error,
                  "Partial file size is invalid. Delete the partial file before retrying.");
        goto finish;
    }
    struct statvfs fs;
    if (fstatvfs(t.fd, &fs) || (uint64_t)fs.f_bavail * (fs.f_frsize ? fs.f_frsize : fs.f_bsize) <
                                   (uint64_t)(r->size - t.offset) + 16 * 1024 * 1024) {
        copy_text(t.error, sizeof t.error, "Not enough free space to finish this download.");
        goto finish;
    }
    c = request(&t);
    if (!c) {
        copy_text(t.error, sizeof t.error, "Could not initialize HTTPS.");
        goto finish;
    }
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    c = NULL;
    if (rc != CURLE_OK || t.status != 200) {
        snprintf(t.error, sizeof t.error, "Source check failed (HTTP %ld, %s).", t.status,
                 curl_easy_strerror(rc));
        goto finish;
    }
    if (t.length != r->size) {
        copy_text(t.error, sizeof t.error, "Source size changed. Catalogue needs verification.");
        goto finish;
    }
    bool strong = t.etag[0] && strncmp(t.etag, "W/", 2);
    if (t.offset && ((j->etag[0] && strcmp(j->etag, t.etag)) ||
                     (!j->etag[0] && j->modified[0] && strcmp(j->modified, t.modified)) ||
                     (!j->etag[0] && !j->modified[0]))) {
        copy_text(
            t.error, sizeof t.error,
            "Source identity cannot be verified for resume. Delete the partial file and retry.");
        goto finish;
    }
    pthread_mutex_lock(&atmosphere.mutex);
    if (!t.offset) {
        copy_text(j->etag, sizeof j->etag, strong ? t.etag : "");
        copy_text(j->modified, sizeof j->modified, t.modified);
    }
    j->received = t.offset;
    int saved = state_save_locked();
    pthread_mutex_unlock(&atmosphere.mutex);
    if (saved) {
        copy_text(t.error, sizeof t.error, "Could not persist source identity.");
        goto finish;
    }
    if (t.offset < r->size) {
        t.probe = false;
        t.started = seconds();
        if (lseek(t.fd, (off_t)t.offset, SEEK_SET) < 0) {
            copy_text(t.error, sizeof t.error, "Cannot seek the partial file.");
            goto finish;
        }
        c = request(&t);
        if (!c) {
            copy_text(t.error, sizeof t.error, "Could not initialize transfer.");
            goto finish;
        }
        char range[64];
        snprintf(range, sizeof range, "%lld-", (long long)t.offset);
        curl_easy_setopt(c, CURLOPT_RANGE, range);
        char h[400];
        if (j->etag[0]) {
            snprintf(h, sizeof h, "If-Match: %s", j->etag);
            conditions = curl_slist_append(conditions, h);
        } else if (j->modified[0]) {
            snprintf(h, sizeof h, "If-Unmodified-Since: %s", j->modified);
            conditions = curl_slist_append(conditions, h);
        }
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, conditions);
        rc = curl_easy_perform(c);
        curl_easy_cleanup(c);
        c = NULL;
        if (rc != CURLE_OK || !t.accepted) {
            if (!t.error[0])
                snprintf(t.error, sizeof t.error, "Transfer failed (HTTP %ld, %s).", t.status,
                         curl_easy_strerror(rc));
            goto finish;
        }
    }
    if (fstat(t.fd, &st) || st.st_size != r->size || fsync(t.fd)) {
        copy_text(t.error, sizeof t.error, "Downloaded file size or disk flush failed validation.");
        goto finish;
    }
    pthread_mutex_lock(&atmosphere.mutex);
    copy_text(j->status, sizeof j->status, "verifying");
    j->speed = 0;
    state_save_locked();
    pthread_mutex_unlock(&atmosphere.mutex);
    if (!checksum(&t)) {
        copy_text(t.error, sizeof t.error, "Checksum verification failed. Partial file preserved.");
        goto finish;
    }
    if (controlled(&t))
        goto finish;
    if (!storage_matches(j)) {
        copy_text(t.error, sizeof t.error, "Destination disconnected before finalization.");
        goto finish;
    }
    if (linkat(dir, partial, dir, j->filename, 0)) {
        /* exFAT does not support hard links. Under the single worker, recheck before rename. */
        if (errno != EPERM && errno != EOPNOTSUPP && errno != ENOSYS) {
            copy_text(t.error, sizeof t.error,
                      "Could not finalize without overwriting an existing file.");
            goto finish;
        }
        /* O_EXCL reserves the name; remove only our reservation if rename fails. */
        int reserve = openat(dir, j->filename, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (reserve < 0) {
            copy_text(t.error, sizeof t.error, "Destination filename is already in use.");
            goto finish;
        }
        close(reserve);
        if (renameat(dir, partial, dir, j->filename)) {
            unlinkat(dir, j->filename, 0);
            copy_text(t.error, sizeof t.error, "Could not finalize the completed file.");
            goto finish;
        }
    } else
        unlinkat(dir, partial, 0);
    fsync(dir);
    done = true;
finish:
    if (c)
        curl_easy_cleanup(c);
    curl_slist_free_all(conditions);
    if (t.fd >= 0) {
        fsync(t.fd);
        struct stat actual;
        if (!fstat(t.fd, &actual)) {
            pthread_mutex_lock(&atmosphere.mutex);
            j->received = actual.st_size;
            pthread_mutex_unlock(&atmosphere.mutex);
        }
        close(t.fd);
    }
    pthread_mutex_lock(&atmosphere.mutex);
    j->speed = 0;
    if (done) {
        copy_text(j->status, sizeof j->status, "complete");
        copy_text(j->verification, sizeof j->verification, r->sha256[0] ? "sha256" : "size");
        j->error[0] = 0;
    } else if (j->cancel) {
        copy_text(j->status, sizeof j->status, "cancelled");
        j->error[0] = 0;
        if (j->remove && dir >= 0 && storage_matches(j)) {
            if (unlinkat(dir, partial, 0) == 0) {
                j->received = 0;
                j->etag[0] = 0;
                j->modified[0] = 0;
            } else
                copy_text(j->error, sizeof j->error,
                          "Cancelled, but partial-file deletion failed.");
        }
    } else if (j->pause || atmosphere.stop) {
        copy_text(j->status, sizeof j->status, "paused");
        copy_text(j->error, sizeof j->error,
                  source_enabled_locked(r)
                      ? ""
                      : "Source disabled. Enable it in Sources before resuming.");
    } else if (retryable(t.status, rc) && j->attempts < 3) {
        j->attempts++;
        time_t fallback = time(NULL) + (time_t)(5U << j->attempts);
        j->retry_at = t.retry_at > fallback ? t.retry_at : fallback;
        atmosphere.host_retry_at = j->retry_at;
        copy_text(j->status, sizeof j->status, "retrying");
        copy_text(j->error, sizeof j->error, t.error);
    } else {
        copy_text(j->status, sizeof j->status, "error");
        copy_text(j->error, sizeof j->error, t.error[0] ? t.error : "Transfer interrupted.");
    }
    state_save_locked();
    pthread_mutex_unlock(&atmosphere.mutex);
    if (dir >= 0)
        close(dir);
}
void *download_worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&atmosphere.mutex);
        if (atmosphere.stop) {
            pthread_mutex_unlock(&atmosphere.mutex);
            break;
        }
        Job *next = NULL;
        time_t now = time(NULL);
        if (now >= atmosphere.host_retry_at && !atmosphere.library_storage_busy && !atmosphere.smb_storage_busy)
            for (size_t i = 0; i < atmosphere.job_count; i++) {
                Job *j = &atmosphere.jobs[i];
                if (!strcmp(j->status, "queued") ||
                    (!strcmp(j->status, "retrying") && now >= j->retry_at)) {
                    if (!source_enabled_locked(&j->release)) {
                        copy_text(j->status, sizeof j->status, "paused");
                        copy_text(j->error, sizeof j->error,
                                  "Source disabled. Enable it in Sources before resuming.");
                        state_save_locked();
                        continue;
                    }
                    if (!next || j->order < next->order)
                        next = j;
                }
            }
        if (next) {
            copy_text(next->status, sizeof next->status, "downloading");
            next->error[0] = 0;
            state_save_locked();
            pthread_mutex_unlock(&atmosphere.mutex);
            run_job(next);
        } else {
            struct timespec deadline = {.tv_sec = now + 1, .tv_nsec = 0};
            pthread_cond_timedwait(&atmosphere.changed, &atmosphere.mutex, &deadline);
            pthread_mutex_unlock(&atmosphere.mutex);
        }
    }
    return NULL;
}
