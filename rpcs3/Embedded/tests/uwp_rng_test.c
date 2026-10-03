#include <wolfssl/options.h>
#include "uwp_rng.h"
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/random.h>
#include <stdio.h>
#include <string.h>
#include <Windows.h>

static int test_rng(void)
{
    unsigned char guarded[66];
    unsigned int i;
    int changed = 0;
    WC_RNG rng;
    memset(guarded, 0xcd, sizeof(guarded));
    if (rpcs3_uwp_generate_seed(NULL, 0) != 0) return 1;
    if (rpcs3_uwp_generate_seed(NULL, 1) != BAD_FUNC_ARG) return 2;
    if (rpcs3_uwp_generate_seed(guarded + 1, 64) != 0) return 3;
    if (guarded[0] != 0xcd || guarded[65] != 0xcd) return 4;
    for (i = 1; i <= 64; ++i) changed |= guarded[i] != 0xcd;
    if (!changed) return 5;
    puts("CNG seed checks passed; initializing wolfCrypt DRBG"); fflush(stdout);
    if (wolfCrypt_Init() != 0) return 6;
    if (wc_InitRng(&rng) != 0) { wolfCrypt_Cleanup(); return 6; }
    if (wc_RNG_GenerateBlock(&rng, guarded + 1, 64) != 0) { wc_FreeRng(&rng); wolfCrypt_Cleanup(); return 7; }
    wc_FreeRng(&rng);
    wolfCrypt_Cleanup();
    if (guarded[0] != 0xcd || guarded[65] != 0xcd) return 8;
    puts("PASS: CNG seed hook and wolfSSL DRBG, zero length, invalid argument and output guards (native PC unit test)");
    return 0;
}

int main(void)
{
    __try { return test_rng(); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printf("FAIL: native exception 0x%08lX in CNG/DRBG unit test\n", GetExceptionCode());
        return 10;
    }
}
