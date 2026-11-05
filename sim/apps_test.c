/* Host unit tests for the app model (deck_app.c) and the settings hint rule
 * (deck_hint.c). Run: sim/build/apps_test */
#include <stdio.h>
#include <string.h>

#include "deck_app.h"
#include "deck_hint.h"
#include "deck_home.h"

static int s_fail, s_count;

static void expect(bool cond, const char *what)
{
    s_count++;
    if (!cond) {
        printf("FAIL  %s\n", what);
        s_fail++;
    }
}

/* ---- A tiny settings store standing in for NVS -------------------------- */

static struct {
    char key[16];
    int32_t val;
} s_kv[32];
static int s_nkv;
static bool s_drop_writes; /* simulate a write that does not stick */

int32_t hal_cfg_get_i32(const char *key, int32_t def)
{
    for (int i = 0; i < s_nkv; i++) {
        if (strcmp(s_kv[i].key, key) == 0) return s_kv[i].val;
    }
    return def;
}

void hal_cfg_set_i32(const char *key, int32_t value)
{
    if (s_drop_writes) return;
    for (int i = 0; i < s_nkv; i++) {
        if (strcmp(s_kv[i].key, key) == 0) {
            s_kv[i].val = value;
            return;
        }
    }
    snprintf(s_kv[s_nkv].key, sizeof(s_kv[s_nkv].key), "%s", key);
    s_kv[s_nkv++].val = value;
}

/* ---- Stub apps ------------------------------------------------------------ */

static int s_enable_calls[3], s_disable_calls[3];

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    (void)parent;
    return true;
}
static void en_opt(deck_app_t *self) { (void)self; s_enable_calls[1]++; }
static void dis_opt(deck_app_t *self) { (void)self; s_disable_calls[1]++; }
static void en_opt2(deck_app_t *self) { (void)self; s_enable_calls[2]++; }
static void dis_opt2(deck_app_t *self) { (void)self; s_disable_calls[2]++; }

static deck_app_t core = {.name = "CORE", .id = "core", .hotkey = 0x10, .on_start = start};
static deck_app_t opt  = {.name = "OPT", .id = "opt", .hotkey = 0x11, .optional = true, .default_on = true,
                          .on_start = start, .on_enable = en_opt, .on_disable = dis_opt};
static deck_app_t opt2 = {.name = "OPT2", .id = "opt2", .hotkey = 0x12, .optional = true, .default_on = false,
                          .on_start = start, .on_enable = en_opt2, .on_disable = dis_opt2};
static deck_app_t bad_id = {.name = "BAD", .id = "waytoolongid", .optional = true, .on_start = start};

int main(void)
{
    /* deck_hint_slot: the lowest disabled slot that is not offline */
    expect(deck_hint_slot((bool[]){0, 0, 0}, (bool[]){0, 0, 0}, 3) == -1, "hint: none disabled");
    expect(deck_hint_slot((bool[]){0, 0, 1, 0, 1}, (bool[]){0, 0, 0, 0, 0}, 5) == 2, "hint: lowest disabled");
    expect(deck_hint_slot((bool[]){0, 0, 0, 0, 1}, (bool[]){0, 0, 0, 0, 0}, 5) == 4, "hint: after re-enable");
    expect(deck_hint_slot((bool[]){1, 0, 1}, (bool[]){1, 0, 0}, 3) == 2, "hint: offline skipped");
    expect(deck_hint_slot((bool[]){1}, (bool[]){1}, 1) == -1, "hint: only offline");

    deck_app_register(&core);
    deck_app_register(&opt);
    deck_app_register(&opt2);
    deck_app_register(&bad_id);
    expect(deck_app_count() == 3, "optional app with an id over 11 chars is rejected");

    /* defaults_without_keys: an upgrade with no app_* keys */
    expect(deck_app_enabled(&core), "defaults: core on");
    expect(deck_app_enabled(&opt), "defaults: default_on app on");
    expect(!deck_app_enabled(&opt2), "defaults: default_off app off");

    /* boot enables only enabled optional apps */
    deck_app_boot();
    expect(s_enable_calls[1] == 1 && s_enable_calls[2] == 0, "boot: on_enable for enabled apps only");

    /* set_enabled persists and calls the hook once */
    expect(deck_app_set_enabled(&opt, false), "disable returns true");
    expect(!deck_app_enabled(&opt) && s_disable_calls[1] == 1, "disable: state and hook");
    expect(hal_cfg_get_i32("app_opt", -1) == 0, "disable: key app_opt written");
    expect(deck_app_set_enabled(&opt, false) && s_disable_calls[1] == 1, "disable twice: hook not repeated");

    /* lookups */
    expect(deck_app_by_hotkey(0x11) == NULL, "hotkey of a disabled app is ignored");
    expect(deck_app_by_hotkey(0x10) == &core, "hotkey of a core app works");
    expect(deck_app_by_id("opt") == &opt && deck_app_by_id("nope") == NULL, "by id");

    /* set_enabled_reverts_when_write_lost */
    s_drop_writes = true;
    expect(!deck_app_set_enabled(&opt, true), "write lost: returns false");
    expect(!deck_app_enabled(&opt) && s_enable_calls[1] == 1, "write lost: state and hooks unchanged");
    s_drop_writes = false;
    expect(deck_app_set_enabled(&opt, true) && deck_app_enabled(&opt) && s_enable_calls[1] == 2, "re-enable");

    /* the APPS group holds every optional app, enabled or not, in order */
    expect(deck_home_apps() == NULL, "apps group: none before it is added");
    deck_home_add_app(&core);
    deck_group_t *g = deck_home_add_apps("APPS", "x");
    deck_app_set_enabled(&opt2, false);
    expect(g && deck_home_apps() == g, "apps group: returned and remembered");
    expect(g && g->count == 2 && g->apps[0] == &opt && g->apps[1] == &opt2, "apps group: optional apps in order");
    expect(deck_home_count() == 2 && deck_home_index(NULL, g) == 1, "apps group: home tile after core");

    /* core cannot be disabled */
    expect(!deck_app_set_enabled(&core, false) && deck_app_enabled(&core), "core stays on");

    if (s_fail) {
        printf("%d of %d checks failed\n", s_fail, s_count);
        return 1;
    }
    printf("all %d checks passed\n", s_count);
    return 0;
}
