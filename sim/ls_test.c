/* Host test for the LiveSync hash and crypto ports against vectors made with
 * the plugin's own JavaScript (sim/ls_vectors.h). Run:
 *   cmake --build sim/build --target ls_test && sim/build/ls_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ls_crypto.h"
#include "ls_hash.h"
#include "ls_vectors.h"

static int fails;
#define CHECK(name, got, want)                                                      \
    do {                                                                            \
        if (strcmp((got), (want)) != 0) {                                           \
            printf("FAIL %s: got %s want %s\n", name, (got), (want));               \
            fails++;                                                                \
        }                                                                           \
    } while (0)

int main(void)
{
    char out[32];
    for (size_t i = 0; i < sizeof(MIXED) / sizeof(MIXED[0]); i++) {
        ls_mixed_hash(MIXED[i].in, MIXED[i].n, out);
        CHECK("mixed", out, MIXED[i].out);
    }
    for (size_t i = 0; i < sizeof(XXH) / sizeof(XXH[0]); i++) {
        uint64_t h = ls_xxh64(XXH[i].in, XXH[i].n, 0);
        char hex[20];
        snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)h);
        CHECK("xxh64 hex", hex, XXH[i].hex);
        ls_base36_u64(h, out);
        CHECK("xxh64 b36", out, XXH[i].b36);
    }
    ls_hashed_passphrase(PASS, out);
    CHECK("hashed passphrase", out, HASHED_PASS);
    for (size_t i = 0; i < sizeof(IDS) / sizeof(IDS[0]); i++) {
        ls_chunk_id(IDS[i].piece, IDS[i].n, HASHED_PASS, out);
        CHECK("chunk id e2ee", out, IDS[i].enc);
        ls_chunk_id(IDS[i].piece, IDS[i].n, NULL, out);
        CHECK("chunk id plain", out, IDS[i].plain);
    }

    ls_master_t m;
    if (!ls_master_derive(PASS, SALT_B64, &m)) {
        printf("FAIL master derive\n");
        return 1;
    }
    size_t n = 0;
    char *plain = ls_decrypt(&m, ENCRYPTED, &n);
    CHECK("decrypt", plain ? plain : "(null)", ROUNDTRIP);
    free(plain);
    char *enc = ls_encrypt(&m, ROUNDTRIP, strlen(ROUNDTRIP));
    char *back = enc ? ls_decrypt(&m, enc, &n) : NULL;
    CHECK("encrypt round trip", back ? back : "(null)", ROUNDTRIP);
    if (enc && strncmp(enc, "%=", 2) != 0) {
        printf("FAIL encrypt prefix\n");
        fails++;
    }
    free(enc);
    free(back);
    ls_master_t wrong;
    ls_master_derive("not the passphrase", SALT_B64, &wrong);
    if (ls_decrypt(&wrong, ENCRYPTED, &n) != NULL) {
        printf("FAIL wrong key accepted\n");
        fails++;
    }
    printf(fails ? "%d FAILED\n" : "all LiveSync vectors pass\n", fails);
    return fails ? 1 : 0;
}
