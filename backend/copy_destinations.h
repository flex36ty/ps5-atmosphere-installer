#ifndef COPY_DESTINATIONS_H
#define COPY_DESTINATIONS_H
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdbool.h>
#include "cJSON.h"
/* ShadowMountPlus sm_paths.h defaults and sm_config_mount.c scanpath syntax.
 * Only paths below the selected Storage root become copy destinations. */
static char *destination_trim(char *s){
    while(isspace((unsigned char)*s))s++;
    size_t n=strlen(s);while(n&&isspace((unsigned char)s[n-1]))s[--n]=0;return s;
}
static cJSON *destination_scanpaths(char *text){
    cJSON *paths=cJSON_CreateArray();char *save=NULL;
    for(char *line=strtok_r(text,"\n",&save);line;line=strtok_r(NULL,"\n",&save)){
        char *s=destination_trim(line);if(!*s||strchr("#;[",*s))continue;
        char *eq=strchr(s,'=');if(!eq)continue;*eq++=0;
        if(strcasecmp(destination_trim(s),"scanpath"))continue;
        char *comment=strpbrk(eq,"#;");if(comment)*comment=0;
        char *path=destination_trim(eq);size_t n=strlen(path);
        while(n>1&&path[n-1]=='/')path[--n]=0;
        if(*path&&n<1024)cJSON_AddItemToArray(paths,cJSON_CreateString(path));
    }return paths;
}
static void destination_add(cJSON *list,const char *folder,const char *source){
    cJSON *p;cJSON_ArrayForEach(p,list){
        const cJSON *f=cJSON_GetObjectItemCaseSensitive(p,"folder"),*s=cJSON_GetObjectItemCaseSensitive(p,"sourceId");
        if(f&&s&&!strcmp(f->valuestring,folder)&&(!*s->valuestring||!strcmp(s->valuestring,source)))return;
    }
    cJSON *pnew=cJSON_CreateObject();cJSON_AddStringToObject(pnew,"folder",folder);
    cJSON_AddStringToObject(pnew,"sourceId",source);cJSON_AddItemToArray(list,pnew);
}
/* valid_relative is the same validator used by the descriptor-based copier. */
static cJSON *destination_folders(const cJSON *paths,const char *root,int external,bool (*valid_relative)(const char *)){
    cJSON *result=cJSON_CreateArray();
    if(cJSON_GetArraySize(paths)==0){
        destination_add(result,"homebrew","");destination_add(result,"etaHEN/games","");
        if(external)destination_add(result,"","");
    }else{
        cJSON *p;size_t n=strlen(root);
        cJSON_ArrayForEach(p,paths){
            const char *path=p->valuestring;
            if(!path||strncmp(path,root,n)||(path[n]&&path[n]!='/'))continue;
            const char *folder=path+n;if(*folder=='/')folder++;
            if((!*folder&&!external)||!valid_relative(folder))continue;
            destination_add(result,folder,"");
        }
    }return result;
}
#endif
