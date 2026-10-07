#ifndef ATMOSPHERE_GAME_REGION_H
#define ATMOSPHERE_GAME_REGION_H
#include <string.h>
/* Content-ID territory, not console locale, spoken language or region locking. */
static const char *game_region(const char *id) {
    if(!id || strlen(id)!=36 || id[6]!='-' || id[16]!='_' || id[19]!='-')return "";
    for(int i=2;i<6;i++)if(id[i]<'0'||id[i]>'9')return "";
    if(strncmp(id+7,"PPSA",4) && strncmp(id+7,"CUSA",4))return "";
    for(int i=11;i<16;i++)if(id[i]<'0'||id[i]>'9')return "";
    for(int i=17;i<19;i++)if(id[i]<'0'||id[i]>'9')return "";
    for(int i=20;i<36;i++)if(!((id[i]>='A'&&id[i]<='Z')||(id[i]>='0'&&id[i]<='9')||id[i]=='_'))return "";
    if(!strncmp(id,"UP",2))return "US";
    if(!strncmp(id,"EP",2))return "EUR";
    if(!strncmp(id,"JP",2))return "JPN";
    if(!strncmp(id,"HP",2))return "ASIA";
    if(!strncmp(id,"KP",2))return "KOR";
    return "";
}
#endif
