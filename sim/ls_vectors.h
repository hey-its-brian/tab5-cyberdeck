/* Generated from the plugin's own code (octagonal-wheels, xxhash-wasm) by
 * a Node script; see sim/ls_test.c. */
#pragma once
#include <stddef.h>
typedef struct { const char *in; size_t n; const char *out; } ls_vec_t;
typedef struct { const char *in; size_t n; const char *b36; const char *hex; } ls_xxvec_t;
typedef struct { const char *piece; size_t n; const char *enc; const char *plain; } ls_idvec_t;
static const ls_vec_t MIXED[] = {
    {"", 0, "hydjuoepw9f3"},
    {"a", 1, "fcnphqu7z6br"},
    {"hello world", 11, "h5jvvqrf4e11"},
    {"a83hrf7f\003y7sa8g31correct horse", 30, "1o0nr46k6fdiy"},
    {"h\303\251llo w\303\266rld \303\274n\303\257code", 23, "a3r99r4vqh9i"},
    {"emoji \360\237\230\200 pair", 15, "44b8wory7hy"},
    {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 300, "1yvl5ot1jyxv3m"},
    {"# Title\012\012line two\012", 18, "1c2c8tw1f5ubgv"},
};
static const ls_xxvec_t XXH[] = {
    {"", 0, "3mzsru4d4ty1l", "ef46db3751d8e999"},
    {"a", 1, "374uz2vg91isb", "d24ec4f1a98c6e5b"},
    {"hello world", 11, "12533ctci3n1k", "45ab6734b21e6968"},
    {"a83hrf7f\003y7sa8g31correct horse", 30, "wwtiewzj1yxc", "3c1df5e509bc4610"},
    {"h\303\251llo w\303\266rld \303\274n\303\257code", 23, "2iz3w9no7sw89", "a62d4fb06c7e0929"},
    {"emoji \360\237\230\200 pair", 15, "1ecmq8a8b5983", "5bf8c898d94d68f3"},
    {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 300, "1zz1q4d33y0fz", "8377db93c80fd5bf"},
    {"# Title\012\012line two\012", 18, "39u1mq9es4tra", "d73d21bc776a13a6"},
};
static const char PASS[] = "correct horse battery staple";
static const char HASHED_PASS[] = "utyp8k1ns6osv";
static const ls_idvec_t IDS[] = {
    {"# Title\012\012line two\012", 18, "h:+3b9krajut9qjh", "h:2mik4ydnwkl04"},
    {"h\303\251llo w\303\266rld \360\237\230\200", 18, "h:+7oy9z78i39dv", "h:3qci5nhul1hd8"},
    {"", 0, "h:+3owezdu5qsgyf", "h:25xaz83tmm99l"},
};
static const char SALT_B64[] = "AAcOFRwjKjE4P0ZNVFtiaXB3foWMk5qhqK+2vcTL0tk=";
static const char ENCRYPTED[] = "%=q++bOAKEcdNzknxhbtPm99nHzkxr2dutVJWcw4IGAPM2/+j+Jr3rsdY/Gw/cu3YwASd3JoRsYIB/7+E4HA6O2uih7OFHIpxB8SWCYzC47QlhVtIr";
static const char ROUNDTRIP[] = "The quick brown fox \360\237\230\200";
