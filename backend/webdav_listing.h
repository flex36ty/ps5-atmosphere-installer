#ifndef ATMOSPHERE_WEBDAV_LISTING_H
#define ATMOSPHERE_WEBDAV_LISTING_H
#include "remote_source.h"
int webdav_listing(const char *xml,size_t length,const char *base,const char *path,struct smb2dirent **entries,size_t *count);
#endif
