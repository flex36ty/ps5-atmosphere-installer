/* In-process UI bridge. Ownership of returned JSON stays with this module. */
#include "atmosphere.h"
#include <stdlib.h>
#include <string.h>
#include "native-api.h"
extern int atmosphere_backend_run(int, char **);
extern int atmosphere_backend_stage(void);
extern const char *atmosphere_backend_last_error(void);
extern void atmosphere_request_stop(void);
int api_start(void) { return 0; } /* Native UI has no HTTP listener. */
void api_stop(void) {}
char *atmosphere_native_request(const char *request) {
    const char *startup_error = atmosphere_backend_last_error();
    if (*startup_error) {
        cJSON *failure = cJSON_CreateObject();
        cJSON_AddStringToObject(failure, "error", startup_error);
        char *result = cJSON_PrintUnformatted(failure);
        cJSON_Delete(failure);
        return result;
    }
    cJSON *in = request ? cJSON_Parse(request) : NULL;
    char error[256] = {0};
    int code = 200;
    if (in && !strcmp(json_text(in,"action"),"deleteInstalled")) {
        code=delete_handoff(in,error,sizeof error);
    } else if (in && *json_text(in, "action")) code = smb_action(in, error, sizeof error);
    bool close_for_delete=in&&!strcmp(json_text(in,"action"),"deleteInstalled")&&code==200;
    cJSON_Delete(in);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "code", code);
    cJSON_AddBoolToObject(out,"closeForDelete",close_for_delete);
    cJSON *delete_result=delete_handoff_result();if(delete_result)cJSON_AddItemToObject(out,"deleteResult",delete_result);
    cJSON_AddStringToObject(out, "error", error);
    static int64_t last_revision = -1;
    cJSON *snapshot=smb_snapshot(false);
    int64_t revision=json_int(snapshot,"revision");
    if(revision!=last_revision) {
        cJSON_Delete(snapshot); snapshot=smb_snapshot(true);
        if (cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(snapshot,"games"))) last_revision=revision;
        else cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(out,"error"),"Cannot copy the game list: insufficient memory. Please rescan.");
    }
    cJSON_AddItemToObject(out, "smb", snapshot);
    Storage drives[ATMOSPHERE_MAX_STORAGE]; size_t count = storage_list(drives);
    /* The native sandbox may deny statfs. ShadowMount's worker queries the
     * backing filesystems outside that sandbox; snapshots never block on HTTP. */
    cJSON *capacity=library_storage_snapshot(false);
    bool capacity_fresh=capacity &&
        cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(capacity,"stale"));
    cJSON *list = cJSON_AddArrayToObject(out, "storage");
    for (size_t i=0; i<count; i++) {
        if(!drives[i].capacity_known && capacity_fresh) {
            for(int kind=0;kind<2 && !drives[i].capacity_known;kind++) {
                cJSON *entry;
                cJSON_ArrayForEach(entry,cJSON_GetObjectItemCaseSensitive(capacity,kind?"destinations":"drives")) {
                    const char *path=json_text(entry,"path");
                    size_t length=strlen(drives[i].root);
                    /* PS5 /data is a nullfs view of /user/data. Its device ID
                     * differs from the backing BFS mount, and /user may not be
                     * exposed inside the app sandbox. This is display metadata;
                     * write access is still checked on /data itself. */
                    bool same_internal=!kind && !strcmp(drives[i].root,"/data") &&
                        !strcmp(path,"/user") && !strcmp(json_text(entry,"filesystem"),"bfs");
                    if(!same_internal && strcmp(path,drives[i].root) &&
                       !(kind && !strncmp(path,drives[i].root,length) && !strcmp(path+length,"/homebrew")))continue;
                    drives[i].free_bytes=(uint64_t)json_int(entry,"freeBytes");
                    drives[i].total_bytes=(uint64_t)json_int(entry,"totalBytes");
                    drives[i].capacity_known=true;
                    break;
                }
            }
        }
        cJSON *s=cJSON_CreateObject();
        cJSON_AddStringToObject(s,"id",drives[i].id);
        cJSON_AddStringToObject(s,"label",drives[i].label);
        cJSON_AddNumberToObject(s,"freeBytes",(double)drives[i].free_bytes);
        cJSON_AddBoolToObject(s,"capacityKnown",drives[i].capacity_known);
        cJSON_AddBoolToObject(s,"external",drives[i].external);
        cJSON_AddItemToArray(list,s);
    }
    cJSON_Delete(capacity);
    char *result=cJSON_PrintUnformatted(out); cJSON_Delete(out); return result;
}
void atmosphere_native_free(void *result) { free(result); }
int atmosphere_module_start(size_t length, void *argument) {
    if (length != sizeof(AtmosphereApi) || !argument) return -22;
    AtmosphereApi *api = argument;
    if (api->version != 1 || api->size != sizeof(*api)) return -22;
    api->run = atmosphere_backend_run;
    api->request = atmosphere_native_request;
    api->release = atmosphere_native_free;
    api->stage = atmosphere_backend_stage;
    api->stop = atmosphere_request_stop;
    return 0;
}
