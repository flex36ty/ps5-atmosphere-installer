#include "atmosphere.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef ATMOSPHERE_DESKTOP
#ifndef ATMOSPHERE_NATIVE_APP
#include <ps5/kernel.h>
#endif
extern int sceKernelSendNotificationRequest(int, const void *, size_t, int);
#endif
#ifdef ATMOSPHERE_NATIVE_APP
static atomic_int backend_stage;
static char backend_error[256];
static atomic_bool backend_failed;
const char *atmosphere_backend_last_error(void) { return atomic_load(&backend_failed) ? backend_error : ""; }
int atmosphere_backend_stage(void) { return atomic_load(&backend_stage); }
#define BACKEND_STAGE(value) atomic_store(&backend_stage, value)
#else
#define BACKEND_STAGE(value) ((void)0)
#endif
static int startup_failure(const char *step, int error) {
#ifdef ATMOSPHERE_NATIVE_APP
    snprintf(backend_error, sizeof backend_error, "%s: %s (code %d)", step,
             error ? strerror(error) : "initialization failed", error);
    atomic_store(&backend_failed, true);
#endif
    return 1;
}
static volatile sig_atomic_t stopping;
static atomic_bool stop_requested;
#ifdef ATMOSPHERE_INSTALL_LAUNCHER
#include "install.h"
#endif
#ifdef ATMOSPHERE_EMBED_RUNTIME
/* The runtime this payload saves on the console; see launcher/runtime_image.c. */
extern const unsigned char atmosphere_runtime_image[], atmosphere_runtime_image_end[];
#endif
static int notify_console(const char *text) {
#ifndef ATMOSPHERE_DESKTOP
    unsigned char notification[3120] = {0};
    snprintf((char *)notification + 45, sizeof notification - 45, "%s", text);
    return sceKernelSendNotificationRequest(0, notification, sizeof notification, 0);
#else
    (void)text;
    return 0;
#endif
}
int pairing_notify(void) {
#if defined(ATMOSPHERE_DESKTOP) && !defined(ATMOSPHERE_TEST)
    return -1;
#else
    char message[160];
    snprintf(message, sizeof message,
             "Atmosphere pairing code: %s\nOpen Pair devices in Atmosphere to keep the code on screen.",
             atmosphere.pair_code);
    return notify_console(message);
#endif
}
static void stop_signal(int n) {
    (void)n;
    stopping = 1;
}
void atmosphere_request_stop(void) {
    atomic_store(&stop_requested, true);
}
#ifdef ATMOSPHERE_NATIVE_APP
int atmosphere_backend_run(int argc, char **argv) {
#else
int main(int argc, char **argv) {
#endif
    BACKEND_STAGE(1);
    setvbuf(stdout, NULL, _IOLBF, 0);
#ifdef ATMOSPHERE_NATIVE_APP
    copy_text(atmosphere.state_dir, sizeof atmosphere.state_dir, "/app0/atmosphere-state");
#else
    copy_text(atmosphere.state_dir, sizeof atmosphere.state_dir, "/data/atmosphere");
#endif
    const unsigned char *runtime = NULL;
    size_t runtime_size = 0;
#ifdef ATMOSPHERE_EMBED_RUNTIME
    runtime = atmosphere_runtime_image;
    runtime_size = (size_t)(atmosphere_runtime_image_end - atmosphere_runtime_image);
#endif
#ifdef ATMOSPHERE_DESKTOP
    atmosphere.desktop = true;
    copy_text(atmosphere.state_dir, sizeof atmosphere.state_dir, ".state");
    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc && !strcmp(argv[i], "--state"))
            copy_text(atmosphere.state_dir, sizeof atmosphere.state_dir, argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--storage"))
            copy_text(atmosphere.desktop_storage, sizeof atmosphere.desktop_storage, argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--port"))
            atmosphere.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--listen-all"))
            atmosphere.listen_all = true;
        else if (i + 1 < argc && !strcmp(argv[i], "--system-root")) {
            copy_text(atmosphere.system_root, sizeof atmosphere.system_root, argv[++i]);
            atmosphere.autoboot = true;
        } else if (i + 1 < argc && !strcmp(argv[i], "--runtime-image")) {
            unsigned char *image;
            if (read_regular_file(argv[++i], 64 * 1024 * 1024, &image, &runtime_size)) {
                perror("runtime image");
                return 1;
            }
            runtime = image;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }
#else
    (void)argc;
    (void)argv;
#ifndef ATMOSPHERE_NATIVE_APP
    atmosphere.autoboot = true;
#endif
#endif
    BACKEND_STAGE(2);
    if (atmosphere.port < 1024 || atmosphere.port > 65535 || catalog_init()) {
        fputs("Invalid configuration/catalogue.\n", stderr);
        return startup_failure("Configuration/catalogue", EINVAL);
    }
#ifdef ATMOSPHERE_TEST
    const char *url = getenv("ATMOSPHERE_TEST_URL"), *size = getenv("ATMOSPHERE_TEST_SIZE"),
               *sha = getenv("ATMOSPHERE_TEST_SHA256");
    if (!url || strncmp(url, "http://127.0.0.1:", 17) || !size) {
        fputs("Tests require a loopback fixture URL and size.\n", stderr);
        return 1;
    }
    copy_text(atmosphere.releases[0].url, sizeof atmosphere.releases[0].url, url);
    atmosphere.releases[0].size = strtoll(size, NULL, 10);
    copy_text(atmosphere.releases[0].sha256, sizeof atmosphere.releases[0].sha256, sha ? sha : "");
    cJSON *fixture = cJSON_GetArrayItem(atmosphere.catalog, 0);
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(fixture, "title"),
                         "Atmosphere local test file");
    cJSON_SetValuestring(
        cJSON_GetObjectItemCaseSensitive(fixture, "description"),
        "A disposable local HTTP fixture for testing the download controls. No PS5 is connected.");
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(fixture, "sizeBytes"),
                         atmosphere.releases[0].size);
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(fixture, "provider"),
                         "Local test server");
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(fixture, "cover"), "/atmosphere.svg");
#endif
    BACKEND_STAGE(3);
    if (mkdir(atmosphere.state_dir, 0700) != 0 && access(atmosphere.state_dir, F_OK) != 0) {
        int error = errno;
        perror("state directory");
        char step[256];
        snprintf(step, sizeof step, "Create %.240s", atmosphere.state_dir);
        return startup_failure(step, error);
    }
    atmosphere.state_fd = open(atmosphere.state_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (atmosphere.state_fd < 0) {
        int error = errno;
        perror("state directory");
        char step[256];
        snprintf(step, sizeof step, "Open %.240s", atmosphere.state_dir);
        return startup_failure(step, error);
    }
    int lock = openat(atmosphere.state_fd, "instance.lock", O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
    if (lock < 0) {
        int error = errno;
        perror("Atmosphere state lock");
#ifdef ATMOSPHERE_NATIVE_APP
        extern const char *atmosphere_file_error(void);
        return startup_failure(atmosphere_file_error()[0] ? atmosphere_file_error() : "Open instance.lock", error);
#else
        return startup_failure("Open instance.lock", error);
#endif
    }
    if (flock(lock, LOCK_EX | LOCK_NB)) {
        int error = errno;
        close(lock);
        close(atmosphere.state_fd);
        if (error == EWOULDBLOCK || error == EAGAIN) {
            /* A newer payload still replaces the saved copy for the next start. */
            int synced = autoboot_sync(runtime, runtime_size);
            if (runtime && synced >= 0) {
                char message[256];
                snprintf(message, sizeof message,
                         "Atmosphere %s saved. The previous session is still running. Stop Atmosphere, then "
                         "run the saved payload to apply it.",
                         ATMOSPHERE_VERSION);
                puts(message);
                notify_console(message);
            } else if (runtime && synced < 0) {
                notify_console("Atmosphere is running, but the replacement could not be fully saved. "
                               "Retry the update; the current session is unchanged.");
            } else {
                puts("Atmosphere is already running. Open the Atmosphere icon; the current queue is "
                     "unchanged.");
                notify_console("Atmosphere is already running. Open its home-screen icon.");
            }
            return 0;
        }
        fputs("Atmosphere state cannot be locked.\n", stderr);
        return startup_failure("Lock saved state", error);
    }
    BACKEND_STAGE(4);
    catalog_load_cache();
    BACKEND_STAGE(5);
    CURLcode curl_result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curl_result != CURLE_OK) {
#ifdef ATMOSPHERE_NATIVE_APP
        snprintf(backend_error,sizeof backend_error,"Initialize TLS: %s (curl %d)",curl_easy_strerror(curl_result),(int)curl_result);
        atomic_store(&backend_failed,true);
#endif
        return 1;
    }
    if (state_load()) {
        fputs("Failed to initialize HTTPS or read saved state. Preserving state for recovery.\n",
              stderr);
        return startup_failure("Read state.json", errno);
    }
    BACKEND_STAGE(6);
#ifndef ATMOSPHERE_NATIVE_APP
    char random[9];
    random_hex(random, 4);
    unsigned long code = strtoul(random, NULL, 16) % 1000000;
    snprintf(atmosphere.pair_code, sizeof atmosphere.pair_code, "%06lu", code);
    printf("Atmosphere %s | %s | port %d\n", ATMOSPHERE_VERSION,
           atmosphere.desktop ? "DESKTOP PREVIEW (game transfers disabled)" : "PS5", atmosphere.port);
    printf("Pairing code: %s\n", atmosphere.pair_code);
#else
    printf("Atmosphere %s | native app\n", ATMOSPHERE_VERSION);
#endif
#if !defined(ATMOSPHERE_DESKTOP) && !defined(ATMOSPHERE_NATIVE_APP)
    printf("Firmware raw: 0x%08x\n", kernel_get_fw_version());
#endif
#ifdef ATMOSPHERE_INSTALL_LAUNCHER
    copy_text(atmosphere.launcher_status, sizeof atmosphere.launcher_status, "checking");
#else
    copy_text(atmosphere.launcher_status, sizeof atmosphere.launcher_status, "not-included");
#endif
#ifndef ATMOSPHERE_NATIVE_APP
    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
#endif
    signal(SIGPIPE, SIG_IGN);
    BACKEND_STAGE(7);
    if (smb_start())
        puts("SMB worker could not start.");
    BACKEND_STAGE(8);
    if (api_start()) {
        char message[200];
        snprintf(message, sizeof message,
                 "Atmosphere cannot listen on TCP port %d. Check for another service using this "
                 "port. No launcher changes were made.",
                 atmosphere.port);
        fprintf(stderr, "%s\n", message);
        notify_console(message);
        return startup_failure("Start API", errno);
    }
    BACKEND_STAGE(9);
    /* Console threads may default to small stacks; curl and OpenSSL need headroom. */
    pthread_attr_t worker_attr;
    int worker_error = pthread_attr_init(&worker_attr);
    if (!worker_error) {
        worker_error = pthread_attr_setstacksize(&worker_attr, ATMOSPHERE_THREAD_STACK);
        if (!worker_error) worker_error = pthread_create(&atmosphere.worker, &worker_attr, download_worker, NULL);
        pthread_attr_destroy(&worker_attr);
    }
    if (worker_error) {
        api_stop();
        return startup_failure("Create download worker", worker_error);
    }
#ifdef ATMOSPHERE_INSTALL_LAUNCHER
    int launcher = atmosphere_launcher_ensure(atmosphere.state_fd);
    pthread_mutex_lock(&atmosphere.mutex);
    copy_text(atmosphere.launcher_status, sizeof atmosphere.launcher_status,
              launcher >= 0 ? "ready" : "error");
    copy_text(atmosphere.launcher_error, sizeof atmosphere.launcher_error, atmosphere_launcher_last_error());
    copy_text(atmosphere.launcher_registration_method, sizeof atmosphere.launcher_registration_method,
              atmosphere_launcher_registration_method());
    pthread_mutex_unlock(&atmosphere.mutex);
#endif
#if defined(ATMOSPHERE_INSTALL_LAUNCHER) || defined(ATMOSPHERE_DESKTOP)
    autoboot_sync(runtime, runtime_size);
#endif
#ifndef ATMOSPHERE_NATIVE_APP
    if (catalog_start())
        puts("Catalogue refresh could not start; the saved catalogue remains available.");
    if (updates_start())
        puts("Atmosphere update worker could not start; the server remains available.");
#endif
    if (library_start())
        puts("Storage information worker could not start.");
#ifndef ATMOSPHERE_NATIVE_APP
    char ready_message[200];
    snprintf(ready_message, sizeof ready_message, "Atmosphere :%d | Pair: %s | %s", atmosphere.port,
             atmosphere.pair_code,
             !strcmp(atmosphere.launcher_status, "ready")   ? "Open the Atmosphere icon"
             : !strcmp(atmosphere.launcher_status, "error") ? "Icon setup failed; server is running"
                                                       : "Server ready");
    notify_console(ready_message);
#endif
    while (!stopping) {
        sleep(1);
        if (atomic_load(&stop_requested)) {
            sleep(1); /* Let the stop acknowledgement reach the browser. */
            stopping = 1;
        }
    }
    api_stop();
    smb_stop();

    library_stop();
#ifndef ATMOSPHERE_NATIVE_APP
    updates_stop();
    catalog_stop();
#endif
    pthread_mutex_lock(&atmosphere.mutex);
    atmosphere.stop = true;
    pthread_cond_signal(&atmosphere.changed);
    pthread_mutex_unlock(&atmosphere.mutex);
    pthread_join(atmosphere.worker, NULL);
    curl_global_cleanup();
    cJSON_Delete(atmosphere.catalog);
    close(lock);
    close(atmosphere.state_fd);
    return 0;
}

