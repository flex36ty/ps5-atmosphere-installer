/* Native-only runtime glue, GPL-3.0-or-later. */
void *__dso_handle=&__dso_handle;
/* Mesa declares this optional TLS initializer; its context starts zeroed. */
void atmosphere_gl_tls_init(void) __asm__("_ZTH23_mesa_glapi_tls_Context");
void atmosphere_gl_tls_init(void) {}
