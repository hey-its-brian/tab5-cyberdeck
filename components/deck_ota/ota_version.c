#include "deck_ota.h"

#include <stdio.h>

static void parse(const char *s, int v[3])
{
    v[0] = v[1] = v[2] = 0;
    if (s && (*s == 'v' || *s == 'V')) s++;
    if (s) sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
}

int ota_version_cmp(const char *a, const char *b)
{
    int x[3], y[3];
    parse(a, x);
    parse(b, y);
    for (int i = 0; i < 3; i++) {
        if (x[i] != y[i]) return x[i] - y[i];
    }
    return 0;
}
