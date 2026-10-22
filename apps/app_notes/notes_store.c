#include "notes_store.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "deck_hal.h"

static char s_root[96];
static char s_dir[96 + NOTES_SUBDIR_MAX];
static char s_subdir[NOTES_SUBDIR_MAX]; /* "" or "a/b", no leading slash */
static bool s_subdir_vault;             /* which source s_subdir belongs to */

/* Read once and kept: a settings read can fail when internal RAM is tight
 * (during a sync), and a failed read must not flip the browser to LOCAL. */
static int s_vault = -1;

bool notes_vault_mode(void)
{
    if (s_vault < 0) s_vault = hal_cfg_get_i32("notes_src", 0) != 0;
    return s_vault != 0;
}

void notes_set_vault_mode(bool vault)
{
    s_vault = vault;
    hal_cfg_set_i32("notes_src", vault);
    s_subdir[0] = '\0';
}

const char *notes_root(void)
{
    const char *root = hal_storage_root();
    if (root == NULL) return NULL;
    bool vault = notes_vault_mode();
    snprintf(s_root, sizeof(s_root), "%s/%s", root, vault ? "vault" : "notes");
    if (vault != s_subdir_vault) {
        s_subdir[0]    = '\0'; /* a different tree */
        s_subdir_vault = vault;
    }
    struct stat st;
    if (stat(s_root, &st) != 0) {
        mkdir(root, 0755); /* simulator: sim/sdcard may not exist yet */
        if (mkdir(s_root, 0755) != 0 && errno != EEXIST) return NULL;
    }
    return s_root;
}

const char *notes_dir(void)
{
    const char *root = notes_root();
    if (root == NULL) return NULL;
    if (s_subdir[0]) {
        snprintf(s_dir, sizeof(s_dir), "%s/%s", root, s_subdir);
    } else {
        snprintf(s_dir, sizeof(s_dir), "%s", root);
    }
    struct stat st;
    if (stat(s_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        s_subdir[0] = '\0'; /* the folder went away (deleted, other card) */
        snprintf(s_dir, sizeof(s_dir), "%s", root);
    }
    return s_dir;
}

const char *notes_subdir(void) { return s_subdir; }

bool notes_enter(const char *name)
{
    if (name == NULL || name[0] == '\0' || strchr(name, '/')) return false;
    char next[NOTES_SUBDIR_MAX];
    int n = s_subdir[0] ? snprintf(next, sizeof(next), "%s/%s", s_subdir, name)
                        : snprintf(next, sizeof(next), "%s", name);
    if (n < 0 || (size_t)n >= sizeof(next)) return false;
    snprintf(s_subdir, sizeof(s_subdir), "%s", next);
    return true;
}

bool notes_up(void)
{
    if (s_subdir[0] == '\0') return false;
    char *slash = strrchr(s_subdir, '/');
    if (slash) {
        *slash = '\0';
    } else {
        s_subdir[0] = '\0';
    }
    return true;
}

static bool path_for(const char *name, char *out, size_t n)
{
    const char *dir = notes_dir();
    if (dir == NULL) return false;
    return (size_t)snprintf(out, n, "%s/%s", dir, name) < n;
}

static bool is_md(const char *name)
{
    size_t n = strlen(name);
    return n > 3 && strcasecmp(name + n - 3, ".md") == 0;
}

static int by_mtime_desc(const void *a, const void *b)
{
    const note_entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1; /* folders first, by name */
    if (x->is_dir) return strcasecmp(x->name, y->name);
    if (x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
    return strcasecmp(x->name, y->name);
}

int notes_list(note_entry_t *out, int max)
{
    const char *dir = notes_dir();
    if (dir == NULL) return 0;
    DIR *d = opendir(dir);
    if (d == NULL) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= NOTES_NAME_MAX) continue; /* .obsidian, .trash too */
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        bool dir_entry = S_ISDIR(st.st_mode);
        if (!dir_entry && (!S_ISREG(st.st_mode) || !is_md(e->d_name))) continue;
        snprintf(out[n].name, NOTES_NAME_MAX, "%s", e->d_name);
        out[n].size   = dir_entry ? 0 : (long)st.st_size;
        out[n].mtime  = st.st_mtime;
        out[n].is_dir = dir_entry;
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(out[0]), by_mtime_desc);
    return n;
}

char *notes_read(const char *name, size_t *len)
{
    char path[256];
    if (!path_for(name, path, sizeof(path))) return NULL;
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > NOTES_FILE_MAX) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)size + 1);
    size_t got = buf ? fread(buf, 1, (size_t)size, f) : 0;
    fclose(f);
    if (buf == NULL) return NULL;
    buf[got] = '\0';
    if (len) *len = got;
    return buf;
}

bool notes_write(const char *name, const char *text, size_t len)
{
    char path[256], tmp[264];
    if (!path_for(name, path, sizeof(path))) return false;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) return false;
    bool ok = fwrite(text, 1, len, f) == len;
    ok      = (fclose(f) == 0) && ok;
    if (!ok) {
        unlink(tmp);
        return false;
    }
    unlink(path); /* FAT rename() will not replace an existing file */
    return rename(tmp, path) == 0;
}

bool notes_delete(const char *name)
{
    char path[256];
    return path_for(name, path, sizeof(path)) && unlink(path) == 0;
}

bool notes_rename(const char *from, const char *to)
{
    char a[256], b[256];
    if (!path_for(from, a, sizeof(a)) || !path_for(to, b, sizeof(b))) return false;
    if (notes_exists(to)) return false;
    return rename(a, b) == 0;
}

bool notes_exists(const char *name)
{
    char path[256];
    struct stat st;
    return path_for(name, path, sizeof(path)) && stat(path, &st) == 0;
}

bool notes_sanitize(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (const char *p = in; *p && k + 4 < n && k + 4 < NOTES_NAME_MAX; p++) {
        unsigned char ch = (unsigned char)*p;
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
            out[k++] = (char)ch;
        } else if (ch == ' ' && k > 0 && out[k - 1] != '-') {
            out[k++] = '-';
        }
    }
    while (k > 0 && (out[k - 1] == '-' || out[k - 1] == '.')) k--;
    out[k] = '\0';
    if (k == 0) return false;
    if (!is_md(out)) strcat(out, ".md");
    return true;
}

static const char s_readme[] =
    "# DECK//OS NOTES\n"
    "\n"
    "Markdown notes, stored as plain `.md` files. Pull the SD card and they open anywhere.\n"
    "\n"
    "## Keys\n"
    "\n"
    "| Keys | Action |\n"
    "|---|---|\n"
    "| `Ctrl+S` | save (also autosaves) |\n"
    "| `Ctrl+P` | flip between edit and preview |\n"
    "| `Ctrl+T` | split: editor and preview side by side |\n"
    "| `Esc` | save and close |\n"
    "\n"
    "## What renders\n"
    "\n"
    "Text can be **strong**, *emphasized*, ~~struck~~, `inline code` or a [link](https://github.com).\n"
    "\n"
    "> Quotes get a side bar.\n"
    "> The net is vast.\n"
    "\n"
    "- bullets\n"
    "  - nest\n"
    "    - deeper\n"
    "1. numbered\n"
    "2. lists\n"
    "\n"
    "- [x] task lists\n"
    "- [ ] still to do\n"
    "\n"
    "```c\n"
    "int main(void) {\n"
    "    return jack_in();\n"
    "}\n"
    "```\n"
    "\n"
    "---\n"
    "\n"
    "Delete this file whenever you like; it comes back only if the folder is empty.\n";

void notes_seed(void)
{
    note_entry_t e;
    if (notes_vault_mode()) return; /* the vault is Obsidian's */
    if (notes_dir() != NULL && notes_list(&e, 1) == 0) {
        notes_write("README.md", s_readme, sizeof(s_readme) - 1);
    }
}
