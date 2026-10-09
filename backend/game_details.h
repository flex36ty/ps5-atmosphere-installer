#ifndef ATMOSPHERE_GAME_DETAILS_H
#define ATMOSPHERE_GAME_DETAILS_H
#include "atmosphere.h"
bool game_apply_param(cJSON *game,const unsigned char *data);
bool game_cache_icon(cJSON *game,const unsigned char *data,size_t length);
#endif
