#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include "../backend/copy_destinations.h"
static bool valid(const char *s){return !strstr(s,"..")&&s[0]!='/'&&!strchr(s,'\\');}
static const char *folder(cJSON *a,int n){return cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(a,n),"folder")->valuestring;}
int main(void){
    char empty[]="# scanpath=/ignored\n";cJSON *paths=destination_scanpaths(empty);
    cJSON *a=destination_folders(paths,"/data",0,valid);assert(cJSON_GetArraySize(a)==2);assert(!strcmp(folder(a,1),"etaHEN/games"));cJSON_Delete(a);
    a=destination_folders(paths,"/mnt/usb0",1,valid);assert(cJSON_GetArraySize(a)==3&&!strcmp(folder(a,2),""));cJSON_Delete(a);cJSON_Delete(paths);
    char config[]="[scan]\n scanpath = /mnt/usb0/PS5/ ; note\nSCANPATH=/data/etaHEN/games\nscanpath=/mnt/usb01/wrong\nscanpath=/mnt/usb0\nscanpath=/mnt/shadowmnt\nscanpath=/mnt/usb0/../bad\nscanpath=/data\n";
    paths=destination_scanpaths(config);a=destination_folders(paths,"/mnt/usb0",1,valid);
    assert(cJSON_GetArraySize(a)==2&&!strcmp(folder(a,0),"PS5")&&!strcmp(folder(a,1),""));
    destination_add(a,"PS5","legacy");assert(cJSON_GetArraySize(a)==2);
    destination_add(a,"custom","source-a");destination_add(a,"custom","source-b");assert(cJSON_GetArraySize(a)==4);cJSON_Delete(a);
    a=destination_folders(paths,"/data",0,valid);assert(cJSON_GetArraySize(a)==1&&!strcmp(folder(a,0),"etaHEN/games"));cJSON_Delete(a);
    a=destination_folders(paths,"/mnt/usb1",1,valid);assert(cJSON_GetArraySize(a)==0);cJSON_Delete(a);cJSON_Delete(paths);
    puts("PASS: defaults, configured roots, comments, root boundaries, traversal rejection and legacy source options");
}
