#include "install.h"
#include <dlfcn.h>
#include <ps5/kernel.h>
#include <stdint.h>
#include <stdio.h>
/* Same registration path as Payload Manager's own icon: app metadata under /user/app only,
 * no system volume remount and no eboot.bin. */
extern int sceAppInstUtilInitialize(void);
extern int sceAppInstUtilTerminate(void);
extern int sceAppInstUtilAppInstallAll(void *);
typedef int (*install_title_fn)(const char *, const char *, void *);
static install_title_fn install_title;
static int appinst_active;
static const char *registration_method = "not-needed";

const char *atmosphere_launcher_registration_method(void) {
    return registration_method;
}

static int prepare(void) {
    uint32_t handle = 0;
    int result = kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle);
    install_title = result ? NULL : (install_title_fn)kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE");
    /* SDK v0.43 also retains the loaded modules' symbol tables in its runtime
     * linker. Use the exact NID there when the kernel lookup cannot resolve it.
     * A missing optional installer function must not prevent server startup. */
    if (!install_title)
        install_title = (install_title_fn)dlsym(RTLD_DEFAULT, "Wudg3Xe3heE");
    /* Match Payload Manager's compatibility path when TitleDir is unavailable.
     * AppInstallAll is a broader registration scan, not a title-specific call. */
    registration_method = install_title ? "title-directory" : "app-scan";
    result = sceAppInstUtilInitialize();
    if (result) {
        atmosphere_launcher_record_error("initialize-appinst", 0, result);
        return -1;
    }
    appinst_active = 1;
    return 0;
}

static int register_title(void) {
    int result = install_title ? install_title("ORBT00001", "/user/app/", NULL)
                               : sceAppInstUtilAppInstallAll(NULL);
    if (result)
        atmosphere_launcher_record_error(install_title ? "register-title" : "register-app-scan", 0, result);
    printf("Atmosphere launcher registration (%s): 0x%x\n", registration_method, result);
    return result ? -1 : 0;
}

int atmosphere_launcher_ensure(int state_fd) {
    registration_method = "not-needed";
    int result = atmosphere_launcher_ensure_at(state_fd, "/user/app", prepare, register_title);
    /* Release AppInst even when an intervening filesystem operation fails. */
    if (appinst_active) {
        sceAppInstUtilTerminate();
        appinst_active = 0;
    }
    return result;
}
