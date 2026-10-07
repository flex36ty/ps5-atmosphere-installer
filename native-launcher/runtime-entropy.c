/* NativeAOT's JSON hashing uses RandomNumberGenerator on Linux. This app-only
 * adapter supplies that narrow API from PS5 system entropy, without a Linux
 * OpenSSL loader. It does not implement hashing, encryption, or TLS APIs.
 * ABI: dotnet/runtime v10.0.0 openssl.c, pal_evp.h and pal_err.h.
 */
#include <stddef.h>
#include <stdint.h>
extern int sceRandomGetRandomNumber(void *, size_t);

int32_t CryptoNative_EnsureOpenSslInitialized(void)
{
    /* The imported system entropy service requires no per-client state. */
    uint8_t probe;
    return sceRandomGetRandomNumber(&probe, 1) < 0 ? -1 : 0;
}

int32_t CryptoNative_GetRandomBytes(uint8_t *buffer, int32_t length)
{
    if (length < 0 || (length && !buffer)) return 0;
    int32_t offset = 0;
    while (offset < length) {
        size_t chunk = length - offset > 64 ? 64 : (size_t)(length - offset);
        if (sceRandomGetRandomNumber(buffer + offset, chunk) < 0) {
            /* Never leave a partially filled result available on failure. */
            volatile uint8_t *p = buffer;
            for (int32_t i = 0; i < length; ++i) p[i] = 0;
            return 0;
        }
        offset += (int32_t)chunk;
    }
    return 1;
}

/* Managed Crypto's shared initializer queries this OpenSSL ABI constant. No
 * digest implementation is linked; introducing one needs a separate port. */
int32_t CryptoNative_GetMaxMdSize(void) { return 64; }

uint64_t CryptoNative_ErrGetExceptionError(int32_t *is_alloc_failure)
{
    if (is_alloc_failure) *is_alloc_failure = 0;
    return 1; /* The sole adapter error: system entropy unavailable/invalid call. */
}

void CryptoNative_ErrErrorStringN(uint64_t error, char *buffer, int32_t length)
{
    (void)error;
    const char message[] = "PS5 system entropy request failed";
    if (!buffer || length <= 0) return;
    int32_t i = 0;
    while (i < length - 1 && message[i]) { buffer[i] = message[i]; ++i; }
    buffer[i] = 0;
}
