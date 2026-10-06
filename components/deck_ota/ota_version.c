#include "deck_ota.h"

#include <stdio.h>
#include <string.h>

/* "v1.2.3" or "1.2.3-beta.1": three numbers, then an optional suffix. */
static const char *parse(const char *s, int v[3])
{
    v[0] = v[1] = v[2] = 0;
    if (s == NULL) return "";
    if (*s == 'v' || *s == 'V') s++;
    sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
    const char *dash = strchr(s, '-');
    return dash ? dash + 1 : "";
}

/* Semver-style: a pre-release ("-beta.1") sorts below the release with the
 * same numbers, so a deck on 0.7.0-beta.1 is offered 0.7.0. */
int ota_version_cmp(const char *a, const char *b)
{
    int x[3], y[3];
    const char *sa = parse(a, x);
    const char *sb = parse(b, y);
    for (int i = 0; i < 3; i++) {
        if (x[i] != y[i]) return x[i] - y[i];
    }
    if (!*sa || !*sb) return (*sa ? -1 : 0) + (*sb ? 1 : 0); /* release beats pre-release */
    return strcmp(sa, sb);
}
