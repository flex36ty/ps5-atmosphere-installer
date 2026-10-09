#ifndef REMOTE_SOURCE_H
#define REMOTE_SOURCE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
typedef struct RemoteSource RemoteSource;
typedef struct RemoteFile RemoteFile;
typedef struct RemoteDir RemoteDir;
RemoteSource *remote_smb(struct smb2_context *s);
RemoteSource *remote_ftp(const char *host,unsigned port,const char *user,const char *password,bool (*cancel)(void));
RemoteSource *remote_webdav(const char *host,unsigned port,const char *user,const char *password,bool tls,bool (*cancel)(void));
const char *remote_protocol(RemoteSource *s);
struct smb2_context *remote_smb_context(RemoteSource *s);
struct smb2fh *remote_smb_file(RemoteFile *f);
void remote_destroy(RemoteSource *s);
void remote_forget_smb(RemoteSource *s);
const char *remote_error(RemoteSource *s);
int remote_stat(RemoteSource *s,const char *path,struct smb2_stat_64 *st);
RemoteFile *remote_open(RemoteSource *s,const char *path,int flags);
int remote_fstat(RemoteSource *s,RemoteFile *f,struct smb2_stat_64 *st);
int remote_close(RemoteSource *s,RemoteFile *f);
int remote_pread(RemoteSource *s,RemoteFile *f,void *data,uint32_t length,uint64_t offset);
RemoteDir *remote_opendir(RemoteSource *s,const char *path);
struct smb2dirent *remote_readdir(RemoteSource *s,RemoteDir *dir);
void remote_closedir(RemoteSource *s,RemoteDir *dir);
uint32_t remote_max_read(RemoteSource *s);
/* Writes are confined by the caller to a newly created backup directory. */
int remote_mkdir(RemoteSource *s,const char *path);
int remote_rmdir(RemoteSource *s,const char *path);
int remote_rename(RemoteSource *s,const char *from,const char *to);
int remote_upload(RemoteSource *s,const char *path,uint64_t size,
                  size_t (*read_data)(void *,size_t,void *),void *context);
#endif
