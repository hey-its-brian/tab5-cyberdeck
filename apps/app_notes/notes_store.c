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

static char s_dir[96];

const char *notes_dir(void)
{
    const char *root = hal_storage_root();
    if (root == NULL) return NULL;
    snprintf(s_dir, sizeof(s_dir), "%s/notes", root);
    struct stat st;
    if (stat(s_dir, &st) != 0) {
        mkdir(root, 0755); /* simulator: sim/sdcard may not exist yet */
        if (mkdir(s_dir, 0755) != 0 && errno != EEXIST) return NULL;
    }
    return s_dir;
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
        if (e->d_name[0] == '.' || !is_md(e->d_name) || strlen(e->d_name) >= NOTES_NAME_MAX) continue;
        char path[192];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        snprintf(out[n].name, NOTES_NAME_MAX, "%s", e->d_name);
        out[n].size  = (long)st.st_size;
        out[n].mtime = st.st_mtime;
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(out[0]), by_mtime_desc);
    return n;
}

char *notes_read(const char *name, size_t *len)
{
    char path[192];
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
    char path[192], tmp[200];
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
    char path[192];
    return path_for(name, path, sizeof(path)) && unlink(path) == 0;
}

bool notes_rename(const char *from, const char *to)
{
    char a[192], b[192];
    if (!path_for(from, a, sizeof(a)) || !path_for(to, b, sizeof(b))) return false;
    if (notes_exists(to)) return false;
    return rename(a, b) == 0;
}

bool notes_exists(const char *name)
{
    char path[192];
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
    if (notes_dir() != NULL && notes_list(&e, 1) == 0) {
        notes_write("README.md", s_readme, sizeof(s_readme) - 1);
    }
}
