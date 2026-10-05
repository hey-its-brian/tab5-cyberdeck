/*
 * Open-Meteo forecast and geocoding: URL building and JSON parsing.
 * Pure C + cJSON (no LVGL), unit tested on the host by sim/weather_test.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define WX_HOURS 24
#define WX_DAYS 7
#define WX_PLACES 6

typedef struct {
    int code;          /* WMO weather code */
    char weekday[4];   /* "MON" */
    float hi, lo;
    int pop;           /* max precipitation probability, % */
} wx_day_t;

typedef struct {
    bool valid;
    char observed[17];  /* "2026-10-05T16:30" local to the location */
    float temp, feels;
    int humidity;
    float wind;
    int wind_dir;       /* degrees, 0 = from north */
    float pressure;     /* hPa */
    int code;
    bool is_day;
    char sunrise[6], sunset[6]; /* "07:12" */
    char temp_unit[4];  /* "°F" or "°C" */
    char wind_unit[6];  /* "mph" or "km/h" */

    int hours;          /* valid entries below, from the current hour on */
    int hour_of_day[WX_HOURS];
    float hour_temp[WX_HOURS];
    int hour_pop[WX_HOURS];

    int days;
    wx_day_t day[WX_DAYS];
} wx_t;

typedef struct {
    char label[80];     /* "Raleigh, North Carolina, US" */
    char name[48];      /* "Raleigh" */
    double lat, lon;
} wx_place_t;

/* URLs (out must be at least 512 bytes). */
void wx_forecast_url(char *out, size_t n, double lat, double lon, bool metric);
void wx_geocode_url(char *out, size_t n, const char *query);

bool wx_parse(const char *json, wx_t *out);
int  wx_parse_places(const char *json, wx_place_t *out, int max);

/* WMO code to a short label and an icon glyph (deck_icons.h). */
const char *wx_code_text(int code);
const char *wx_code_icon(int code, bool day);

/* "N", "NE", ... for a wind direction in degrees. */
const char *wx_compass(int deg);
