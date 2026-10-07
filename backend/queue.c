#include "atmosphere.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
bool job_waiting(const Job *j) {
    return !strcmp(j->status, "queued") || !strcmp(j->status, "paused") ||
           !strcmp(j->status, "retrying");
}
static uint64_t remaining(const Job *j) {
    if (j->total <= 0 || j->received >= j->total) return 0;
    return (uint64_t)j->total - (j->received > 0 ? (uint64_t)j->received : 0);
}
uint64_t storage_pending_locked(const Storage *s, const Job *exclude) {
    uint64_t pending = 0;
    for (size_t i = 0; i < atmosphere.job_count; i++) {
        const Job *j = &atmosphere.jobs[i];
        if (j == exclude || !j->id[0] || !strcmp(j->status, "complete") ||
            !strcmp(j->status, "cancelled") || strcmp(j->storage_id, s->id) ||
            strcmp(j->root, s->root) || j->device != s->device || j->inode != s->inode)
            continue;
        uint64_t left = remaining(j);
        if (UINT64_MAX - pending < left)
            return UINT64_MAX;
        pending += left;
    }
    return pending;
}
static bool fits(const Storage *s, uint64_t size, const Job *exclude) {
    uint64_t pending = storage_pending_locked(s, exclude), margin = 16 * 1024 * 1024;
    return pending <= s->free_bytes && size <= s->free_bytes - pending &&
           margin <= s->free_bytes - pending - size;
}
/* Never orphan a kept partial file. Removing a history record deletes no disk files. */
static bool removable(const Job *j) {
    if (!strcmp(j->status, "complete"))
        return true;
    if (strcmp(j->status, "cancelled"))
        return false;
    int dir = storage_open(j);
    if (dir < 0)
        return false;
    char partial[192];
    snprintf(partial, sizeof partial, "%s.part", j->filename);
    struct stat st;
    int rc = fstatat(dir, partial, &st, AT_SYMLINK_NOFOLLOW), saved = errno;
    close(dir);
    return rc && saved == ENOENT;
}
int history_clear_locked(const cJSON *input, char *err, size_t cap) {
    const cJSON *filter = cJSON_GetObjectItemCaseSensitive(input, "status");
    const char *status = json_text(input, "status");
    if (filter && (!cJSON_IsString(filter) ||
                   (strcmp(status, "complete") && strcmp(status, "cancelled")))) {
        copy_text(err, cap, "History status must be complete or cancelled.");
        return 400;
    }
    if (atmosphere.stop) {
        copy_text(err, cap, "Atmosphere is stopping.");
        return 503;
    }
    Job *backup = malloc(sizeof atmosphere.jobs);
    if (!backup) {
        copy_text(err, cap, "Could not clear history.");
        return 503;
    }
    memcpy(backup, atmosphere.jobs, sizeof atmosphere.jobs);
    for (size_t i = 0; i < atmosphere.job_count; i++)
        if (atmosphere.jobs[i].id[0] && (!filter || !strcmp(atmosphere.jobs[i].status, status)) &&
            removable(&atmosphere.jobs[i]))
            memset(&atmosphere.jobs[i], 0, sizeof(Job));
    int rc = state_save_locked();
    if (rc) {
        memcpy(atmosphere.jobs, backup, sizeof atmosphere.jobs);
        copy_text(err, cap, "Could not save history changes.");
    }
    free(backup);
    return rc ? 503 : 200;
}
int job_create_locked(const char *rid, const char *sid, char *err, size_t cap, Job **result) {
    if (atmosphere.library_storage_busy) {
        copy_text(err, cap, "A library storage operation is in progress. Wait for it to finish before downloading.");
        return 409;
    }
    if (atmosphere.stop) {
        copy_text(err, cap, "Atmosphere is stopping. Reconnect after restarting it.");
        return 503;
    }
    Release *r = release_find(rid);
    if (!r) {
        copy_text(err, cap, "Unknown curated release.");
        return 404;
    }
    if (!source_enabled_locked(r)) {
        copy_text(err, cap,
                  "Enable this source and acknowledge the download notice in Sources first.");
        return 409;
    }
    if (!provider_supported(r)) {
        copy_text(err, cap, "This provider does not support direct console downloads yet.");
        return 409;
    }
#ifndef ATMOSPHERE_TEST
    if (atmosphere.desktop) {
        copy_text(err, cap, "Desktop preview cannot download games. Connect to Atmosphere on your PS5.");
        return 409;
    }
#endif
    if (atmosphere.state_failed) {
        copy_text(err, cap, "Queue state cannot be saved. Check console storage.");
        return 503;
    }
    long finished = -1, unused = -1;
    unsigned last_order = 0;
    for (size_t i = 0; i < atmosphere.job_count; i++) {
        Job *j = &atmosphere.jobs[i];
        if (!j->id[0]) {
            if (unused < 0)
                unused = (long)i;
            continue;
        }
        if (j->order > last_order)
            last_order = j->order;
        if (!strcmp(j->release_id, rid) && !strcmp(j->storage_id, sid)) {
            /* A completed job is only history. Its slot is reused in place (the worker may hold
               pointers to other jobs), and the file check below still refuses to overwrite. */
            if (!strcmp(j->status, "complete")) {
                finished = (long)i;
                continue;
            }
            copy_text(err, cap,
                      "This release already has a job on that drive. Resume or retry it.");
            return 409;
        }
    }
    if (finished < 0)
        finished = unused;
    if (finished < 0 && atmosphere.job_count >= ATMOSPHERE_MAX_JOBS) {
        copy_text(err, cap, "Queue is full. Clear finished or cancelled history to make room.");
        return 409;
    }
    Storage a[ATMOSPHERE_MAX_STORAGE], *s = NULL;
    size_t n = storage_list(a);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(a[i].id, sid)) {
            s = &a[i];
            break;
        }
    if (!s) {
        copy_text(err, cap, "Selected storage is not connected or writable.");
        return 409;
    }
    if (!fits(s, (uint64_t)r->size, NULL)) {
        copy_text(
            err, cap,
            "Not enough space after unfinished downloads. Free space or cancel a queued download.");
        return 409;
    }
    Job j = {0};
    j.release = *r;
    j.order = last_order + 1;
    random_hex(j.id, 8);
    copy_text(j.release_id, sizeof j.release_id, rid);
    copy_text(j.storage_id, sizeof j.storage_id, sid);
    copy_text(j.root, sizeof j.root, s->root);
    copy_text(j.filename, sizeof j.filename, r->filename);
    copy_text(j.status, sizeof j.status, "queued");
    j.device = s->device;
    j.inode = s->inode;
    j.total = r->size;
    int dir = storage_open(&j);
    if (dir < 0) {
        copy_text(err, cap, "Cannot open the selected homebrew folder.");
        return 409;
    }
    char partial[192];
    snprintf(partial, sizeof partial, "%s.part", j.filename);
    struct stat st;
    bool exists = fstatat(dir, j.filename, &st, AT_SYMLINK_NOFOLLOW) == 0 ||
                  fstatat(dir, partial, &st, AT_SYMLINK_NOFOLLOW) == 0;
    close(dir);
    if (exists) {
        copy_text(err, cap,
                  "A destination or partial file already exists. Atmosphere will not overwrite it.");
        return 409;
    }
    size_t slot = finished >= 0 ? (size_t)finished : atmosphere.job_count;
    Job previous = atmosphere.jobs[slot];
    atmosphere.jobs[slot] = j;
    if (finished < 0)
        atmosphere.job_count++;
    *result = &atmosphere.jobs[slot];
    copy_text(atmosphere.preferred_storage, sizeof atmosphere.preferred_storage, sid);
    if (state_save_locked()) {
        if (finished >= 0)
            atmosphere.jobs[slot] = previous;
        else
            atmosphere.job_count--;
        copy_text(err, cap, "Could not persist the queue.");
        return 503;
    }
    pthread_cond_signal(&atmosphere.changed);
    return 201;
}
int job_action_locked(Job *j, const char *action, bool remove, char *err, size_t cap) {
    if (atmosphere.stop) {
        copy_text(err, cap, "Atmosphere is stopping. Reconnect after restarting it.");
        return 503;
    }
    bool active = !strcmp(j->status, "downloading") || !strcmp(j->status, "verifying");
    if (!strcmp(action, "remove")) {
        if (!removable(j)) {
            copy_text(err, cap,
                      "Only completed or cancelled history can be removed. Reconnect the original "
                      "drive and delete any kept partial file first.");
            return 409;
        }
        Job previous = *j;
        memset(j, 0, sizeof *j);
        if (state_save_locked()) {
            *j = previous;
            copy_text(err, cap, "Could not save history changes.");
            return 503;
        }
        return 200;
    }
    if (!strcmp(action, "move-up") || !strcmp(action, "move-down")) {
        if (!job_waiting(j)) {
            copy_text(err, cap, "Only waiting downloads can be reordered.");
            return 409;
        }
        bool up = !strcmp(action, "move-up");
        Job *neighbor = NULL;
        for (size_t i = 0; i < atmosphere.job_count; i++) {
            Job *other = &atmosphere.jobs[i];
            if (other == j || !job_waiting(other))
                continue;
            if ((up ? other->order < j->order : other->order > j->order) &&
                (!neighbor ||
                 (up ? other->order > neighbor->order : other->order < neighbor->order)))
                neighbor = other;
        }
        if (!neighbor)
            return 200;
        unsigned previous = j->order;
        j->order = neighbor->order;
        neighbor->order = previous;
        if (state_save_locked()) {
            neighbor->order = j->order;
            j->order = previous;
            copy_text(err, cap, "Could not save queue order.");
            return 503;
        }
        return 200;
    }
    if (!strcmp(action, "pause")) {
        if (active)
            j->pause = true;
        else if (!strcmp(j->status, "queued") || !strcmp(j->status, "retrying"))
            copy_text(j->status, sizeof j->status, "paused");
        else {
            copy_text(err, cap, "This job is not running.");
            return 409;
        }
    } else if (!strcmp(action, "resume") || !strcmp(action, "retry")) {
        if (atmosphere.library_storage_busy) {
            copy_text(err, cap, "Wait for the library storage operation to finish before resuming downloads.");
            return 409;
        }
        Release *r = &j->release;
        if (!source_enabled_locked(r) || !provider_supported(r)) {
            copy_text(err, cap,
                      "Enable a supported source in Sources before resuming this download.");
            return 409;
        }
        if (active || !strcmp(j->status, "complete") || !strcmp(j->status, "queued")) {
            copy_text(err, cap, "This job cannot be resumed in its current state.");
            return 409;
        }
        if (!storage_matches(j)) {
            copy_text(err, cap, "Reconnect the original destination drive.");
            return 409;
        }
        Storage drives[ATMOSPHERE_MAX_STORAGE];
        size_t count = storage_list(drives);
        bool enough = false;
        for (size_t i = 0; i < count; i++)
            if (!strcmp(drives[i].id, j->storage_id))
                enough = fits(&drives[i], remaining(j), j);
        if (!enough) {
            copy_text(err, cap,
                      "Not enough space after unfinished downloads. Free space or cancel a queued "
                      "download.");
            return 409;
        }
        j->pause = false;
        j->cancel = false;
        j->remove = false;
        j->attempts = 0;
        j->retry_at = 0;
        j->error[0] = 0;
        copy_text(j->status, sizeof j->status, "queued");
    } else if (!strcmp(action, "cancel")) {
        if (!strcmp(j->status, "complete")) {
            copy_text(err, cap, "Completed files cannot be deleted through cancellation.");
            return 409;
        }
        j->cancel = true;
        j->remove = remove;
        if (!active) {
            if (remove) {
                int d = storage_open(j);
                if (d < 0) {
                    copy_text(err, cap,
                              "Reconnect the original drive before deleting the partial file.");
                    return 409;
                }
                char p[192];
                snprintf(p, sizeof p, "%s.part", j->filename);
                int rc = unlinkat(d, p, 0);
                close(d);
                if (rc && j->received) {
                    copy_text(err, cap, "Could not delete the partial file.");
                    return 409;
                }
                j->received = 0;
                j->etag[0] = 0;
                j->modified[0] = 0;
            }
            copy_text(j->status, sizeof j->status, "cancelled");
        }
    } else {
        copy_text(err, cap, "Unknown queue action.");
        return 404;
    }
    if (state_save_locked()) {
        copy_text(err, cap, "Could not persist the queue change.");
        return 503;
    }
    pthread_cond_signal(&atmosphere.changed);
    return 200;
}
