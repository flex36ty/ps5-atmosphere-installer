/* Native OpenGL owner; the existing UI and SMB backend remain isolated modules. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
/* Two 4K backgrounds plus covers and one in-flight PNG decode need >128 MiB. */
const size_t ps5_opengl_heap_size = 256u * 1024u * 1024u;
typedef struct {uint32_t version,size;void *open,*close,*begin,*present,*rect,*text,*measure,*texture,*image,*delete_texture,*error,*clip,*artwork;} Graphics;
typedef struct {uint32_t version,size;int (*run)(void*,void*,void*);} Ui;
extern int atmosphere_gl_start(size_t,void*);
extern int sceKernelLoadStartModule(const char*,size_t,void*,unsigned,void*,int*);
extern void _sceKernelRtldSetApplicationHeapAPI(void*);
/* Match the 0x48-byte allocator table installed by the native libc runtime.
 * Late-loaded PRX TLS must use the same initialized allocator as the host.
 * These references resolve to app_heap's wrappers, including foreign frees. */
static void *tls_heap_api[9];
static int install_tls_heap(void) {
    void *probe=NULL;
    int rc=posix_memalign(&probe,32,4096);
    fprintf(stderr,"[native-host] TLS allocator probe rc=%d address=%p\n",rc,probe);
    fflush(stderr);
    if(rc || !probe)return -1;
    free(probe);
    tls_heap_api[0]=(void*)malloc;
    tls_heap_api[1]=(void*)free;
    tls_heap_api[6]=(void*)posix_memalign;
    _sceKernelRtldSetApplicationHeapAPI(tls_heap_api);
    fprintf(stderr,"[native-host] registered host allocator for dynamic TLS\n");
    fflush(stderr);return 0;
}
int main(void) {
    mkdir("/app0/atmosphere-state",0700);
    Graphics graphics={0};graphics.version=1;graphics.size=sizeof(graphics);
    if(atmosphere_gl_start(sizeof(graphics),&graphics)!=0)return 1;
    fprintf(stderr,"[native-host] opening renderer\n");fflush(stderr);
    if(((int(*)(void))graphics.open)()!=0) {
        fprintf(stderr,"[native-host] renderer failed: %s\n",((const char*(*)(void))graphics.error)());
        return 2;
    }
    if(install_tls_heap()!=0)return 4;
    Ui ui={2,sizeof(Ui),NULL};int status=-1;
    fprintf(stderr,"[native-host] loading UI module\n");fflush(stderr);
    int handle=sceKernelLoadStartModule("/app0/sce_module/atmosphere_ui.prx",sizeof(ui),&ui,0,NULL,&status);
    fprintf(stderr,"[native-host] UI load handle=%x result=%d\n",handle,status);fflush(stderr);
    if(handle>=0 && status==0 && ui.version==2 && ui.size==sizeof(ui) && ui.run) {
        fprintf(stderr,"[native-host] entering Atmosphere UI\n");fflush(stderr);
        status=ui.run(&graphics,(void*)malloc,(void*)free);
        fprintf(stderr,"[native-host] UI returned %d\n",status);fflush(stderr);
    }
    // Keep a readable error on screen; the logs carry the underlying failure.
    for(;;) {
        ((void(*)(uint32_t))graphics.begin)(0xff090e19);
        ((void(*)(const char*,float,float,float,uint32_t,float))graphics.text)("Atmosphere stopped. Close the app and check its startup log.",180,450,30,0xffffffff,1560);
        if(((int(*)(void))graphics.present)()!=0)break;
    }
    ((void(*)(void))graphics.close)();return 3;
}
