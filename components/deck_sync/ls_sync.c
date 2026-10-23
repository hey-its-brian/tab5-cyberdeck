/*
 * Self-hosted LiveSync backend: two-way sync of markdown notes between
 * <storage>/vault and the plugin's CouchDB database.
 *
 * Supported database shape (the plugin's defaults since 0.25): E2EE "v2"
 * (HKDF) or no encryption, hashAlg xxhash64, no path obfuscation, no
 * compression. Anything else is refused up front with a clear log line.
 *
 * State between runs lives in <vault>/.deck-sync.json: the _changes
 * sequence we have seen and, per note, the revision we hold and the local
 * file time it was written with. A local file whose time differs from that
 * record has been edited here and is pushed; a remote revision that differs
 * from the record is pulled. Both at once: the remote copy wins the path and
 * the local text is kept beside it as "<name> (deck conflict).md", which the
 * next push sends up, so nothing is lost.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "deck_hal.h"
#include "deck_net.h"
#include "deck_sync.h"
#include "ls_couch.h"
#include "ls_crypto.h"
#include "ls_hash.h"
#include "mbedtls/base64.h"

#define INDEX_FILE ".deck-sync.json"
#define PIECE_MAX 3000   /* bytes per chunk, split at newlines */
#define CHANGES_LIMIT 25 /* note metadata only, but a page must fit in RAM twice (text + parsed) */
#define NOTE_MAX (256 * 1024)

typedef struct {
    couch_t db;
    char passphrase[128];
    bool encrypt;
    bool case_sensitive;
    ls_master_t master;
    char hashed_pass[16];
    sync_log_fn log;
    volatile bool *cancel;
    const char *dir;
    cJSON *index;  /* {"seq": "...", "files": {path: {rev, mtime, local}}, "retry": {id: 1}} */
    cJSON *files;
    cJSON *retry;  /* documents whose chunks could not be fetched last time */
    int pulled, pushed, deleted, skipped, conflicts;
    int legacy; /* notes stored the old inline way or as binary */
    bool dead; /* the server stopped answering; stop instead of grinding on */
    bool full; /* ignore the stored sequence: walk everything again */
} ctx_t;

static unsigned heap_kb(void)
{
    hal_sysinfo_t si;
    hal_sysinfo(&si);
    return (unsigned)(si.heap_int_free / 1024);
}

static void logf_(ctx_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(ctx_t *c, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    c->log(buf);
}

/* ---- Settings ------------------------------------------------------------ */

static bool configured(void)
{
    char url[160], db[48];
    return hal_cfg_get_str(SYNC_KEY_URL, url, sizeof(url)) && url[0] && hal_cfg_get_str(SYNC_KEY_DB, db, sizeof(db)) &&
           db[0];
}

static void load_settings(ctx_t *c)
{
    hal_cfg_get_str(SYNC_KEY_URL, c->db.url, sizeof(c->db.url));
    size_t n = strlen(c->db.url);
    while (n && c->db.url[n - 1] == '/') c->db.url[--n] = '\0';
    hal_cfg_get_str(SYNC_KEY_DB, c->db.db, sizeof(c->db.db));
    hal_cfg_get_str(SYNC_KEY_USER, c->db.user, sizeof(c->db.user));
    hal_cfg_get_str(SYNC_KEY_PASS, c->db.pass, sizeof(c->db.pass));
    hal_cfg_get_str(SYNC_KEY_PHRASE, c->passphrase, sizeof(c->passphrase));
    c->encrypt = c->passphrase[0] != '\0';
}

/* ---- Index --------------------------------------------------------------- */

static void index_path(ctx_t *c, char *out, size_t n) { snprintf(out, n, "%s/%s", c->dir, INDEX_FILE); }

static void index_load(ctx_t *c)
{
    char path[160];
    index_path(c, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (n > 0 && n < 512 * 1024) ? malloc((size_t)n + 1) : NULL;
        if (buf) {
            buf[fread(buf, 1, (size_t)n, f)] = '\0';
            c->index = cJSON_Parse(buf);
            free(buf);
        }
        fclose(f);
    }
    if (c->index == NULL || !cJSON_IsObject(c->index)) {
        cJSON_Delete(c->index);
        c->index = cJSON_CreateObject();
    }
    c->files = cJSON_GetObjectItemCaseSensitive(c->index, "files");
    if (!cJSON_IsObject(c->files)) {
        cJSON_DeleteItemFromObjectCaseSensitive(c->index, "files");
        c->files = cJSON_AddObjectToObject(c->index, "files");
    }
    c->retry = cJSON_GetObjectItemCaseSensitive(c->index, "retry");
    if (!cJSON_IsObject(c->retry)) {
        cJSON_DeleteItemFromObjectCaseSensitive(c->index, "retry");
        c->retry = cJSON_AddObjectToObject(c->index, "retry");
    }
}

static bool index_save(ctx_t *c)
{
    char path[160], tmp[168];
    index_path(c, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    char *text = cJSON_PrintUnformatted(c->index);
    if (text == NULL) return false;
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(text, 1, strlen(text), f) == strlen(text);
    if (f) ok = (fclose(f) == 0) && ok;
    free(text);
    if (!ok) return false;
    unlink(path);
    return rename(tmp, path) == 0;
}

static void index_set(ctx_t *c, const char *path, const char *rev, double mtime, time_t local)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "rev", rev);
    cJSON_AddNumberToObject(e, "mtime", mtime);
    cJSON_AddNumberToObject(e, "local", (double)local);
    cJSON_DeleteItemFromObjectCaseSensitive(c->files, path);
    cJSON_AddItemToObject(c->files, path, e);
}

/* ---- Paths --------------------------------------------------------------- */

static bool is_md(const char *p)
{
    size_t n = strlen(p);
    return n > 3 && strcmp(p + n - 3, ".md") == 0;
}

/* Vault path to document id: lowercased unless the database keeps case,
 * and a "/" in front when it starts with "_" (CouchDB reserves those). */
static void path_to_id(ctx_t *c, const char *path, char *out, size_t n)
{
    size_t o = 0;
    if (path[0] == '_' && o < n - 1) out[o++] = '/';
    for (const char *p = path; *p && o < n - 1; p++) {
        char ch = *p;
        if (!c->case_sensitive && ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        out[o++] = ch;
    }
    out[o] = '\0';
}

static void file_path(ctx_t *c, const char *vpath, char *out, size_t n) { snprintf(out, n, "%s/%s", c->dir, vpath); }

/* mkdir -p for the folders of a vault path. */
static void ensure_dirs(ctx_t *c, const char *vpath)
{
    char full[320];
    file_path(c, vpath, full, sizeof(full));
    for (char *p = full + strlen(c->dir) + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(full, 0755);
            *p = '/';
        }
    }
}

static bool safe_vault_path(const char *p)
{
    if (p[0] == '\0' || p[0] == '/' || strstr(p, "..") || strlen(p) > 200) return false;
    for (const char *q = p; *q; q++) {
        if ((unsigned char)*q < 0x20 || *q == '\\' || *q == ':') return false;
    }
    return true;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (n >= 0 && n <= NOTE_MAX) ? malloc((size_t)n + 1) : NULL;
    size_t got = buf ? fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    if (buf == NULL) return NULL;
    buf[got] = '\0';
    if (len) *len = got;
    return buf;
}

static bool write_file(const char *path, const char *text, size_t len)
{
    char tmp[340];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) return false;
    bool ok = fwrite(text, 1, len, f) == len;
    ok      = (fclose(f) == 0) && ok;
    if (!ok) {
        unlink(tmp);
        return false;
    }
    unlink(path);
    return rename(tmp, path) == 0;
}

static time_t file_mtime(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

/* ---- Preflight ----------------------------------------------------------- */

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool jbool(const cJSON *o, const char *key, bool def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

static double jnum(const cJSON *o, const char *key, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static bool preflight(ctx_t *c)
{
    int st = 0;
    cJSON *j = couch_get(&c->db, "", &st);
    if (j == NULL || st == 0) {
        logf_(c, "cannot reach %s", c->db.url);
        cJSON_Delete(j);
        return false;
    }
    if (st == 401 || st == 403) {
        logf_(c, "login refused (HTTP %d)", st);
        cJSON_Delete(j);
        return false;
    }
    if (st != 200) {
        logf_(c, "database %s: HTTP %d", c->db.db, st);
        cJSON_Delete(j);
        return false;
    }
    cJSON_Delete(j);

    /* The plugin's version marker: 12 (0.25 and 1.0) or 13 with a feature list. */
    j = couch_get(&c->db, "obsydian_livesync_version", &st);
    if (st == 404) {
        logf_(c, "not a LiveSync database (no version document)");
        cJSON_Delete(j);
        return false;
    }
    int ver = j ? (int)jnum(j, "version", 0) : 0;
    const cJSON *features = j ? cJSON_GetObjectItemCaseSensitive(j, "used_features") : NULL;
    if (ver < 12 || ver > 13) {
        logf_(c, "database version %d is not supported (need 12 or 13)", ver);
        cJSON_Delete(j);
        return false;
    }
    if (cJSON_IsArray(features) && cJSON_GetArraySize(features) > 0) {
        const cJSON *f;
        cJSON_ArrayForEach(f, features)
        {
            logf_(c, "unsupported feature: %s", cJSON_IsString(f) ? f->valuestring : "?");
        }
        cJSON_Delete(j);
        return false;
    }
    cJSON_Delete(j);

    /* The milestone carries the settings every device must share. */
    j = couch_get(&c->db, "_local/obsydian_livesync_milestone", &st);
    if (j && st == 200) {
        if (jbool(j, "locked", false)) {
            logf_(c, "database is locked (rebuilt); let Obsidian finish first");
            cJSON_Delete(j);
            return false;
        }
        const cJSON *tweaks = cJSON_GetObjectItemCaseSensitive(j, "tweak_values");
        const cJSON *pref   = cJSON_GetObjectItemCaseSensitive(tweaks, "PREFERRED");
        if (!cJSON_IsObject(pref) && cJSON_IsObject(tweaks)) pref = tweaks->child; /* any node will do */
        if (cJSON_IsObject(pref)) {
            bool enc           = jbool(pref, "encrypt", false);
            c->case_sensitive  = jbool(pref, "handleFilenameCaseSensitive", false);
            const char *alg    = jstr(pref, "hashAlg");
            const char *e2ee   = jstr(pref, "E2EEAlgorithm");
            bool obfuscate     = jbool(pref, "usePathObfuscation", false);
            bool compress      = jbool(pref, "enableCompression", false);
            int id_version     = (int)jnum(pref, "idDerivationVersion", 0);
            if (enc != c->encrypt) {
                logf_(c, enc ? "database is encrypted: set the passphrase" : "database is not encrypted: clear the passphrase");
                cJSON_Delete(j);
                return false;
            }
            if (alg && strcmp(alg, "xxhash64") != 0) {
                logf_(c, "hash algorithm %s is not supported", alg);
                cJSON_Delete(j);
                return false;
            }
            if (enc && e2ee && strcmp(e2ee, "v2") != 0) {
                logf_(c, "encryption algorithm \"%s\" is not supported (need v2)", e2ee);
                cJSON_Delete(j);
                return false;
            }
            if (obfuscate || compress || id_version != 0) {
                logf_(c, "%s is on in the plugin; not supported",
                      obfuscate ? "path obfuscation" : compress ? "compression" : "the independent id key");
                cJSON_Delete(j);
                return false;
            }
        }
    }
    cJSON_Delete(j);

    if (c->encrypt) {
        j = couch_get(&c->db, "_local/obsidian_livesync_sync_parameters", &st);
        const char *salt = j ? jstr(j, "pbkdf2salt") : NULL;
        if (salt == NULL || salt[0] == '\0') {
            logf_(c, "no encryption salt yet; sync once from Obsidian first");
            cJSON_Delete(j);
            return false;
        }
        /* The master key is 310000 PBKDF2 rounds: seconds on the deck, so it
         * is kept across boots, keyed by the salt it came from. */
        char cached_salt[64] = "", cached_key[72] = "";
        hal_cfg_get_str("ls_msalt", cached_salt, sizeof(cached_salt));
        hal_cfg_get_str("ls_mkey", cached_key, sizeof(cached_key));
        bool hit = strcmp(cached_salt, salt) == 0 && strlen(cached_key) == 64;
        if (hit) {
            for (int i = 0; i < 32; i++) {
                unsigned v;
                sscanf(cached_key + i * 2, "%2x", &v);
                c->master.key[i] = (uint8_t)v;
            }
            c->master.valid = true;
        } else {
            logf_(c, "deriving the key (takes a moment)...");
            if (!ls_master_derive(c->passphrase, salt, &c->master)) {
                logf_(c, "key derivation failed");
                cJSON_Delete(j);
                return false;
            }
            char hex[72];
            for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", c->master.key[i]);
            hal_cfg_set_str("ls_mkey", hex);
            hal_cfg_set_str("ls_msalt", salt);
        }
        cJSON_Delete(j);
        ls_hashed_passphrase(c->passphrase, c->hashed_pass);

        /* syncinfo is 30 random letters under the same key: a cheap check
         * that the passphrase is right before touching any note. */
        j = couch_get(&c->db, "syncinfo", &st);
        const char *data = j ? jstr(j, "data") : NULL;
        if (data && st == 200) {
            char *plain = ls_decrypt(&c->master, data, NULL);
            if (plain == NULL) {
                logf_(c, "wrong passphrase");
                hal_cfg_set_str("ls_mkey", NULL); /* do not trust the cache either */
                cJSON_Delete(j);
                return false;
            }
            free(plain);
        }
        cJSON_Delete(j);
    }
    return true;
}

/* ---- Pull ---------------------------------------------------------------- */

/* Fetch and decode a note's chunks into one malloc'd string. */
/* Decode one piece of a note: decrypt "%=" strings, and for binary
 * ("newnote") documents base64-decode the result. Returns malloc'd bytes. */
static char *decode_piece(ctx_t *c, const char *data, bool binary, size_t *plen, const char **why)
{
    char *plain = NULL;
    size_t n    = 0;
    if (data[0] == '%' && data[1] == '=') {
        plain = ls_decrypt(&c->master, data, &n);
        if (plain == NULL) {
            *why = "would not decrypt";
            return NULL;
        }
    } else if (data[0] == '%' || data[0] == '[' || data[0] == '\x0e') {
        *why = "uses an older format";
        return NULL;
    } else {
        n     = strlen(data);
        plain = malloc(n + 1);
        if (plain == NULL) return NULL;
        memcpy(plain, data, n + 1);
    }
    if (binary) {
        size_t cap = n / 4 * 3 + 4, out_n = 0;
        unsigned char *bytes = malloc(cap + 1);
        if (bytes == NULL || mbedtls_base64_decode(bytes, cap, &out_n, (const unsigned char *)plain, n) != 0) {
            free(bytes);
            free(plain);
            *why = "is not valid base64";
            return NULL;
        }
        free(plain);
        bytes[out_n] = '\0';
        plain        = (char *)bytes;
        n            = out_n;
    }
    *plen = n;
    return plain;
}

static bool append(char **out, size_t *tot, const char *src, size_t n)
{
    if (*tot + n > NOTE_MAX) return false;
    char *grown = realloc(*out, *tot + n + 1);
    if (grown == NULL) return false;
    *out = grown;
    memcpy(*out + *tot, src, n);
    *tot += n;
    (*out)[*tot] = '\0';
    return true;
}

static char *fetch_note(ctx_t *c, const cJSON *doc, size_t *len)
{
    const char *type      = jstr(doc, "type");
    bool binary           = type && strcmp(type, "newnote") == 0;
    const cJSON *children = cJSON_GetObjectItemCaseSensitive(doc, "children");
    const cJSON *eden     = cJSON_GetObjectItemCaseSensitive(doc, "eden");
    int n = cJSON_IsArray(children) ? cJSON_GetArraySize(children) : 0;
    char *out = malloc(1);
    if (out == NULL) return NULL;
    out[0]     = '\0';
    size_t tot = 0;

    /* The oldest form keeps the text inline, as one string or a list. */
    const cJSON *inline_data = cJSON_GetObjectItemCaseSensitive(doc, "data");
    if (type && strcmp(type, "notes") == 0 && inline_data) {
        const cJSON *piece;
        const cJSON *first = cJSON_IsArray(inline_data) ? inline_data->child : inline_data;
        for (piece = first; piece; piece = cJSON_IsArray(inline_data) ? piece->next : NULL) {
            if (!cJSON_IsString(piece)) continue;
            size_t plen     = 0;
            const char *why = "";
            char *plain     = decode_piece(c, piece->valuestring, false, &plen, &why);
            if (plain == NULL || !append(&out, &tot, plain, plen)) {
                logf_(c, "note %s %s", jstr(doc, "path") ? jstr(doc, "path") : "?", plain ? "too large" : why);
                free(plain);
                free(out);
                return NULL;
            }
            free(plain);
        }
        if (len) *len = tot;
        return out;
    }

    if (n == 0) {
        if (len) *len = 0;
        return out;
    }
    cJSON *req  = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "keys", cJSON_Duplicate(children, true));
    char *body  = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    int st      = 0;
    cJSON *resp = body ? couch_post(&c->db, "_all_docs?include_docs=true", body, &st) : NULL;
    free(body);
    const cJSON *rows = resp ? cJSON_GetObjectItemCaseSensitive(resp, "rows") : NULL;
    if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) != n) {
        logf_(c, st == 0 ? "chunk fetch: no answer from the server" : "chunk fetch failed (HTTP %d)", st);
        if (st == 0) c->dead = true;
        cJSON_Delete(resp);
        free(out);
        return NULL;
    }
    int i = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, rows)
    {
        const cJSON *leaf = cJSON_GetObjectItemCaseSensitive(row, "doc");
        const char *id    = cJSON_GetStringValue(cJSON_GetArrayItem(children, i++));
        const char *data  = leaf ? jstr(leaf, "data") : NULL;
        if (data == NULL && id && cJSON_IsObject(eden)) {
            /* Inline chunk kept in the note itself (an older plugin option). */
            const cJSON *e = cJSON_GetObjectItemCaseSensitive(eden, id);
            data           = e ? jstr(e, "data") : NULL;
        }
        if (data == NULL) {
            logf_(c, "chunk %s missing; try again later", id ? id : "?");
            cJSON_Delete(resp);
            free(out);
            return NULL;
        }
        size_t plen     = 0;
        const char *why = "";
        char *plain     = decode_piece(c, data, binary, &plen, &why);
        if (plain == NULL) {
            logf_(c, "chunk %s %s", id ? id : "?", why);
            cJSON_Delete(resp);
            free(out);
            return NULL;
        }
        bool ok = append(&out, &tot, plain, plen);
        free(plain);
        if (!ok) {
            logf_(c, "note too large");
            cJSON_Delete(resp);
            free(out);
            return NULL;
        }
    }
    cJSON_Delete(resp);
    if (len) *len = tot;
    return out;
}

static void conflict_copy(ctx_t *c, const char *vpath)
{
    char full[320], copy[340], vcopy[240];
    file_path(c, vpath, full, sizeof(full));
    size_t n = strlen(vpath);
    snprintf(vcopy, sizeof(vcopy), "%.*s (deck conflict).md", (int)(n - 3), vpath);
    file_path(c, vcopy, copy, sizeof(copy));
    size_t len   = 0;
    char *text   = read_file(full, &len);
    if (text && write_file(copy, text, len)) {
        logf_(c, "conflict: kept your copy as %s", vcopy);
        c->conflicts++;
    }
    free(text);
}

/* One remote document from _changes. */
static bool apply_remote(ctx_t *c, const cJSON *doc)
{
    const char *id   = jstr(doc, "_id");
    const char *rev  = jstr(doc, "_rev");
    const char *type = jstr(doc, "type");
    const char *path = jstr(doc, "path");
    if (id == NULL || rev == NULL || type == NULL) return true;
    if (strncmp(id, "h:", 2) == 0 || strncmp(id, "i:", 2) == 0 || strncmp(id, "ix:", 3) == 0 ||
        strncmp(id, "ps:", 3) == 0 || strncmp(id, "_design/", 8) == 0 || id[0] == '_') {
        return true;
    }
    if (strncmp(id, "f:", 2) == 0) {
        logf_(c, "obfuscated paths are not supported");
        return false;
    }
    if (strcmp(type, "plain") != 0 && strcmp(type, "newnote") != 0 && strcmp(type, "notes") != 0) return true;
    if (path == NULL) path = id;
    if (!is_md(path)) {
        c->skipped++; /* attachments and other files stay in Obsidian */
        return true;
    }
    if (strcmp(type, "plain") != 0) c->legacy++; /* inline or binary-encoded markdown: read fine, written back as plain */
    if (!safe_vault_path(path)) {
        logf_(c, "skipping odd path %s", path);
        return true;
    }

    cJSON *have      = cJSON_GetObjectItemCaseSensitive(c->files, path);
    const char *hrev = have ? jstr(have, "rev") : NULL;
    bool deleted     = jbool(doc, "deleted", false) || jbool(doc, "_deleted", false);
    char full[320];
    file_path(c, path, full, sizeof(full));
    time_t local_now = file_mtime(full);
    bool local_edit  = have && local_now != 0 && (double)local_now != jnum(have, "local", 0);

    if (deleted) {
        if (have) {
            if (local_edit) {
                conflict_copy(c, path);
            }
            unlink(full);
            cJSON_DeleteItemFromObjectCaseSensitive(c->files, path);
            c->deleted++;
            logf_(c, "removed %s", path);
        }
        return true;
    }
    if (hrev && strcmp(hrev, rev) == 0) return true; /* already have it */

    size_t len  = 0;
    char *text  = fetch_note(c, doc, &len);
    if (text == NULL) {
        /* Logged. Remember it so the next run tries again instead of the
         * change sequence moving past it for good. */
        if (!cJSON_HasObjectItem(c->retry, id)) cJSON_AddNumberToObject(c->retry, id, 1);
        return !c->dead;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(c->retry, id);
    if (local_edit) conflict_copy(c, path);
    ensure_dirs(c, path);
    if (!write_file(full, text, len)) {
        logf_(c, "could not write %s", path);
        free(text);
        return false;
    }
    free(text);
    index_set(c, path, rev, jnum(doc, "mtime", 0), file_mtime(full));
    c->pulled++;
    if (c->pulled % 10 == 0) logf_(c, "pulled %d... (%u KB free)", c->pulled, heap_kb());
    return true;
}

/* Notes that failed last time: fetch each by id and try again. */
static bool retry_pending(ctx_t *c)
{
    int n = cJSON_GetArraySize(c->retry);
    if (n == 0) return true;
    logf_(c, "retrying %d note%s", n, n == 1 ? "" : "s");
    cJSON *e = c->retry->child;
    while (e) {
        cJSON *next = e->next;
        if (*c->cancel || c->dead) return false;
        char enc[520];
        couch_encode_id(e->string, enc, sizeof(enc));
        int st     = 0;
        cJSON *doc = couch_get(&c->db, enc, &st);
        if (doc && st == 200) {
            if (!apply_remote(c, doc)) {
                cJSON_Delete(doc);
                return false;
            }
        } else if (st == 404) {
            cJSON_DeleteItemFromObjectCaseSensitive(c->retry, e->string); /* gone meanwhile */
        }
        cJSON_Delete(doc);
        e = next;
    }
    index_save(c);
    return true;
}

static bool pull(ctx_t *c)
{
    if (!retry_pending(c)) return false;
    char seq[160] = "0";
    const char *s = jstr(c->index, "seq");
    if (s && !c->full) snprintf(seq, sizeof(seq), "%s", s);
    if (c->full) logf_(c, "rescanning the whole database");
    for (;;) {
        if (*c->cancel) return false;
        char path[300], enc[200];
        couch_encode_id(seq, enc, sizeof(enc));
        snprintf(path, sizeof(path), "_changes?since=%s&include_docs=true&filter=_selector&limit=%d", enc,
                 CHANGES_LIMIT);
        int st      = 0;
        cJSON *resp = couch_post(&c->db, path, "{\"selector\":{\"type\":{\"$ne\":\"leaf\"}}}", &st);
        const cJSON *results = resp ? cJSON_GetObjectItemCaseSensitive(resp, "results") : NULL;
        if (!cJSON_IsArray(results)) {
            const cJSON *reason = resp ? cJSON_GetObjectItemCaseSensitive(resp, "reason") : NULL;
            if (st == 200 && resp == NULL) {
                logf_(c, "change feed: reply unreadable or too large");
            } else if (cJSON_IsString(reason)) {
                logf_(c, "change feed failed (HTTP %d): %.60s", st, reason->valuestring);
            } else {
                logf_(c, "change feed failed (HTTP %d)", st);
            }
            cJSON_Delete(resp);
            return false;
        }
        bool ok = true;
        int n   = 0;
        const cJSON *r;
        cJSON_ArrayForEach(r, results)
        {
            n++;
            const cJSON *doc = cJSON_GetObjectItemCaseSensitive(r, "doc");
            if (cJSON_IsObject(doc) && !apply_remote(c, doc)) {
                ok = false;
                break;
            }
        }
        const cJSON *last = cJSON_GetObjectItemCaseSensitive(resp, "last_seq");
        if (ok && cJSON_IsString(last)) {
            snprintf(seq, sizeof(seq), "%s", last->valuestring);
        } else if (ok && cJSON_IsNumber(last)) {
            snprintf(seq, sizeof(seq), "%.0f", last->valuedouble);
        }
        cJSON_Delete(resp);
        if (!ok) return false;
        cJSON_DeleteItemFromObjectCaseSensitive(c->index, "seq");
        cJSON_AddStringToObject(c->index, "seq", seq);
        index_save(c);
        if (n < CHANGES_LIMIT) return true;
    }
}

/* ---- Push ---------------------------------------------------------------- */

/* Split text at newlines into pieces of at most PIECE_MAX bytes, never
 * inside a UTF-8 sequence. Returns the number of pieces; `starts`/`lens`
 * hold up to `max`. */
static int split_pieces(const char *text, size_t len, size_t *starts, size_t *lens, int max)
{
    int n = 0;
    size_t pos = 0;
    while (pos < len && n < max) {
        size_t end = pos + PIECE_MAX;
        if (end >= len) {
            end = len;
        } else {
            size_t cut = end;
            while (cut > pos + PIECE_MAX / 2 && text[cut - 1] != '\n') cut--;
            if (cut > pos + PIECE_MAX / 2) {
                end = cut;
            } else {
                while (end > pos && ((unsigned char)text[end] & 0xC0) == 0x80) end--; /* not mid-character */
            }
        }
        starts[n] = pos;
        lens[n]   = end - pos;
        n++;
        pos = end;
    }
    return n;
}

static bool push_note(ctx_t *c, const char *vpath)
{
    char full[320];
    file_path(c, vpath, full, sizeof(full));
    size_t len = 0;
    char *text = read_file(full, &len);
    if (text == NULL) {
        logf_(c, "could not read %s", vpath);
        return true;
    }
    time_t local = file_mtime(full);

    size_t starts[128], lens[128];
    int n = split_pieces(text, len, starts, lens, 128);
    cJSON *docs     = cJSON_CreateArray();
    cJSON *children = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        char id[24];
        ls_chunk_id(text + starts[i], lens[i], c->encrypt ? c->hashed_pass : NULL, id);
        cJSON_AddItemToArray(children, cJSON_CreateString(id));
        cJSON *leaf = cJSON_CreateObject();
        cJSON_AddStringToObject(leaf, "_id", id);
        cJSON_AddStringToObject(leaf, "type", "leaf");
        if (c->encrypt) {
            char *enc = ls_encrypt(&c->master, text + starts[i], lens[i]);
            if (enc == NULL) {
                cJSON_Delete(leaf);
                cJSON_Delete(docs);
                cJSON_Delete(children);
                free(text);
                logf_(c, "encryption failed");
                return false;
            }
            cJSON_AddStringToObject(leaf, "data", enc);
            cJSON_AddBoolToObject(leaf, "e_", true);
            free(enc);
        } else {
            char save        = text[starts[i] + lens[i]];
            text[starts[i] + lens[i]] = '\0';
            cJSON_AddStringToObject(leaf, "data", text + starts[i]);
            text[starts[i] + lens[i]] = save;
        }
        cJSON_AddItemToArray(docs, leaf);
    }
    free(text);

    /* Chunks first (duplicates are fine: same content, same id), then the note. */
    if (n) {
        cJSON *bulk = cJSON_CreateObject();
        cJSON_AddItemToObject(bulk, "docs", docs);
        char *body = cJSON_PrintUnformatted(bulk);
        cJSON_Delete(bulk);
        int st      = 0;
        cJSON *resp = body ? couch_post(&c->db, "_bulk_docs", body, &st) : NULL;
        free(body);
        cJSON_Delete(resp);
        if (st != 201 && st != 200) {
            logf_(c, "chunk upload failed (HTTP %d)", st);
            cJSON_Delete(children);
            return false;
        }
    } else {
        cJSON_Delete(docs);
    }

    char id[260], enc_id[520];
    path_to_id(c, vpath, id, sizeof(id));
    couch_encode_id(id, enc_id, sizeof(enc_id));
    for (int attempt = 0; attempt < 2; attempt++) {
        int st       = 0;
        cJSON *cur   = couch_get(&c->db, enc_id, &st);
        const char *rev = (cur && st == 200) ? jstr(cur, "_rev") : NULL;
        double ctime = (cur && st == 200) ? jnum(cur, "ctime", 0) : 0;
        double mtime = (double)local * 1000.0;
        if (ctime <= 0) ctime = mtime;
        cJSON *doc = cJSON_CreateObject();
        cJSON_AddStringToObject(doc, "_id", id);
        if (rev) cJSON_AddStringToObject(doc, "_rev", rev);
        cJSON_AddStringToObject(doc, "path", vpath);
        cJSON_AddStringToObject(doc, "type", "plain");
        cJSON_AddItemToObject(doc, "children", cJSON_Duplicate(children, true));
        cJSON_AddNumberToObject(doc, "ctime", ctime);
        cJSON_AddNumberToObject(doc, "mtime", mtime);
        cJSON_AddNumberToObject(doc, "size", (double)len);
        cJSON_AddItemToObject(doc, "eden", cJSON_CreateObject());
        cJSON_Delete(cur);
        char *body  = cJSON_PrintUnformatted(doc);
        cJSON_Delete(doc);
        cJSON *resp = body ? couch_put(&c->db, enc_id, body, &st) : NULL;
        free(body);
        const char *newrev = resp ? jstr(resp, "rev") : NULL;
        if ((st == 201 || st == 200) && newrev) {
            index_set(c, vpath, newrev, mtime, local);
            cJSON_Delete(resp);
            cJSON_Delete(children);
            c->pushed++;
            logf_(c, "sent %s", vpath);
            return true;
        }
        cJSON_Delete(resp);
        if (st != 409) {
            logf_(c, "upload of %s failed (HTTP %d)", vpath, st);
            break;
        }
        /* 409: someone wrote in between; take their revision and try once more. */
    }
    cJSON_Delete(children);
    return false;
}

static bool push_delete(ctx_t *c, const char *vpath)
{
    char id[260], enc_id[520];
    path_to_id(c, vpath, id, sizeof(id));
    couch_encode_id(id, enc_id, sizeof(enc_id));
    int st     = 0;
    cJSON *cur = couch_get(&c->db, enc_id, &st);
    if (cur == NULL || st != 200) {
        cJSON_Delete(cur); /* already gone */
        return true;
    }
    if (jbool(cur, "deleted", false)) {
        cJSON_Delete(cur);
        return true;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(cur, "deleted");
    cJSON_AddBoolToObject(cur, "deleted", true);
    cJSON_DeleteItemFromObjectCaseSensitive(cur, "mtime");
    cJSON_AddNumberToObject(cur, "mtime", (double)time(NULL) * 1000.0);
    char *body  = cJSON_PrintUnformatted(cur);
    cJSON_Delete(cur);
    cJSON *resp = body ? couch_put(&c->db, enc_id, body, &st) : NULL;
    free(body);
    cJSON_Delete(resp);
    if (st == 201 || st == 200) {
        logf_(c, "deleted %s remotely", vpath);
        c->deleted++;
        return true;
    }
    logf_(c, "delete of %s failed (HTTP %d)", vpath, st);
    return false;
}

static bool walk(ctx_t *c, const char *rel, bool *changed)
{
    char dir[320];
    if (rel[0]) {
        snprintf(dir, sizeof(dir), "%s/%s", c->dir, rel);
    } else {
        snprintf(dir, sizeof(dir), "%s", c->dir);
    }
    DIR *d = opendir(dir);
    if (d == NULL) return true;
    struct dirent *e;
    bool ok = true;
    while (ok && (e = readdir(d)) != NULL) {
        if (*c->cancel) break;
        if (e->d_name[0] == '.') continue; /* .obsidian, .trash, our index */
        char vpath[240];
        size_t rl = strlen(rel), nl = strlen(e->d_name);
        if (rl + nl + 2 > sizeof(vpath)) continue; /* a path this long never came from Obsidian */
        memcpy(vpath, rel, rl);
        if (rl) vpath[rl++] = '/';
        memcpy(vpath + rl, e->d_name, nl + 1);
        char full[320];
        file_path(c, vpath, full, sizeof(full));
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            ok = walk(c, vpath, changed);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !is_md(e->d_name)) continue;
        size_t n = strlen(vpath);
        if (n > 4 && strcmp(vpath + n - 4, ".tmp") == 0) continue;
        const cJSON *have = cJSON_GetObjectItemCaseSensitive(c->files, vpath);
        if (have && (double)st.st_mtime == jnum(have, "local", 0)) continue; /* unchanged since we synced it */
        *changed = true;
        ok       = push_note(c, vpath);
        if (ok) index_save(c);
    }
    closedir(d);
    return ok;
}

static bool push(ctx_t *c)
{
    bool changed = false;
    if (!walk(c, "", &changed)) return false;
    /* Files we had and no longer have were deleted here. */
    cJSON *e = c->files->child;
    while (e) {
        cJSON *next = e->next;
        char full[320];
        file_path(c, e->string, full, sizeof(full));
        struct stat st;
        if (stat(full, &st) != 0) {
            if (!push_delete(c, e->string)) return false;
            cJSON_DeleteItemFromObjectCaseSensitive(c->files, e->string);
            index_save(c);
        }
        e = next;
    }
    return true;
}

/* ---- Entry --------------------------------------------------------------- */

static bool sync_run(const char *dir, sync_log_fn log, volatile bool *cancel, bool full)
{
    ctx_t *c = calloc(1, sizeof(*c));
    if (c == NULL) return false;
    c->log    = log;
    c->cancel = cancel;
    c->dir    = dir;
    c->full   = full;
    load_settings(c);
    mkdir(dir, 0755);
    logf_(c, "starting (%u KB free)", heap_kb());
    bool ok = preflight(c);
    if (ok) {
        index_load(c);
        ok = pull(c) && push(c);
        /* Our own writes come back through _changes; pulling again records
         * their sequence so they are not fetched a second time. */
        if (ok) ok = pull(c);
        index_save(c);
        net_http_idle(); /* release the connection before measuring */
        if (ok) {
            int left = cJSON_GetArraySize(c->retry);
            logf_(c, "done: %d pulled, %d sent, %d removed%s (%u KB free)", c->pulled, c->pushed, c->deleted,
                  c->conflicts ? ", conflicts kept" : "", heap_kb());
            if (left) logf_(c, "%d note%s still to fetch; sync again", left, left == 1 ? "" : "s");
            if (c->skipped) logf_(c, "%d non-markdown files left to Obsidian", c->skipped);
            if (c->legacy) logf_(c, "%d notes were in an older storage form", c->legacy);
        }
        cJSON_Delete(c->index);
    }
    memset(&c->master, 0, sizeof(c->master));
    memset(c->passphrase, 0, sizeof(c->passphrase));
    memset(c->db.pass, 0, sizeof(c->db.pass));
    free(c);
    return ok;
}

const deck_sync_backend_t g_sync_livesync = {
    .name       = "LIVESYNC",
    .configured = configured,
    .sync       = sync_run,
};
