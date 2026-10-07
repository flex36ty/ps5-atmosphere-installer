#include "atmosphere.h"
#include <string.h>
/* Provider boundary: never turn metadata/page URLs into unverified downloads. */
bool provider_supported(const Release *release) {
#ifdef ATMOSPHERE_TEST
    return !strncmp(release->url, "http://127.0.0.1:", 17);
#else
    if (!strcmp(release->id, "atmosphere-network-check"))
        return !strcmp(release->url,
                       "https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.19/LICENSE");
    if (strncmp(release->url, "https://archive.org/download/", 29))
        return false;
    return !strcmp(release->format, "FFPFSC") || !strcmp(release->format, "exFAT");
#endif
}
/* Vikingfile remains disabled until a console-only direct resolver is proven. */
