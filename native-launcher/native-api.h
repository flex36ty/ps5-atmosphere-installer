#include <stdint.h>
#include <stddef.h>
typedef struct AtmosphereApi {
    uint32_t version, size;
    int (*run)(int, char **);
    char *(*request)(const char *);
    void (*release)(void *);
    int (*stage)(void);
    void (*stop)(void);
} AtmosphereApi;
_Static_assert(sizeof(AtmosphereApi)==48, "Native UI ABI layout changed");
int atmosphere_module_start(size_t length, void *argument);
