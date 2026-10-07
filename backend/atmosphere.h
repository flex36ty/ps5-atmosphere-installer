#ifndef ATMOSPHERE_H
#define ATMOSPHERE_H
#ifdef ATMOSPHERE_DESKTOP
#define _POSIX_C_SOURCE 200809L
#endif
#include "cJSON.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#define ATMOSPHERE_MAX_JOBS 100
#define ATMOSPHERE_MAX_STORAGE 17
#define ATMOSPHERE_MAX_RELEASES 4096
#define ATMOSPHERE_THREAD_STACK (1024 * 1024)
#define ATMOSPHERE_VERSION "0.4.1"
#define ATMOSPHERE_SOURCE_NOTICE_VERSION 1
#define ATMOSPHERE_SOURCE_ARCHIVE 1U
#define ATMOSPHERE_SOURCE_VIKINGFILE 2U
typedef struct {
    unsigned enabled, notice_version;
    time_t acknowledged_at;
} SourceSettings;
typedef struct {
    char id[24], title[128], title_id[16], format[12], filename[160], url[2048], sha256[65];
    char source[16], game_id[64];
    int64_t size;
} Release;
typedef struct {
    char id[24], label[64], root[512];
    uint64_t free_bytes, total_bytes;
    dev_t device;
    ino_t inode;
    bool external;
    bool capacity_known;
} Storage;
typedef struct {
    Release release; /* Immutable download identity, independent of catalogue refreshes. */
    char id[24], release_id[24], storage_id[24], root[512], filename[160];
    char status[24], error[256], etag[256], modified[128], verification[24];
    int64_t received, total;
    double speed;
    dev_t device;
    ino_t inode;
    unsigned attempts;
    unsigned order;
    time_t retry_at;
    bool pause, cancel, remove;
} Job;
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t worker;
    Release releases[ATMOSPHERE_MAX_RELEASES + 1]; /* Includes the connection diagnostic. */
    size_t release_count;
    cJSON *catalog;
    unsigned catalog_revision;
    SourceSettings sources;
    Job jobs[ATMOSPHERE_MAX_JOBS];
    size_t job_count;
    char favorites[ATMOSPHERE_MAX_RELEASES][64];
    size_t favorite_count;
    char state_dir[512], desktop_storage[512], pair_code[7], tokens[16][65];
    char preferred_storage[24];
    char launcher_status[24];
    char launcher_error[192];
    char launcher_registration_method[24];
    char system_root[512]; /* Prefix for console paths; desktop tests use a disposable tree. */
    size_t token_count;
    unsigned pair_failures;
    time_t pair_locked_until;
    time_t pair_notify_after; /* Monotonic seconds; notification requests share one cooldown. */
    int port;
    bool desktop, listen_all, stop, state_failed, autoboot;
    bool library_storage_busy; /* Serializes Atmosphere transfers with ShadowMount storage jobs. */
    bool smb_storage_busy;
    int state_fd;
    time_t host_retry_at;
} Atmosphere;
extern Atmosphere atmosphere;
void copy_text(char *to, size_t cap, const char *s);
const char *json_text(const cJSON *o, const char *key);
int64_t json_int(const cJSON *o, const char *key);
void random_hex(char *out, size_t bytes);
int state_save_locked(void);
int state_load(void);
Release *release_find(const char *id);
int release_parse(const cJSON *entry, Release *release, bool diagnostic);
cJSON *release_json(const Release *release);
int catalog_init(void);
void catalog_load_cache(void);
int catalog_start(void);
void catalog_stop(void);
cJSON *catalog_status(void);
int catalog_refresh(char *error, size_t cap);
bool provider_supported(const Release *release);
unsigned source_flag(const char *id);
bool source_enabled_locked(const Release *release);
cJSON *sources_json_locked(void);
int sources_set_locked(const cJSON *input, char *error, size_t cap);
Job *job_find(const char *id);
size_t storage_list(Storage *out);
bool storage_has_space(int fd, uint64_t required);
bool storage_matches(const Job *job);
int storage_open(const Job *job);
void *download_worker(void *unused);
cJSON *jobs_json_locked(void);
int job_create_locked(const char *release_id, const char *storage_id, char *error, size_t cap,
                      Job **result);
int job_action_locked(Job *job, const char *action, bool remove, char *error, size_t cap);
bool job_waiting(const Job *job);
uint64_t storage_pending_locked(const Storage *storage, const Job *exclude);
int history_clear_locked(const cJSON *input, char *error, size_t cap);
cJSON *favorites_json_locked(void);
int favorite_set_locked(const cJSON *input, char *error, size_t cap);
int read_regular_file(const char *path, size_t max, unsigned char **data, size_t *length);
int autoboot_install(const unsigned char *image, size_t length, const char *version);
void autoboot_saved_version(char *version, size_t cap);
int updates_start(void);
void updates_stop(void);
cJSON *updates_status(void);
int updates_action(const cJSON *input, char *error, size_t cap);
void atmosphere_request_stop(void);
int autoboot_sync(const unsigned char *image, size_t length);
cJSON *autoboot_status(void);
int autoboot_set(const char *manager, bool enabled, char *error, size_t cap);
int integration_set(const char *manager, bool enabled, char *error, size_t cap);
int api_start(void);
void api_stop(void);
int library_start(void);
bool library_rescan_after_copy(void);
void library_stop(void);
cJSON *library_snapshot(bool refresh);
cJSON *library_storage_snapshot(bool refresh);
int library_action(const cJSON *input, char *error, size_t cap);
int delete_handoff(const cJSON *input,char *error,size_t cap);
cJSON *delete_handoff_result(void);
int pairing_notify(void);
int smb_start(void);
int installed_start(void);
void installed_stop(void);
void installed_refresh(const char *folder);
cJSON *installed_snapshot(void);
bool installed_match(const char *title_id);
void smb_stop(void);
cJSON *smb_snapshot(bool include_games);
int smb_action(const cJSON *input, char *error, size_t cap);
#endif
