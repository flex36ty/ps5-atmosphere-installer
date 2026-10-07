#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
extern int atmosphere_ui_run(void *graphics,void *image_allocate,void *image_free);
extern int atmosphere_runtime_initialize(void);
extern void RhSetRuntimeInitializationCallback(int (*callback)(void));
typedef void (*Initializer)(void);
extern Initializer ui_init_begin[] __asm__("__init_array_start");
extern Initializer ui_init_end[] __asm__("__init_array_end");
static int constructors_ready;
static int initialize_runtime(void) {
    fprintf(stderr,"[native-ui] initializing NativeAOT runtime\n");fflush(stderr);
    int result=atmosphere_runtime_initialize();
    fprintf(stderr,"[native-ui] runtime initialized result=%d\n",result);fflush(stderr);
    return result;
}
typedef struct {uint32_t version,size;int (*run)(void*,void*,void*);} UiApi;
int atmosphere_ui_start(size_t size,void *args) {
    if(!args||size!=sizeof(UiApi))return -1;
    UiApi *api=args;
    if(api->version!=2||api->size!=sizeof(*api))return -1;
    if(!constructors_ready) {
        /* runtime-streams is the first constructor. A populated stderr also
         * detects a loader that already executed the constructor array. */
        if(!stderr) {
            for(Initializer *p=ui_init_begin;p<ui_init_end;++p)
                if(*p && *p!=(Initializer)(intptr_t)-1)(*p)();
        }
        constructors_ready=1;
    }
    /* The native PRX start path does not reliably invoke the ELF init array.
     * Register the DLL bootstrap callback explicitly before any managed entry.
     * This is the same registration performed by libbootstrapperdll's ctor. */
    RhSetRuntimeInitializationCallback(initialize_runtime);
    api->run=atmosphere_ui_run;return 0;
}
