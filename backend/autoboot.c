/* Saved runtime copy and opt-in payload-manager integration. External copies and startup
 * entries require separate user choices. Atmosphere never creates an autoloader's
 * autoload.txt (a new one stops autoloaders from opening Payload Manager). Paths are relative to
 * atmosphere.system_root so desktop tests can use a disposable tree; on the console it is empty. */
#include "launcher.h"
#include "atmosphere.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#define SAVED_NAME "atmosphere.elf"
#define ATMOSPHERE_DIR "/data/atmosphere"
#define SAVED ATMOSPHERE_DIR "/" SAVED_NAME
#define PLDMGR_DIR "/data/pldmgr"
#define PLDMGR_COPY PLDMGR_DIR "/payloads/atmosphere/" SAVED_NAME
#define PLDMGR_LIST PLDMGR_DIR "/autoload.txt"
#define PLDMGR_CONFIG PLDMGR_DIR "/pldmgr_config.txt"
#define HOMEBREW_DIR "/data/homebrew"
#define HOMEBREW_APP HOMEBREW_DIR "/AtmosphereStore"
#define MAX_LISTS 18
#define MAX_LIST_BYTES (64 * 1024)
#define MAX_IMAGE_BYTES (64 * 1024 * 1024)
typedef char Path[1100];

static void at(char *out, const char *path) {
    snprintf(out, sizeof(Path), "%s%s", atmosphere.system_root, path);
}
static const char *shown(const char *path) {
    return path + strlen(atmosphere.system_root);
}
static bool is_dir(const char *path) {
    struct stat st;
    return !lstat(path, &st) && S_ISDIR(st.st_mode);
}
static bool is_file(const char *path) {
    struct stat st;
    return !lstat(path, &st) && S_ISREG(st.st_mode);
}
static int ensure_dir(const char *path) {
    if (mkdir(path, 0755) && errno != EEXIST)
        return -1;
    return is_dir(path) ? 0 : -1;
}
/* Regular files only, never through a symlink. */
int read_regular_file(const char *path, size_t max, unsigned char **data, size_t *length) {
    *data = NULL;
    *length = 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > max) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    size_t n = (size_t)st.st_size, p = 0;
    unsigned char *buffer = malloc(n + 1);
    while (buffer && p < n) {
        ssize_t got = read(fd, buffer + p, n - p);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        p += (size_t)got;
    }
    close(fd);
    if (!buffer || p != n) {
        free(buffer);
        errno = EIO;
        return -1;
    }
    buffer[n] = 0;
    *data = buffer;
    *length = n;
    return 0;
}
/* 1 identical, 0 missing or different, -1 not a regular file or unreadable. */
static int same_content(const char *path, const unsigned char *data, size_t length) {
    struct stat st;
    if (lstat(path, &st))
        return errno == ENOENT ? 0 : -1;
    if (!S_ISREG(st.st_mode))
        return -1;
    if ((uint64_t)st.st_size != length)
        return 0;
    unsigned char *old;
    size_t n;
    if (read_regular_file(path, length, &old, &n))
        return -1;
    int same = n == length && !memcmp(old, data, length);
    free(old);
    return same;
}
static int write_atomic(const char *path, const unsigned char *data, size_t length, mode_t mode) {
    Path temp;
    snprintf(temp, sizeof temp, "%s.atmosphere-tmp", path);
    unlink(temp);
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
    if (fd < 0)
        return -1;
    size_t p = 0;
    int rc = 0;
    while (p < length) {
        ssize_t w = write(fd, data + p, length - p);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            rc = -1;
            break;
        }
        p += (size_t)w;
    }
    if (!rc)
        rc = fsync(fd);
    if (close(fd))
        rc = -1;
    if (!rc)
        rc = rename(temp, path);
    if (rc)
        unlink(temp);
    return rc;
}
/* 1 written, 0 already current, -1 refused or failed. */
static int ensure_copy(const char *path, const unsigned char *data, size_t length, mode_t mode) {
    int same = same_content(path, data, length);
    if (same)
        return same > 0 ? 0 : -1;
    return write_atomic(path, data, length, mode) ? -1 : 1;
}
static int saved_image(unsigned char **data, size_t *length) {
    Path p;
    at(p, SAVED);
    if (read_regular_file(p, MAX_IMAGE_BYTES, data, length))
        return -1;
    if (*length < 4 || memcmp(*data, "\177ELF", 4)) {
        free(*data);
        *data = NULL;
        return -1;
    }
    return 0;
}
static int lock_settings(void) {
    Path p;
    at(p, ATMOSPHERE_DIR);
    if (ensure_dir(p))
        return -1;
    at(p, ATMOSPHERE_DIR "/autoboot.lock");
    int fd = open(p, O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
    if (fd >= 0 && flock(fd, LOCK_EX)) {
        close(fd);
        fd = -1;
    }
    return fd;
}

typedef struct { bool payload_manager, homebrew_launcher; } Integrations;
/* Absence means no consent, including on upgrades from versions that copied automatically. */
static int integrations_read(Integrations *settings) {
    memset(settings, 0, sizeof *settings);
    Path path;
    at(path, ATMOSPHERE_DIR "/integrations.json");
    unsigned char *data; size_t length;
    if (read_regular_file(path, 4096, &data, &length))
        return errno == ENOENT ? 0 : -1;
    cJSON *json = cJSON_ParseWithLengthOpts((const char *)data, length + 1, NULL, true);
    free(data);
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(json, "schemaVersion"),
          *pm = cJSON_GetObjectItemCaseSensitive(json, "payloadManager"),
          *hb = cJSON_GetObjectItemCaseSensitive(json, "homebrewLauncher");
    bool valid = cJSON_IsObject(json) && cJSON_IsNumber(schema) && schema->valuedouble == 1 &&
                 cJSON_IsBool(pm) && cJSON_IsBool(hb);
    if (valid) {
        settings->payload_manager = cJSON_IsTrue(pm);
        settings->homebrew_launcher = cJSON_IsTrue(hb);
    }
    cJSON_Delete(json);
    return valid ? 0 : -1;
}
static int integrations_save(const Integrations *settings) {
    Path path;
    at(path, ATMOSPHERE_DIR "/integrations.json");
    char data[128];
    int n = snprintf(data, sizeof data,
        "{\"schemaVersion\":1,\"payloadManager\":%s,\"homebrewLauncher\":%s}\n",
        settings->payload_manager ? "true" : "false", settings->homebrew_launcher ? "true" : "false");
    return ensure_copy(path, (unsigned char *)data, (size_t)n, 0600) < 0 ? -1 : 0;
}

/* Start-up lists: one payload per line, "!ms" delays, "#" comments, "@" directives. */
static void trim(const char **s, size_t *n) {
    while (*n && strchr(" \t\r", (*s)[*n - 1]))
        (*n)--;
    while (*n && strchr(" \t", **s)) {
        (*s)++;
        (*n)--;
    }
}
static bool atmosphere_line(const char *s, size_t n) {
    trim(&s, &n);
    size_t k = strlen(SAVED_NAME);
    return n >= k && !memcmp(s + n - k, SAVED_NAME, k) && (n == k || s[n - k - 1] == '/');
}
static bool payload_line(const char *s, size_t n) {
    trim(&s, &n);
    return n && !strchr("!#@", *s);
}
static void scan_list(const unsigned char *data, size_t n, bool *atmosphere_present, int *others) {
    *atmosphere_present = false;
    *others = 0;
    for (size_t i = 0; i < n;) {
        size_t end = i;
        while (end < n && data[end] != '\n')
            end++;
        if (atmosphere_line((const char *)data + i, end - i))
            *atmosphere_present = true;
        else if (payload_line((const char *)data + i, end - i))
            (*others)++;
        i = end + 1;
    }
}
static int read_list(const char *path, bool *atmosphere_present, int *others) {
    unsigned char *data;
    size_t n;
    *atmosphere_present = false;
    *others = 0;
    if (read_regular_file(path, MAX_LIST_BYTES, &data, &n))
        return -1;
    scan_list(data, n, atmosphere_present, others);
    free(data);
    return 0;
}
/* Adds or removes Atmosphere's line; every other line is kept byte for byte. */
static int edit_list(const char *path, bool add, bool create, int *others) {
    unsigned char *old;
    size_t n;
    if (read_regular_file(path, MAX_LIST_BYTES, &old, &n)) {
        if (errno != ENOENT || !create || !(old = calloc(1, 1)))
            return -1;
        n = 0;
    }
    bool present;
    int count;
    scan_list(old, n, &present, &count);
    if (others)
        *others = count;
    unsigned char *out = malloc(n + sizeof SAVED_NAME + 2);
    if (!out) {
        free(old);
        return -1;
    }
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        size_t end = i;
        while (end < n && old[end] != '\n')
            end++;
        size_t next = end < n ? end + 1 : end;
        if (add || !atmosphere_line((const char *)old + i, end - i)) {
            memcpy(out + o, old + i, next - i);
            o += next - i;
        }
        i = next;
    }
    if (add && !present) {
        if (o && out[o - 1] != '\n')
            out[o++] = '\n';
        memcpy(out + o, SAVED_NAME "\n", sizeof SAVED_NAME);
        o += sizeof SAVED_NAME;
    }
    int rc = 0;
    if (o != n || memcmp(out, old, n)) {
        struct stat st;
        rc = write_atomic(path, out, o, lstat(path, &st) ? 0644 : (st.st_mode & 0777));
    }
    free(out);
    free(old);
    return rc;
}

static bool pldmgr_switch_on(void) {
    Path p;
    unsigned char *data;
    size_t n;
    at(p, PLDMGR_CONFIG);
    if (read_regular_file(p, MAX_LIST_BYTES, &data, &n))
        return false;
    const char *line = strstr((const char *)data, "AUTOLOAD_ENABLED=");
    bool on = false;
    while (line) {
        if (line == (const char *)data || line[-1] == '\n')
            on = atoi(line + 17) == 1;
        line = strstr(line + 1, "AUTOLOAD_ENABLED=");
    }
    free(data);
    return on;
}

static int by_path(const void *a, const void *b) {
    return strcmp(a, b);
}
/* Existing autoload.txt files of the ps5_autoloader family, on internal storage and USB. */
static size_t autoload_lists(Path *lists, size_t max) {
    static const char *bases[] = {"/data",     "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
                                  "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7"};
    size_t count = 0;
    for (size_t b = 0; b < sizeof bases / sizeof bases[0] && count < max; b++) {
        Path base;
        at(base, bases[b]);
        DIR *d = opendir(base);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d)) && count < max) {
            if (strcmp(e->d_name, "ps5_autoloader") && strncmp(e->d_name, "ps5_autoloader_", 15))
                continue;
            Path dir;
            snprintf(dir, sizeof dir, "%s/%s", base, e->d_name);
            snprintf(lists[count], sizeof lists[count], "%s/autoload.txt", dir);
            if (is_dir(dir) && is_file(lists[count]))
                count++;
        }
        closedir(d);
    }
    qsort(lists, count, sizeof *lists, by_path);
    return count;
}
static void beside(char *out, const char *list) {
    snprintf(out, sizeof(Path), "%.*s/" SAVED_NAME, (int)(strrchr(list, '/') - list), list);
}

static int list_in_payload_manager(const unsigned char *elf, size_t n, const char *version) {
    Path p;
    at(p, PLDMGR_DIR "/payloads");
    if (ensure_dir(p))
        return -1;
    at(p, PLDMGR_DIR "/payloads/atmosphere");
    if (ensure_dir(p))
        return -1;
    at(p, PLDMGR_COPY);
    if (ensure_copy(p, elf, n, 0755) < 0)
        return -1;
    /* Preserve import/source fields while keeping the displayed version current. */
    at(p, PLDMGR_COPY ".json");
    cJSON *meta = NULL;
    unsigned char *old;
    size_t old_size;
    if (!read_regular_file(p, MAX_LIST_BYTES, &old, &old_size)) {
        meta = cJSON_Parse((const char *)old);
        free(old);
        if (!cJSON_IsObject(meta)) {
            cJSON_Delete(meta);
            return -1;
        }
    } else if (errno != ENOENT) {
        return -1;
    }
    if (!meta)
        meta = cJSON_CreateObject();
    char description[128];
    snprintf(description, sizeof description,
             "Download manager for PS5. Open the Atmosphere icon or port %d.", atmosphere.port);
    if (!cJSON_HasObjectItem(meta, "name"))
        cJSON_AddStringToObject(meta, "name", "Atmosphere");
    if (!cJSON_HasObjectItem(meta, "description"))
        cJSON_AddStringToObject(meta, "description", description);
    if (version && *version) {
        cJSON_DeleteItemFromObjectCaseSensitive(meta, "version");
        cJSON_AddStringToObject(meta, "version", version);
    }
    char *text = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    int rc = text ? ensure_copy(p, (unsigned char *)text, strlen(text), 0644) : -1;
    free(text);
    return rc < 0 ? -1 : 0;
}
static int list_in_homebrew_launcher(const unsigned char *elf, size_t n) {
    Path p;
    at(p, HOMEBREW_APP);
    if (ensure_dir(p))
        return -1;
    at(p, HOMEBREW_APP "/sce_sys");
    if (ensure_dir(p))
        return -1;
    at(p, HOMEBREW_APP "/eboot.elf");
    if (ensure_copy(p, elf, n, 0755) < 0)
        return -1;
    at(p, HOMEBREW_APP "/sce_sys/icon0.png");
    return ensure_copy(p, launcher_icon, sizeof launcher_icon, 0644) < 0 ? -1 : 0;
}
/* Keeps every copy Atmosphere placed for a payload manager identical to the saved runtime. */
static int refresh_copies(const unsigned char *elf, size_t n, const char *version) {
    Integrations settings;
    if (integrations_read(&settings)) {
        puts("Atmosphere: manager preferences could not be read; external copies are unchanged.");
        return -1;
    }
    int failed = 0;
    Path p;
    at(p, PLDMGR_DIR);
    if (settings.payload_manager && is_dir(p) && list_in_payload_manager(elf, n, version)) {
        failed = 1;
        printf("Atmosphere auto-start: could not list Atmosphere in Payload Manager.\n");
    }
    at(p, HOMEBREW_DIR);
    if (settings.homebrew_launcher && is_dir(p) && list_in_homebrew_launcher(elf, n)) {
        failed = 1;
        printf("Atmosphere auto-start: could not list Atmosphere in Homebrew Launcher.\n");
    }
    Path lists[MAX_LISTS];
    size_t count = autoload_lists(lists, MAX_LISTS);
    for (size_t i = 0; i < count; i++) {
        bool present;
        int others;
        if (read_list(lists[i], &present, &others) || !present)
            continue;
        beside(p, lists[i]);
        if (ensure_copy(p, elf, n, 0755) < 0) {
            failed = 1;
            printf("Atmosphere auto-start: could not update %s\n", shown(p));
        }
    }
    return failed ? -1 : 0;
}

void autoboot_saved_version(char *version, size_t cap) {
    Path p;
    unsigned char *data;
    size_t n;
    version[0] = 0;
    at(p, ATMOSPHERE_DIR "/saved-version.txt");
    if (!read_regular_file(p, 63, &data, &n)) {
        copy_text(version, cap, (char *)data);
        free(data);
    }
}

int autoboot_install(const unsigned char *image, size_t length, const char *version) {
    if (!atmosphere.autoboot)
        return 0;
    int lock = lock_settings();
    if (lock < 0) {
        puts("Atmosphere auto-start: start-up settings cannot be locked.");
        return -1;
    }
    int updated = 0;
    if (image) {
        Path p;
        at(p, SAVED);
        updated = ensure_copy(p, image, length, 0755);
        if (updated < 0)
            puts("Atmosphere auto-start: cannot save " SAVED ".");
    }
    if (updated >= 0 && image && version) {
        Path p;
        at(p, ATMOSPHERE_DIR "/saved-version.txt");
        if (ensure_copy(p, (const unsigned char *)version, strlen(version), 0600) < 0)
            updated = -1;
    }
    unsigned char *elf;
    size_t n;
    char saved_version[64];
    autoboot_saved_version(saved_version, sizeof saved_version);
    if (!saved_image(&elf, &n)) {
        if (refresh_copies(elf, n, saved_version))
            updated = -1;
        free(elf);
    } else {
        updated = -1;
    }
    close(lock);
    return updated;
}

int autoboot_sync(const unsigned char *image, size_t length) {
    return autoboot_install(image, length, image ? ATMOSPHERE_VERSION : NULL);
}

cJSON *autoboot_status(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "available", atmosphere.autoboot);
    if (!atmosphere.autoboot)
        return o;
    Integrations settings;
    if (integrations_read(&settings))
        cJSON_AddStringToObject(o, "preferencesError", "Manager preferences could not be read. Automatic copy updates are paused.");
    Path p;
    bool present;
    int others;
    at(p, SAVED);
    cJSON_AddStringToObject(o, "savedPath", SAVED);
    cJSON_AddBoolToObject(o, "saved", is_file(p));
    cJSON *pm = cJSON_AddObjectToObject(o, "payloadManager");
    cJSON_AddBoolToObject(pm, "managed", settings.payload_manager);
    at(p, PLDMGR_DIR);
    cJSON_AddBoolToObject(pm, "installed", is_dir(p));
    at(p, PLDMGR_COPY);
    cJSON_AddBoolToObject(pm, "listed", is_file(p));
    at(p, PLDMGR_LIST);
    read_list(p, &present, &others);
    cJSON_AddBoolToObject(pm, "enabled", present);
    cJSON_AddBoolToObject(pm, "switchOn", pldmgr_switch_on());
    cJSON_AddNumberToObject(pm, "otherEntries", others);
    cJSON *files = cJSON_AddArrayToObject(cJSON_AddObjectToObject(o, "autoloadTxt"), "files");
    Path lists[MAX_LISTS];
    size_t count = autoload_lists(lists, MAX_LISTS);
    for (size_t i = 0; i < count; i++) {
        cJSON *f = cJSON_CreateObject();
        read_list(lists[i], &present, &others);
        cJSON_AddStringToObject(f, "path", shown(lists[i]));
        cJSON_AddBoolToObject(f, "enabled", present);
        cJSON_AddItemToArray(files, f);
    }
    cJSON *hb = cJSON_AddObjectToObject(o, "homebrewLauncher");
    cJSON_AddBoolToObject(hb, "managed", settings.homebrew_launcher);
    at(p, HOMEBREW_DIR);
    cJSON_AddBoolToObject(hb, "installed", is_dir(p));
    at(p, HOMEBREW_APP "/eboot.elf");
    cJSON_AddBoolToObject(hb, "listed", is_file(p));
    at(p, "/data/etaHEN");
    cJSON_AddBoolToObject(cJSON_AddObjectToObject(o, "etaHEN"), "installed", is_dir(p));
    return o;
}

int integration_set(const char *manager, bool enable, char *err, size_t cap) {
    if (!atmosphere.autoboot) {
        copy_text(err, cap, "Manager integration is set up from Atmosphere on your PS5.");
        return 409;
    }
    bool pm = !strcmp(manager, "payload-manager");
    if (!pm && strcmp(manager, "homebrew-launcher")) {
        copy_text(err, cap, "Unknown payload manager.");
        return 404;
    }
    int lock = lock_settings();
    if (lock < 0) {
        copy_text(err, cap, "Atmosphere's manager settings are busy. Try again.");
        return 503;
    }
    int code = 200;
    Integrations settings;
    Path path;
    unsigned char *elf = NULL; size_t length = 0;
    at(path, pm ? PLDMGR_DIR : HOMEBREW_DIR);
    if (integrations_read(&settings)) {
        copy_text(err, cap, "Manager preferences could not be read. No manager files were changed.");
        code = 503;
    } else if (enable && !is_dir(path)) {
        copy_text(err, cap, "This payload manager is not installed.");
        code = 409;
    } else if (enable && saved_image(&elf, &length)) {
        copy_text(err, cap, "Atmosphere's saved copy is missing. Run atmosphere.elf again.");
        code = 409;
    } else {
        if (pm) settings.payload_manager = enable;
        else settings.homebrew_launcher = enable;
        if (integrations_save(&settings)) {
            copy_text(err, cap, "Could not save your choice. No manager files were changed.");
            code = 503;
        } else if (enable) {
            char version[64];
            autoboot_saved_version(version, sizeof version);
            if (pm ? list_in_payload_manager(elf, length, version) : list_in_homebrew_launcher(elf, length)) {
                copy_text(err, cap, "Your choice was saved, but Atmosphere could not finish adding its copy. Retry setup.");
                code = 503;
            }
        }
    }
    free(elf);
    close(lock);
    return code;
}

int autoboot_set(const char *manager, bool enable, char *err, size_t cap) {
    if (!atmosphere.autoboot) {
        copy_text(err, cap, "Auto-start is set up from Atmosphere on your PS5.");
        return 409;
    }
    bool payload_manager = !strcmp(manager, "payload-manager");
    if (!payload_manager && strcmp(manager, "autoload-txt")) {
        copy_text(err, cap, "Unknown payload manager.");
        return 404;
    }
    int lock = lock_settings();
    if (lock < 0) {
        copy_text(err, cap, "Atmosphere's start-up settings are busy. Try again.");
        return 503;
    }
    unsigned char *elf = NULL;
    size_t n = 0;
    int code = 200;
    Path p;
    bool have_saved = !saved_image(&elf, &n);
    if (enable && !have_saved) {
        copy_text(err, cap, "Atmosphere's saved copy is missing. Run atmosphere.elf again.");
        code = 409;
    } else if (payload_manager) {
        Integrations settings;
        int others = 0;
        at(p, PLDMGR_DIR);
        bool installed = is_dir(p);
        at(p, PLDMGR_LIST);
        if (!installed) {
            copy_text(err, cap, "Payload Manager isn't installed on this console.");
            code = 409;
        } else if (enable && (integrations_read(&settings) || !settings.payload_manager)) {
            copy_text(err, cap, "Choose Add Atmosphere under Payload managers before enabling auto-start.");
            code = 409;
        } else if ((enable && list_in_payload_manager(elf, n, NULL)) ||
                   edit_list(p, enable, true, &others)) {
            copy_text(err, cap, "Could not update Payload Manager's autoload list.");
            code = 503;
        }
    } else {
        Path lists[MAX_LISTS];
        size_t count = autoload_lists(lists, MAX_LISTS);
        if (!count) {
            copy_text(err, cap,
                      "No autoload.txt was found. Atmosphere won't create one, because a new "
                      "autoload.txt stops your autoloader from opening Payload Manager.");
            code = 409;
        }
        for (size_t i = 0; i < count && code == 200; i++) {
            beside(p, lists[i]);
            if (enable ? ensure_copy(p, elf, n, 0755) < 0 || edit_list(lists[i], true, false, NULL)
                       : edit_list(lists[i], false, false, NULL)) {
                snprintf(err, cap, "Could not update %s.", shown(lists[i]));
                code = 503;
            } else if (!enable && have_saved && same_content(p, elf, n) == 1)
                unlink(p);
        }
    }
    free(elf);
    close(lock);
    return code;
}
