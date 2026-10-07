#define _POSIX_C_SOURCE 200809L
#include "../backend/copy_pipeline.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
typedef struct {uint64_t fail;unsigned calls;bool short_reads;} Fixture;
static int fetch(void *ctx,unsigned char *data,uint32_t size,uint64_t offset){
    Fixture *f=ctx;f->calls++;if(offset>=f->fail)return -1;
    if(f->short_reads && size>37)size=37;
    for(uint32_t i=0;i<size;i++)data[i]=(unsigned char)((offset+i)%251);
    return (int)size;
}
int main(void){
    unsigned char a[256],b[256],*data;CopyPipeline p;Fixture f={UINT64_MAX,0,true};
    assert(!copy_pipeline_start(&p,a,b,sizeof a,10003,fetch,&f));
    uint64_t offset=0;while(offset<10003){int n=copy_pipeline_take(&p,&data);assert(n>0);for(int i=0;i<n;i++)assert(data[i]==(offset+i)%251);offset+=n;copy_pipeline_release(&p);}copy_pipeline_finish(&p);assert(offset==10003);
    f=(Fixture){UINT64_MAX,0,false};assert(!copy_pipeline_start(&p,a,b,sizeof a,4096,fetch,&f));
    assert(copy_pipeline_take(&p,&data)==256);
    /* The second read finishes while the consumer still holds the first buffer. */
    pthread_mutex_lock(&p.mutex);while(!p.ready[1])pthread_cond_wait(&p.changed,&p.mutex);pthread_mutex_unlock(&p.mutex);
    for(unsigned i=0;i<256;i++)assert(data[i]==i%251);
    copy_pipeline_finish(&p);assert(f.calls==2); /* stop while both slots are occupied */
    f=(Fixture){256,0,false};assert(!copy_pipeline_start(&p,a,b,sizeof a,4096,fetch,&f));
    assert(copy_pipeline_take(&p,&data)==256);copy_pipeline_release(&p);
    assert(copy_pipeline_take(&p,&data)==-1);copy_pipeline_finish(&p);
    puts("PASS: ordered short reads, network/write overlap, held-buffer stability, cancellation and read failure");
}
