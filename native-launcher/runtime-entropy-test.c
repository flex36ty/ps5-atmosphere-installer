#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
static int calls, fail_at;
int sceRandomGetRandomNumber(void *p, size_t n)
{
    assert(n > 0 && n <= 64);
    if (++calls == fail_at) return -1;
    memset(p, calls, n);
    return 0;
}
extern int32_t CryptoNative_EnsureOpenSslInitialized(void);
extern int32_t CryptoNative_GetRandomBytes(uint8_t *, int32_t);
extern uint64_t CryptoNative_ErrGetExceptionError(int32_t *);
extern void CryptoNative_ErrErrorStringN(uint64_t, char *, int32_t);
int main(void)
{
    uint8_t bytes[130];
    assert(CryptoNative_EnsureOpenSslInitialized() == 0);
    calls=0;
    assert(CryptoNative_GetRandomBytes(bytes, 130) == 1 && calls == 3);
    assert(bytes[0]==1 && bytes[63]==1 && bytes[64]==2 && bytes[129]==3);
    calls=0; fail_at=2;
    assert(CryptoNative_GetRandomBytes(bytes, 130) == 0);
    for(int i=0;i<130;i++) assert(bytes[i]==0);
    calls=0; fail_at=1;
    assert(CryptoNative_EnsureOpenSslInitialized() == -1);
    assert(CryptoNative_GetRandomBytes(NULL, 0) == 1);
    assert(CryptoNative_GetRandomBytes(NULL, 1) == 0);
    assert(CryptoNative_GetRandomBytes(bytes, -1) == 0);
    int32_t alloc=1; assert(CryptoNative_ErrGetExceptionError(&alloc)==1 && alloc==0);
    char message[5]={'x','x','x','x','x'};
    CryptoNative_ErrErrorStringN(1, message, 4);
    assert(strcmp(message,"PS5")==0 && message[4]=='x');
    CryptoNative_ErrErrorStringN(1, NULL, 0);
    puts("Entropy adapter boundary/failure tests: PASS");
}
