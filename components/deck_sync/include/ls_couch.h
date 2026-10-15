/* The little CouchDB the sync needs: JSON in, JSON out, basic auth. Worker
 * task only (blocking HTTP). */
#pragma once

#include <stddef.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char url[160]; /* http(s)://host:5984, no trailing slash */
    char db[48];
    char user[48];
    char pass[64];
} couch_t;

/* `path` is appended to <url>/<db>/ as given (query string and all); encode
 * document ids with couch_encode_id first. The parsed body comes back for
 * any HTTP status (CouchDB error replies are JSON too); NULL means no
 * answer at all (status 0) or a body that was not JSON. */
cJSON *couch_get(const couch_t *c, const char *path, int *status);
cJSON *couch_post(const couch_t *c, const char *path, const char *body, int *status);
cJSON *couch_put(const couch_t *c, const char *path, const char *body, int *status);

/* Percent-encode a document id for a URL path ("/" becomes %2F). */
void couch_encode_id(const char *id, char *out, size_t n);

#ifdef __cplusplus
}
#endif
