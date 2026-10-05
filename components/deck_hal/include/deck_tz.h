/* Timezone table shared by the device and simulator HALs. */
#pragma once

typedef struct {
    const char *name;
    const char *posix;
} deck_tz_t;

#define DECK_TZ_DEFAULT 1 /* US EASTERN */

static const deck_tz_t g_deck_tz[] = {
    {"UTC", "UTC0"},
    {"US EASTERN", "EST5EDT,M3.2.0,M11.1.0"},
    {"US CENTRAL", "CST6CDT,M3.2.0,M11.1.0"},
    {"US MOUNTAIN", "MST7MDT,M3.2.0,M11.1.0"},
    {"ARIZONA", "MST7"},
    {"US PACIFIC", "PST8PDT,M3.2.0,M11.1.0"},
    {"ALASKA", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"HAWAII", "HST10"},
    {"UK / IRELAND", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"CENTRAL EUROPE", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"EASTERN EUROPE", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"INDIA", "IST-5:30"},
    {"CHINA / SINGAPORE", "CST-8"},
    {"JAPAN / KOREA", "JST-9"},
    {"SYDNEY", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"AUCKLAND", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"SAO PAULO", "<-03>3"},
};

#define DECK_TZ_COUNT ((int)(sizeof(g_deck_tz) / sizeof(g_deck_tz[0])))
