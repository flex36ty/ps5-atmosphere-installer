#include "atmosphere.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

unsigned source_flag(const char *id) {
    if (!strcmp(id, "archive"))
        return ATMOSPHERE_SOURCE_ARCHIVE;
    if (!strcmp(id, "vikingfile"))
        return ATMOSPHERE_SOURCE_VIKINGFILE;
    return 0;
}

bool source_enabled_locked(const Release *r) {
    if (!r)
        return false;
    /* The pinned connection diagnostic is not a game source. */
    if (!strcmp(r->id, "atmosphere-network-check"))
        return true;
    return atmosphere.sources.notice_version == ATMOSPHERE_SOURCE_NOTICE_VERSION &&
           atmosphere.sources.acknowledged_at > 0 && (atmosphere.sources.enabled & source_flag(r->source));
}

cJSON *sources_json_locked(void) {
    static const struct {
        const char *id, *label;
        bool ready;
    } providers[] = {{"vikingfile", "Vikingfile", false}, {"archive", "Archive.org", true}};
    cJSON *o = cJSON_CreateObject(), *enabled = cJSON_AddArrayToObject(o, "enabled"),
          *options = cJSON_AddArrayToObject(o, "options");
    bool acknowledged = atmosphere.sources.notice_version == ATMOSPHERE_SOURCE_NOTICE_VERSION &&
                        atmosphere.sources.acknowledged_at > 0;
    cJSON_AddBoolToObject(o, "acknowledged", acknowledged);
    cJSON_AddNumberToObject(o, "noticeVersion", ATMOSPHERE_SOURCE_NOTICE_VERSION);
    for (size_t i = 0; i < sizeof providers / sizeof providers[0]; i++) {
        if (acknowledged && (atmosphere.sources.enabled & source_flag(providers[i].id)))
            cJSON_AddItemToArray(enabled, cJSON_CreateString(providers[i].id));
        unsigned count = 0;
        for (size_t j = 0; j < atmosphere.release_count; j++)
            if (!strcmp(atmosphere.releases[j].source, providers[i].id))
                count++;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "id", providers[i].id);
        cJSON_AddStringToObject(p, "label", providers[i].label);
        cJSON_AddNumberToObject(p, "releaseCount", count);
        cJSON_AddBoolToObject(p, "downloadReady", providers[i].ready);
        cJSON_AddItemToArray(options, p);
    }
    return o;
}

int sources_set_locked(const cJSON *input, char *err, size_t cap) {
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(input, "enabled"), *x;
    if (!cJSON_IsArray(enabled) || cJSON_GetArraySize(enabled) > 2) {
        copy_text(err, cap, "Select Vikingfile, Archive.org, both, or neither.");
        return 400;
    }
    unsigned flags = 0;
    cJSON_ArrayForEach(x, enabled) {
        unsigned flag = cJSON_IsString(x) ? source_flag(x->valuestring) : 0;
        if (!flag || (flags & flag)) {
            copy_text(err, cap, "Unknown or duplicate source.");
            return 400;
        }
        flags |= flag;
    }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(input, "noticeVersion");
    if (!cJSON_IsNumber(version) || version->valuedouble != ATMOSPHERE_SOURCE_NOTICE_VERSION ||
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input, "acknowledged"))) {
        copy_text(err, cap,
                  "Read and acknowledge the current download notice before saving sources.");
        return 400;
    }
    /* Settings and queued-job pauses are committed together. A failed write changes neither. */
    SourceSettings old = atmosphere.sources;
    Job *previous = atmosphere.job_count ? malloc(atmosphere.job_count * sizeof(Job)) : NULL;
    if (atmosphere.job_count && !previous) {
        copy_text(err, cap, "Could not save source settings. Try again.");
        return 503;
    }
    if (previous)
        memcpy(previous, atmosphere.jobs, atmosphere.job_count * sizeof(Job));
    atmosphere.sources.enabled = flags;
    atmosphere.sources.notice_version = ATMOSPHERE_SOURCE_NOTICE_VERSION;
    if (old.notice_version != ATMOSPHERE_SOURCE_NOTICE_VERSION || old.acknowledged_at <= 0)
        atmosphere.sources.acknowledged_at = time(NULL);
    for (size_t i = 0; i < atmosphere.job_count; i++) {
        Job *j = &atmosphere.jobs[i];
        if (source_enabled_locked(&j->release))
            continue;
        bool active = !strcmp(j->status, "downloading") || !strcmp(j->status, "verifying");
        if (active || !strcmp(j->status, "queued") || !strcmp(j->status, "retrying")) {
            j->pause = true;
            if (!active)
                copy_text(j->status, sizeof j->status, "paused");
            copy_text(j->error, sizeof j->error,
                      "Source disabled. Enable it in Sources before resuming.");
        }
    }
    if (state_save_locked()) {
        atmosphere.sources = old;
        if (previous)
            memcpy(atmosphere.jobs, previous, atmosphere.job_count * sizeof(Job));
        free(previous);
        copy_text(err, cap, "Could not save source settings. Check console storage.");
        return 503;
    }
    free(previous);
    pthread_cond_signal(&atmosphere.changed);
    return 200;
}
