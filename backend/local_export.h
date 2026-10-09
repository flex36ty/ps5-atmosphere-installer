#ifndef ATMOSPHERE_LOCAL_EXPORT_H
#define ATMOSPHERE_LOCAL_EXPORT_H
#include "remote_source.h"
#include "atmosphere.h"
typedef void (*ExportProgress)(uint64_t done,uint64_t total,const char *phase,const char *remote_path);
int local_export(RemoteSource *remote,const cJSON *work,const char *folder,
                 bool (*cancel)(void),ExportProgress progress,char error[256]);
#endif
