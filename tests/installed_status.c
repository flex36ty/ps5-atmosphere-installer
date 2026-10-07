/* Provider inventory must work when sandboxed local enumeration fails. */
#include "../backend/installed.c"
#include <assert.h>
static const char *provider;
const char *json_text(const cJSON *object,const char *key){
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(object,key);
    return cJSON_IsString(value)?value->valuestring:"";
}
cJSON *library_snapshot(bool refresh){(void)refresh;return cJSON_Parse(provider);}
int main(void){
    index_started=true;index_complete=false;
    provider="{\"status\":\"ready\",\"stale\":false,\"games\":[{\"titleId\":\"PPSA12345\",\"installed\":true,\"path\":\"/data/game\",\"location\":\"Internal storage\"}]}";
    cJSON *state=installed_snapshot();
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state,"complete")));
    assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state,"checking")));
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(state,"games"))==1);
    cJSON_Delete(state);
    assert(installed_match("PPSA12345"));assert(!installed_match("PPSA99999"));
    provider="{\"status\":\"ready\",\"stale\":false,\"games\":[]}";
    state=installed_snapshot();assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state,"complete")));cJSON_Delete(state);
    provider="{\"status\":\"ready\",\"stale\":true,\"games\":[]}";
    state=installed_snapshot();assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state,"complete")));cJSON_Delete(state);
    provider="{\"status\":\"error\",\"games\":[]}";
    state=installed_snapshot();assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state,"complete")));cJSON_Delete(state);
    puts("PASS: complete provider inventory overrides unavailable local scan; stale/error inventories remain unknown");
    return 0;
}
