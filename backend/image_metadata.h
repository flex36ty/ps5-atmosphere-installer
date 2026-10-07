#ifndef ATMOSPHERE_IMAGE_METADATA_H
#define ATMOSPHERE_IMAGE_METADATA_H
#include <stddef.h>
#include <stdint.h>
/* read_at returns zero only after reading exactly length bytes. */
typedef struct {
    void *context;
    uint64_t size;
    int (*read_at)(void *context, uint64_t offset, void *buffer, size_t length);
    /* Optional sequential DDS sink. Nonzero aborts; caller commits only on success. */
    int skip_background; /* Read titles and icons without touching background payloads. */
    int (*background_write)(void *context, const void *buffer, size_t length);
    int skip_icon; /* Installed-title checks need param.json only. */
} ImageSource;
typedef struct {
    unsigned char *param, *icon, *background;
    size_t param_size, icon_size, background_size;
    int background_streamed;
    int backport_files; /* Root fakelib/fakelib2 directory detected; not compatibility proof. */
    uint64_t bytes_read;
    char status[128];
} ImageMetadata;
/* Bounded, read-only parser. Caller owns returned buffers even on failure. */
int image_metadata_read(const ImageSource *source, ImageMetadata *result);
void image_metadata_free(ImageMetadata *result);
#endif
