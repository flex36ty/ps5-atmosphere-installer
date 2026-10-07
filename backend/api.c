#include "assets.h"
#include "atmosphere.h"
#include <arpa/inet.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#ifdef ATMOSPHERE_TEST
#define ATMOSPHERE_PLATFORM "fixture"
#else
#define ATMOSPHERE_PLATFORM (atmosphere.desktop ? "desktop" : "ps5")
#endif
static struct MHD_Daemon *daemon_handle;
typedef struct {
    char body[4097];
    size_t length;
    bool too_large;
} Request;
static enum MHD_Result respond(struct MHD_Connection *c, unsigned code, const char *body, size_t n,
                               const char *mime) {
    struct MHD_Response *r =
        MHD_create_response_from_buffer(n, (void *)body, MHD_RESPMEM_MUST_COPY);
    if (!r)
        return MHD_NO;
    MHD_add_response_header(r, "Content-Type", mime);
    MHD_add_response_header(r, "X-Content-Type-Options", "nosniff");
    MHD_add_response_header(r, "Referrer-Policy", "no-referrer");
    MHD_add_response_header(r, "Cache-Control", "no-store");
    MHD_add_response_header(r, "Content-Security-Policy",
                            "default-src 'self'; img-src 'self' https: data:; style-src 'self' "
                            "'unsafe-inline'; script-src 'self'; "
                            "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'");
    enum MHD_Result rc = MHD_queue_response(c, code, r);
    MHD_destroy_response(r);
    return rc;
}
static enum MHD_Result json_response(struct MHD_Connection *c, unsigned code, cJSON *o) {
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s)
        return MHD_NO;
    enum MHD_Result rc = respond(c, code, s, strlen(s), "application/json");
    free(s);
    return rc;
}
static enum MHD_Result error(struct MHD_Connection *c, unsigned code, const char *message) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", message);
    return json_response(c, code, o);
}
static bool numeric_host(const char *host) {
    if (!host || strlen(host) > 100)
        return false;
    char h[128];
    copy_text(h, sizeof h, host);
    char *p = strchr(h, ':');
    if (p)
        *p = 0;
    if (!strcmp(h, "localhost"))
        return true;
    struct in_addr a;
    return inet_pton(AF_INET, h, &a) == 1;
}
static bool local_peer(struct MHD_Connection *c) {
    const union MHD_ConnectionInfo *i =
        MHD_get_connection_info(c, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    if (!i || !i->client_addr || i->client_addr->sa_family != AF_INET)
        return false;
    const struct sockaddr_in *a = (const struct sockaddr_in *)i->client_addr;
    return (ntohl(a->sin_addr.s_addr) >> 24) == 127;
}
static bool authorized(struct MHD_Connection *c) {
    const char *h = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Authorization");
    if (!h || strncmp(h, "Bearer ", 7) || strlen(h + 7) != 64)
        return false;
    bool yes = false;
    pthread_mutex_lock(&atmosphere.mutex);
    for (size_t i = 0; i < atmosphere.token_count; i++)
        if (CRYPTO_memcmp(h + 7, atmosphere.tokens[i], 64) == 0)
            yes = true;
    pthread_mutex_unlock(&atmosphere.mutex);
    return yes;
}
static cJSON *storage_json(void) {
    Storage s[ATMOSPHERE_MAX_STORAGE];
    size_t n = storage_list(s);
    cJSON *a = cJSON_CreateArray();
    pthread_mutex_lock(&atmosphere.mutex);
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", s[i].id);
        cJSON_AddStringToObject(o, "label", s[i].label);
        char path[540];
        snprintf(path, sizeof path, "%s/homebrew", s[i].root);
        cJSON_AddStringToObject(o, "path", path);
        cJSON_AddNumberToObject(o, "freeBytes", (double)s[i].free_bytes);
        cJSON_AddNumberToObject(o, "totalBytes", (double)s[i].total_bytes);
        uint64_t pending = storage_pending_locked(&s[i], NULL);
        cJSON_AddNumberToObject(o, "pendingBytes", (double)pending);
        cJSON_AddNumberToObject(o, "projectedFreeBytes", (double)s[i].free_bytes - (double)pending);
        cJSON_AddBoolToObject(o, "external", s[i].external);
        cJSON_AddItemToArray(a, o);
    }
    pthread_mutex_unlock(&atmosphere.mutex);
    return a;
}
static enum MHD_Result route(struct MHD_Connection *c, const char *url, const char *method,
                             Request *req) {
    const char *host = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Host");
    if (!numeric_host(host))
        return error(c, 403, "Use the console IP address to access Atmosphere.");
    bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
    if (!get && !post)
        return error(c, 405, "Method not supported.");
    if (post) {
        const char *origin = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Origin");
        char expected[150];
        snprintf(expected, sizeof expected, "http://%s", host);
        if (origin && strcmp(origin, expected))
            return error(c, 403, "Origin does not match this console.");
        const char *ct = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Content-Type");
        if (!ct || strncasecmp(ct, "application/json", 16))
            return error(c, 415, "Use application/json.");
    }
    if (get && !strcmp(url, "/api/v1/system")) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", "Atmosphere");
        cJSON_AddStringToObject(o, "version", ATMOSPHERE_VERSION);
        cJSON_AddStringToObject(o, "platform", ATMOSPHERE_PLATFORM);
        cJSON_AddBoolToObject(o, "paired", authorized(c));
        cJSON_AddBoolToObject(o, "localSessionAvailable", local_peer(c));
#if defined(ATMOSPHERE_DESKTOP) && !defined(ATMOSPHERE_TEST)
        cJSON_AddBoolToObject(o, "pairNotificationAvailable", false);
#else
        cJSON_AddBoolToObject(o, "pairNotificationAvailable", true);
#endif
        cJSON_AddBoolToObject(o, "consoleValidated", false);
        cJSON_AddStringToObject(o, "targetFirmware", "12.60 / Relapse");
        pthread_mutex_lock(&atmosphere.mutex);
        cJSON_AddBoolToObject(o, "stateHealthy", !atmosphere.state_failed);
        cJSON_AddNumberToObject(o, "catalogueRevision", atmosphere.catalog_revision);
        cJSON_AddStringToObject(o, "preferredStorage", atmosphere.preferred_storage);
        cJSON_AddNumberToObject(o, "httpPort", atmosphere.port);
        cJSON_AddStringToObject(o, "launcherStatus", atmosphere.launcher_status);
        cJSON_AddStringToObject(o, "launcherError", atmosphere.launcher_error);
        cJSON_AddStringToObject(o, "launcherRegistrationMethod",
                                atmosphere.launcher_registration_method);
        pthread_mutex_unlock(&atmosphere.mutex);
        return json_response(c, 200, o);
    }
    if (get && !strcmp(url, "/api/v1/catalog/updates"))
        return json_response(c, 200, catalog_status());
    if (get && !strcmp(url, "/api/v1/catalog")) {
        cJSON *a = cJSON_CreateArray(), *x;
        pthread_mutex_lock(&atmosphere.mutex);
        cJSON_ArrayForEach(x, atmosphere.catalog) {
            if (!source_enabled_locked(release_find(json_text(x, "id"))))
                continue;
            cJSON *entry = cJSON_Duplicate(x, true);
            cJSON_DeleteItemFromObjectCaseSensitive(entry, "url");
            cJSON_AddItemToArray(a, entry);
        }
        pthread_mutex_unlock(&atmosphere.mutex);
        return json_response(c, 200, a);
    }
    if (get && !strcmp(url, "/api/v1/sources")) {
        pthread_mutex_lock(&atmosphere.mutex);
        cJSON *o = sources_json_locked();
        pthread_mutex_unlock(&atmosphere.mutex);
        return json_response(c, 200, o);
    }
    if (get && !strcmp(url, "/api/v1/session")) {
        if (!local_peer(c))
            return error(c, 403, "Pair this device using the code shown on your PS5.");
        cJSON *o = cJSON_CreateObject();
        pthread_mutex_lock(&atmosphere.mutex);
        if (!atmosphere.token_count) {
            random_hex(atmosphere.tokens[0], 32);
            atmosphere.token_count = 1;
        }
        int rc = state_save_locked();
        cJSON_AddStringToObject(o, "token", atmosphere.tokens[0]);
        cJSON_AddStringToObject(o, "pairCode", atmosphere.pair_code);
        pthread_mutex_unlock(&atmosphere.mutex);
        if (rc) {
            cJSON_Delete(o);
            return error(c, 503, "Could not save pairing.");
        }
        return json_response(c, 200, o);
    }
    cJSON *input = NULL;
    if (post) {
        input = cJSON_Parse(req->body);
        if (!cJSON_IsObject(input)) {
            cJSON_Delete(input);
            return error(c, 400, "Expected a JSON object.");
        }
    }
    if (post && !strcmp(url, "/api/v1/pair/show")) {
        cJSON_Delete(input);
        struct timespec clock;
        if (clock_gettime(CLOCK_MONOTONIC, &clock))
            return error(c, 503, "Could not show the pairing code. Try again.");
        pthread_mutex_lock(&atmosphere.mutex);
        time_t remaining = atmosphere.pair_notify_after - clock.tv_sec;
        int rc = 0;
        if (remaining <= 0) {
            rc = pairing_notify();
            /* Failed notifications are throttled too, so repeated requests cannot flood the API. */
            atmosphere.pair_notify_after = clock.tv_sec + 30;
        }
        pthread_mutex_unlock(&atmosphere.mutex);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "retryAfter", remaining > 0 ? (double)remaining : 30);
        if (remaining > 0)
            cJSON_AddStringToObject(o, "error", "Wait a moment before showing the code again.");
        else if (rc)
            cJSON_AddStringToObject(
                o, "error",
                "Could not show a notification. Open Pair devices in Atmosphere on your PS5.");
        else
            cJSON_AddBoolToObject(o, "shown", true);
        /* Only the console displays the code. This response never grants a session or token. */
        return json_response(c, remaining > 0 ? 429 : rc ? 503 : 200, o);
    }
    if (post && !strcmp(url, "/api/v1/pair")) {
        pthread_mutex_lock(&atmosphere.mutex);
        time_t now = time(NULL);
        int code = 200;
        char token[65] = {0};
        if (now < atmosphere.pair_locked_until)
            code = 429;
        else if (strlen(json_text(input, "code")) != 6 ||
                 CRYPTO_memcmp(json_text(input, "code"), atmosphere.pair_code, 6) != 0) {
            code = 403;
            if (++atmosphere.pair_failures >= 5) {
                atmosphere.pair_locked_until = now + 60;
                atmosphere.pair_failures = 0;
            }
        } else if (atmosphere.token_count == 16)
            code = 409;
        else {
            random_hex(token, 32);
            copy_text(atmosphere.tokens[atmosphere.token_count++], 65, token);
            atmosphere.pair_failures = 0;
            if (state_save_locked()) {
                atmosphere.token_count--;
                code = 503;
            }
        }
        pthread_mutex_unlock(&atmosphere.mutex);
        cJSON_Delete(input);
        if (code != 200)
            return error(c, (unsigned)code,
                         code == 429   ? "Too many attempts. Wait one minute."
                         : code == 409 ? "Pairing slots are full."
                                       : "Pairing failed. Check the console code.");
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "token", token);
        return json_response(c, 200, o);
    }
    if (!strncmp(url, "/api/", 5)) {
        if (!authorized(c)) {
            cJSON_Delete(input);
            return error(c, 401, "Pair this device with your PS5 to continue.");
        }
        if (get && !strcmp(url, "/api/v1/storage"))
            return json_response(c, 200, storage_json());
        if (get && !strcmp(url, "/api/v1/smb/status"))
            return json_response(c, 200, smb_snapshot(false));
        if ((get || post) && !strcmp(url, "/api/v1/smb")) {
            char e[256] = {0};
            int code = post ? smb_action(input, e, sizeof e) : 200;
            cJSON_Delete(input);
            return code < 400 ? json_response(c, (unsigned)code, smb_snapshot(true))
                              : error(c, (unsigned)code, e);
        }
        if ((get || post) && !strcmp(url, "/api/v1/library")) {
            cJSON_Delete(input);
            return json_response(c, 200, library_snapshot(post));
        }
        if ((get || post) && !strcmp(url, "/api/v1/library/storage")) {
            cJSON_Delete(input);
            return json_response(c, 200, library_storage_snapshot(post));
        }
        if (post && !strcmp(url, "/api/v1/library/actions")) {
            char e[256] = {0};
            int code = library_action(input, e, sizeof e);
            cJSON_Delete(input);
            return code == 202 ? json_response(c, 202, library_snapshot(false))
                               : error(c, (unsigned)code, e);
        }
        if ((get || post) && !strcmp(url, "/api/v1/favorites")) {
            char e[256] = {0};
            pthread_mutex_lock(&atmosphere.mutex);
            int code = post ? favorite_set_locked(input, e, sizeof e) : 200;
            cJSON *o = code == 200 ? favorites_json_locked() : NULL;
            pthread_mutex_unlock(&atmosphere.mutex);
            cJSON_Delete(input);
            return o ? json_response(c, 200, o) : error(c, (unsigned)code, e);
        }
        if (post && !strcmp(url, "/api/v1/downloads/clear-history")) {
            char e[256] = {0};
            pthread_mutex_lock(&atmosphere.mutex);
            int code = history_clear_locked(input, e, sizeof e);
            cJSON *o = code == 200 ? jobs_json_locked() : NULL;
            pthread_mutex_unlock(&atmosphere.mutex);
            cJSON_Delete(input);
            return o ? json_response(c, 200, o) : error(c, (unsigned)code, e);
        }
        if (post && !strcmp(url, "/api/v1/sources")) {
            char e[256] = {0};
            pthread_mutex_lock(&atmosphere.mutex);
            int code = sources_set_locked(input, e, sizeof e);
            cJSON *o = code == 200 ? sources_json_locked() : NULL;
            pthread_mutex_unlock(&atmosphere.mutex);
            cJSON_Delete(input);
            return o ? json_response(c, 200, o) : error(c, (unsigned)code, e);
        }
        if (get && !strcmp(url, "/api/v1/downloads")) {
            pthread_mutex_lock(&atmosphere.mutex);
            cJSON *o = jobs_json_locked();
            pthread_mutex_unlock(&atmosphere.mutex);
            return json_response(c, 200, o);
        }
        if (post && !strcmp(url, "/api/v1/downloads")) {
            char e[256] = {0};
            Job *j = NULL;
            pthread_mutex_lock(&atmosphere.mutex);
            int code = job_create_locked(json_text(input, "releaseId"),
                                         json_text(input, "storageId"), e, sizeof e, &j);
            cJSON *o = NULL;
            if (code == 201) {
                o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "id", j->id);
            }
            pthread_mutex_unlock(&atmosphere.mutex);
            cJSON_Delete(input);
            return o ? json_response(c, 201, o) : error(c, (unsigned)code, e);
        }
        if (post && !strcmp(url, "/api/v1/catalog/updates")) {
            char e[256] = {0};
            int code =
                !strcmp(json_text(input, "action"), "refresh") ? catalog_refresh(e, sizeof e) : 400;
            cJSON_Delete(input);
            return code == 202 ? json_response(c, 202, catalog_status())
                               : error(c, (unsigned)code, *e ? e : "Expected refresh action.");
        }
        if (get && !strcmp(url, "/api/v1/updates"))
            return json_response(c, 200, updates_status());
        if (post && !strcmp(url, "/api/v1/updates")) {
            char e[256] = {0};
            int code = updates_action(input, e, sizeof e);
            cJSON_Delete(input);
            return code == 202 ? json_response(c, 202, updates_status())
                               : error(c, (unsigned)code, e);
        }
        if (get && !strcmp(url, "/api/v1/autoboot"))
            return json_response(c, 200, autoboot_status());
        if (post && (!strcmp(url, "/api/v1/autoboot") || !strcmp(url, "/api/v1/integrations"))) {
            char e[256] = {0};
            const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(input, "enabled");
            int code = !cJSON_IsBool(enabled) ? 400 :
                !strcmp(url, "/api/v1/integrations")
                    ? integration_set(json_text(input, "manager"), cJSON_IsTrue(enabled), e, sizeof e)
                    : autoboot_set(json_text(input, "manager"), cJSON_IsTrue(enabled), e, sizeof e);
            cJSON_Delete(input);
            if (code == 200)
                return json_response(c, 200, autoboot_status());
            return error(c, (unsigned)code, code == 400 ? "Expected manager and enabled." : e);
        }
        if (post && !strncmp(url, "/api/v1/downloads/", 18)) {
            char id[24] = {0}, action[24] = {0}, trailing;
            int parsed = sscanf(url + 18, "%23[^/]/%23[^/]%c", id, action, &trailing);
            if (parsed != 2) {
                cJSON_Delete(input);
                return error(c, 404, "Unknown download action.");
            }
            char e[256] = {0};
            pthread_mutex_lock(&atmosphere.mutex);
            Job *j = job_find(id);
            int code =
                j ? job_action_locked(
                        j, action,
                        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input, "deletePartial")), e,
                        sizeof e)
                  : 404;
            cJSON *o = code == 200 ? jobs_json_locked() : NULL;
            pthread_mutex_unlock(&atmosphere.mutex);
            cJSON_Delete(input);
            return o ? json_response(c, 200, o)
                     : error(c, (unsigned)code, j ? e : "Unknown download.");
        }
        cJSON_Delete(input);
        return error(c, 404, "Unknown endpoint.");
    }
    cJSON_Delete(input);
    if (!get)
        return error(c, 405, "Method not supported.");
    const char *path = !strcmp(url, "/") ? "/index.html" : url;
    for (size_t i = 0; i < sizeof assets / sizeof assets[0]; i++)
        if (!strcmp(path, assets[i].path))
            return respond(c, 200, (const char *)assets[i].data, assets[i].size, assets[i].mime);
    return error(c, 404, "Not found.");
}
static enum MHD_Result handle(void *cls, struct MHD_Connection *c, const char *url,
                              const char *method, const char *version, const char *data,
                              size_t *size, void **con_cls) {
    (void)cls;
    (void)version;
    if (!*con_cls) {
        *con_cls = calloc(1, sizeof(Request));
        return *con_cls ? MHD_YES : MHD_NO;
    }
    Request *r = *con_cls;
    if (*size) {
        if (r->length + *size > 4096)
            r->too_large = true;
        else {
            memcpy(r->body + r->length, data, *size);
            r->length += *size;
            r->body[r->length] = 0;
        }
        *size = 0;
        return MHD_YES;
    }
    if (r->too_large)
        return error(c, 413, "Request body is too large.");
    return route(c, url, method, r);
}
static void completed(void *cls, struct MHD_Connection *c, void **con_cls,
                      enum MHD_RequestTerminationCode reason) {
    (void)cls;
    (void)c;
    (void)reason;
    free(*con_cls);
    *con_cls = NULL;
}
int api_start(void) {
    struct sockaddr_in bind_address = {0};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons((uint16_t)atmosphere.port);
    bind_address.sin_addr.s_addr =
        htonl(atmosphere.desktop && !atmosphere.listen_all ? INADDR_LOOPBACK : INADDR_ANY);
    daemon_handle = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD, (uint16_t)atmosphere.port, NULL, NULL, handle, NULL,
        MHD_OPTION_SOCK_ADDR, &bind_address, MHD_OPTION_CONNECTION_LIMIT, 32U,
        MHD_OPTION_CONNECTION_TIMEOUT, 15U, MHD_OPTION_CONNECTION_MEMORY_LIMIT, (size_t)32768,
        MHD_OPTION_NOTIFY_COMPLETED, completed, NULL, MHD_OPTION_THREAD_STACK_SIZE,
        (size_t)ATMOSPHERE_THREAD_STACK, MHD_OPTION_END);
    return daemon_handle ? 0 : -1;
}
void api_stop(void) {
    if (daemon_handle)
        MHD_stop_daemon(daemon_handle);
}
