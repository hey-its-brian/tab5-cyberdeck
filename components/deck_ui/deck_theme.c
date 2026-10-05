#include "deck_theme.h"

#include "deck_assets.h"
#include "deck_hal.h"

deck_palette_t g_pal;
deck_fonts_t g_font;

typedef struct {
    const char *name;
    uint32_t accent;
    uint32_t accent2;
} accent_def_t;

/* Each accent pairs with a contrasting secondary neon. */
static const accent_def_t s_accents[DECK_ACCENT_COUNT] = {
    {"NETRUNNER", 0x00F0FF, 0xFF2A6D},  /* cyan + magenta */
    {"ARASAKA",   0xFF2A6D, 0x00F0FF},  /* magenta + cyan */
    {"NOMAD",     0xF3E600, 0xFF2A6D},  /* acid yellow + magenta */
    {"MILITECH",  0x39FF14, 0xF3E600},  /* phosphor green + yellow */
};

static int s_accent;

static const lv_font_t *load_font(deck_asset_t id, int px)
{
    size_t size = 0;
    const uint8_t *data = deck_asset(id, &size);
    lv_font_t *f = data ? lv_tiny_ttf_create_data_ex(data, size, px, LV_FONT_KERNING_NONE, 96) : NULL;
    if (f == NULL) {
        LV_LOG_WARN("font %d @%dpx unavailable, using Montserrat", (int)id, px);
        return px >= 20 ? &lv_font_montserrat_20 : &lv_font_montserrat_14;
    }
    return f;
}

/* Only fonts we created can be modified; the built-in fallbacks are const. */
static void set_fallback(const lv_font_t *font, const lv_font_t *fallback)
{
    if (font != &lv_font_montserrat_20 && font != &lv_font_montserrat_14) {
        ((lv_font_t *)font)->fallback = fallback;
    }
}

void deck_theme_set_accent(int index)
{
    if (index < 0 || index >= DECK_ACCENT_COUNT) index = 0;
    s_accent = index;

    g_pal.bg       = lv_color_hex(0x07070D);
    g_pal.grid     = lv_color_hex(0x16182A);
    g_pal.panel    = lv_color_hex(0x0D0F1C);
    g_pal.panel_hi = lv_color_hex(0x171A30);
    g_pal.line     = lv_color_hex(0x2A3050);
    g_pal.text     = lv_color_hex(0xD8E1F0);
    g_pal.dim      = lv_color_hex(0x5F6884);
    g_pal.accent   = lv_color_hex(s_accents[index].accent);
    g_pal.accent2  = lv_color_hex(s_accents[index].accent2);
    g_pal.warn     = lv_color_hex(0xF3E600);
    g_pal.ok       = lv_color_hex(0x39FF14);
    g_pal.danger   = lv_color_hex(0xFF3B3B);

    /* Stock widgets (sliders, switches, rollers) pick up the accent through
     * LVGL's default theme. Needs fonts, so skip until deck_theme_init(). */
    lv_display_t *disp = lv_display_get_default();
    if (disp != NULL && g_font.mono_m != NULL) {
        lv_theme_t *th = lv_theme_default_init(disp, g_pal.accent, g_pal.accent2, true, g_font.mono_m);
        lv_display_set_theme(disp, th);
    }
}

int deck_theme_accent(void) { return s_accent; }

const char *deck_theme_accent_name(int index)
{
    return (index >= 0 && index < DECK_ACCENT_COUNT) ? s_accents[index].name : "?";
}

lv_color_t deck_theme_accent_color(int index)
{
    return lv_color_hex((index >= 0 && index < DECK_ACCENT_COUNT) ? s_accents[index].accent : 0xFFFFFF);
}

void deck_theme_init(void)
{
    deck_theme_set_accent((int)hal_cfg_get_i32("accent", 0));

    g_font.mono_s  = load_font(DECK_ASSET_FONT_MONO, 16);
    g_font.mono_m  = load_font(DECK_ASSET_FONT_MONO, 20);
    g_font.mono_l  = load_font(DECK_ASSET_FONT_MONO, 28);
    g_font.disp_s  = load_font(DECK_ASSET_FONT_DISPLAY, 18);
    g_font.disp_m  = load_font(DECK_ASSET_FONT_DISPLAY, 24);
    g_font.disp_l  = load_font(DECK_ASSET_FONT_DISPLAY, 36);
    g_font.disp_xl = load_font(DECK_ASSET_FONT_DISPLAY, 56);
    g_font.icon_s  = load_font(DECK_ASSET_FONT_ICONS, 24);
    g_font.icon_m  = load_font(DECK_ASSET_FONT_ICONS, 40);
    g_font.icon_l  = load_font(DECK_ASSET_FONT_ICONS, 96);

    /* Orbitron lacks some symbols (arrows, box drawing); let any missing
     * glyph fall back to the mono face. */
    set_fallback(g_font.disp_s, g_font.mono_s);
    set_fallback(g_font.disp_m, g_font.mono_m);
    set_fallback(g_font.disp_l, g_font.mono_l);
    set_fallback(g_font.disp_xl, g_font.mono_l);
    /* Icons are NOT a fallback for text fonts: tiny_ttf logs an error for
     * every glyph miss before falling back, which would spam the console on
     * each redraw. Use deck_icon_text() to put an icon next to text. */

    deck_theme_set_accent(s_accent); /* now that fonts exist, apply the LVGL theme */
}
