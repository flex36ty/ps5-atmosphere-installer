#include "atmosphere.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "native-api.h"
int atmosphere_backend_run(int argc,char **argv) {(void)argc;(void)argv;return 17;}
int atmosphere_backend_stage(void) {return 9;}
void atmosphere_request_stop(void) {}
static const char *startup_error="";
const char *atmosphere_backend_last_error(void) {return startup_error;}
extern char *atmosphere_native_request(const char *);
extern void atmosphere_native_free(void *);
static int64_t revision=1; static int copies; static bool omit_games;
const char *json_text(const cJSON *o,const char *key) {const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsString(v)?v->valuestring:"";}
int64_t json_int(const cJSON *o,const char *key) {const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsNumber(v)?(int64_t)v->valuedouble:0;}
cJSON *smb_snapshot(bool games) {cJSON *s=cJSON_CreateObject();cJSON_AddNumberToObject(s,"revision",revision);if(games && !omit_games)cJSON_AddArrayToObject(s,"games");return s;}
size_t storage_list(Storage *s) {memset(s,0,sizeof(*s));strcpy(s->id,"usb0");strcpy(s->label,"USB");s->external=true;s->free_bytes=4096;return 1;}
int smb_action(const cJSON *in,char *error,size_t cap) {
 if(!strcmp(json_text(in,"action"),"copy")) {assert(!strcmp(json_text(in,"gameId"),"game-1"));assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(in,"usbRoot")));copies++;return 202;}
 snprintf(error,cap,"Rejected action");return 400;
}
static cJSON *call(const char *in) {char *s=atmosphere_native_request(in);assert(s);cJSON *o=cJSON_Parse(s);atmosphere_native_free(s);assert(o);return o;}
int main(void) {
 AtmosphereApi api={.version=1,.size=sizeof(AtmosphereApi)};
 assert(atmosphere_module_start(0,&api)!=0);
 assert(atmosphere_module_start(sizeof(api),NULL)!=0);
 api.version=2;assert(atmosphere_module_start(sizeof(api),&api)!=0);assert(!api.run);
 api.version=1;assert(atmosphere_module_start(sizeof(api),&api)==0);
 assert(api.run(0,NULL)==17 && api.stage()==9 && api.request==atmosphere_native_request && api.release==atmosphere_native_free && api.stop==atmosphere_request_stop);
 cJSON *o=call("{}");assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(o,"smb"),"games")));assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(o,"storage"))==1);cJSON_Delete(o);
 o=call("{}");assert(!cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(o,"smb"),"games"));cJSON_Delete(o);
 o=call("{\"action\":\"copy\",\"gameId\":\"game-1\",\"usbRoot\":true}");assert(json_int(o,"code")==202&&copies==1);cJSON_Delete(o);
 o=call("{\"action\":\"invalid\"}");assert(json_int(o,"code")==400&&!strcmp(json_text(o,"error"),"Rejected action"));cJSON_Delete(o);
 revision++;o=call("{}");assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(o,"smb"),"games")));cJSON_Delete(o);
 revision++;omit_games=true;o=call("{}");assert(strstr(json_text(o,"error"),"game list"));cJSON_Delete(o);
 omit_games=false;o=call("{}");assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(o,"smb"),"games")));cJSON_Delete(o);
 startup_error="Open saved state: denied";o=call("{}");assert(!strcmp(json_text(o,"error"),startup_error));assert(!cJSON_GetObjectItemCaseSensitive(o,"smb"));cJSON_Delete(o);
 puts("PASS: native bridge, startup handoff and startup error propagation");
}
