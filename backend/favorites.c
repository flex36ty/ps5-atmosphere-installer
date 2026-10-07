#include "atmosphere.h"
#include <string.h>
cJSON *favorites_json_locked(void) {
    cJSON *a = cJSON_CreateArray();
    for (size_t i = 0; i < atmosphere.favorite_count; i++)
        cJSON_AddItemToArray(a, cJSON_CreateString(atmosphere.favorites[i]));
    return a;
}
int favorite_set_locked(const cJSON *input, char *error, size_t cap) {
    const char *id = json_text(input, "gameId");
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(input, "favorite");
    if (!*id || strlen(id) >= 64 || !cJSON_IsBool(value)) {
        copy_text(error, cap, "Expected a game and favourite choice.");
        return 400;
    }
    if (atmosphere.stop) {
        copy_text(error, cap, "Atmosphere is stopping.");
        return 503;
    }
    size_t index = atmosphere.favorite_count;
    for (size_t i = 0; i < atmosphere.favorite_count; i++)
        if (!strcmp(id, atmosphere.favorites[i]))
            index = i;
    bool add = cJSON_IsTrue(value), exists = index < atmosphere.favorite_count;
    if (add == exists)
        return 200;
    if (add) {
        bool known = false;
        for (size_t i = 0; i < atmosphere.release_count; i++)
            if (!strcmp(id, atmosphere.releases[i].game_id))
                known = true;
        if (!known) {
            copy_text(error, cap, "Game is not in this catalogue.");
            return 404;
        }
        if (atmosphere.favorite_count == ATMOSPHERE_MAX_RELEASES) {
            copy_text(error, cap, "Favourites are full.");
            return 409;
        }
        copy_text(atmosphere.favorites[atmosphere.favorite_count++], 64, id);
    } else {
        /* Swap with the last ID; display order belongs to Browse's chosen sort. */
        atmosphere.favorite_count--;
        if (index != atmosphere.favorite_count)
            copy_text(atmosphere.favorites[index], 64, atmosphere.favorites[atmosphere.favorite_count]);
    }
    if (state_save_locked()) {
        if (add)
            atmosphere.favorite_count--;
        else {
            if (index != atmosphere.favorite_count)
                copy_text(atmosphere.favorites[atmosphere.favorite_count], 64, atmosphere.favorites[index]);
            atmosphere.favorite_count++;
            copy_text(atmosphere.favorites[index], 64, id);
        }
        copy_text(error, cap, "Could not save favourites.");
        return 503;
    }
    return 200;
}
