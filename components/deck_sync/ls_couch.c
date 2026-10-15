#include "ls_couch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deck_net.h"

static cJSON *request(const couch_t *c, const char *method, const char *path, const char *body, int *status)
{
    char url[512];
    snprintf(url, sizeof(url), "%s/%s/%s", c->url, c->db, path ? path : "");
    int st     = 0;
    size_t len = 0;
    char *resp = net_http_request(method, url, c->user, c->pass, body, body ? strlen(body) : 0, &st, &len);
    if (status) *status = st;
    if (resp == NULL) return NULL;
    cJSON *j = cJSON_Parse(resp);
    free(resp);
    return j;
}

cJSON *couch_get(const couch_t *c, const char *path, int *status) { return request(c, "GET", path, NULL, status); }
cJSON *couch_post(const couch_t *c, const char *path, const char *body, int *status)
{
    return request(c, "POST", path, body, status);
}
cJSON *couch_put(const couch_t *c, const char *path, const char *body, int *status)
{
    return request(c, "PUT", path, body, status);
}

void couch_encode_id(const char *id, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)id; *p && o + 4 < n; p++) {
        unsigned char ch = *p;
        bool keep = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
                    ch == '_' || ch == '.' || ch == '~';
        if (keep) {
            out[o++] = (char)ch;
        } else {
            out[o++] = '%';
            out[o++] = hex[ch >> 4];
            out[o++] = hex[ch & 15];
        }
    }
    out[o] = '\0';
}
