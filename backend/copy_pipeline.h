#ifndef ATMOSPHERE_COPY_PIPELINE_H
#define ATMOSPHERE_COPY_PIPELINE_H
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef int (*CopyFetch)(void *,unsigned char *,uint32_t,uint64_t);
typedef struct {
    pthread_mutex_t mutex;pthread_cond_t changed;pthread_t thread;
    unsigned char *buffers[2];int count[2];bool ready[2],stop,finished;
    unsigned read_slot;uint32_t capacity;uint64_t total;
    CopyFetch fetch;void *context;
} CopyPipeline;
static void *copy_pipeline_worker(void *opaque) {
    CopyPipeline *p=opaque;uint64_t offset=0;unsigned slot=0;
    pthread_mutex_lock(&p->mutex);
    while(offset<p->total && !p->stop) {
        while(p->ready[slot] && !p->stop)pthread_cond_wait(&p->changed,&p->mutex);
        if(p->stop)break;
        uint32_t want=p->total-offset<p->capacity?(uint32_t)(p->total-offset):p->capacity;
        pthread_mutex_unlock(&p->mutex);
        int n=p->fetch(p->context,p->buffers[slot],want,offset);
        pthread_mutex_lock(&p->mutex);
        if(n<=0 || (uint32_t)n>want)n=-1;
        p->count[slot]=n;p->ready[slot]=true;pthread_cond_broadcast(&p->changed);
        if(n<0)break;
        offset+=(uint32_t)n;slot^=1;
    }
    p->finished=true;pthread_cond_broadcast(&p->changed);pthread_mutex_unlock(&p->mutex);return NULL;
}
static int copy_pipeline_start(CopyPipeline *p,unsigned char *a,unsigned char *b,uint32_t capacity,uint64_t total,CopyFetch fetch,void *ctx) {
    memset(p,0,sizeof *p);p->buffers[0]=a;p->buffers[1]=b;p->capacity=capacity;p->total=total;p->fetch=fetch;p->context=ctx;
    if(pthread_mutex_init(&p->mutex,NULL))return -1;
    if(pthread_cond_init(&p->changed,NULL)){pthread_mutex_destroy(&p->mutex);return -1;}
    if(pthread_create(&p->thread,NULL,copy_pipeline_worker,p)){pthread_cond_destroy(&p->changed);pthread_mutex_destroy(&p->mutex);return -1;}
    return 0;
}
/* A returned buffer belongs to the consumer until release, including on error. */
static int copy_pipeline_take(CopyPipeline *p,unsigned char **buffer) {
    pthread_mutex_lock(&p->mutex);unsigned slot=p->read_slot;
    while(!p->ready[slot] && !p->finished)pthread_cond_wait(&p->changed,&p->mutex);
    int n=p->ready[slot]?p->count[slot]:-1;*buffer=p->buffers[slot];pthread_mutex_unlock(&p->mutex);return n;
}
static void copy_pipeline_release(CopyPipeline *p) {
    pthread_mutex_lock(&p->mutex);p->ready[p->read_slot]=false;p->read_slot^=1;pthread_cond_broadcast(&p->changed);pthread_mutex_unlock(&p->mutex);
}
static void copy_pipeline_finish(CopyPipeline *p) {
    pthread_mutex_lock(&p->mutex);p->stop=true;pthread_cond_broadcast(&p->changed);pthread_mutex_unlock(&p->mutex);
    pthread_join(p->thread,NULL);pthread_cond_destroy(&p->changed);pthread_mutex_destroy(&p->mutex);
}
#endif
