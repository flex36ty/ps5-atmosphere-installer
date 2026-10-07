/* OpenSSL's x86 assembly declares this 40-byte CPU capability table as common.
 * Provide real zero-initialized storage for the native module linker.
 * OpenSSL's existing initializer populates it at module startup.
 */
unsigned int OPENSSL_ia32cap_P[10] = {0};
