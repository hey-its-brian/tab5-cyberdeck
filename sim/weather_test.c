/* Host tests for apps/app_weather/weather_data.c against captured responses. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weather_data.h"

static int s_fail, s_count;

#define CHECK(cond, ...)                \
    do {                                \
        s_count++;                      \
        if (!(cond)) {                  \
            printf("FAIL  " __VA_ARGS__); \
            printf("\n");               \
            s_fail++;                   \
        }                               \
    } while (0)

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    b[fread(b, 1, (size_t)n, f)] = '\0';
    fclose(f);
    return b;
}

int main(void)
{
    char *fc = slurp(FIXTURES "/forecast.json");
    char *gc = slurp(FIXTURES "/geocode.json");
    CHECK(fc && gc, "fixtures missing");
    if (!fc || !gc) return 1;

    static wx_t w;
    CHECK(wx_parse(fc, &w), "parse forecast");
    CHECK(w.days == 7, "7 days, got %d", w.days);
    CHECK(w.hours == WX_HOURS, "24 hours, got %d", w.hours);
    CHECK(strcmp(w.temp_unit, "\xC2\xB0" "F") == 0, "unit degF, got %s", w.temp_unit);
    CHECK(strcmp(w.wind_unit, "mph") == 0, "wind unit mph, got %s", w.wind_unit);
    CHECK(w.hour_of_day[0] == atoi(w.observed + 11), "hourly starts at observed hour (%d vs %s)", w.hour_of_day[0],
          w.observed);
    CHECK(strlen(w.sunrise) == 5 && w.sunrise[2] == ':', "sunrise HH:MM, got %s", w.sunrise);
    CHECK(w.day[0].hi >= w.day[0].lo, "hi >= lo");
    CHECK(strlen(w.day[0].weekday) == 3, "weekday name");
    CHECK(w.humidity > 0 && w.humidity <= 100, "humidity %d", w.humidity);

    wx_place_t p[WX_PLACES];
    int n = wx_parse_places(gc, p, WX_PLACES);
    CHECK(n >= 1, "places parsed: %d", n);
    CHECK(n >= 1 && strstr(p[0].label, "Raleigh, North Carolina, US") != NULL, "label: %s", n ? p[0].label : "");
    CHECK(n >= 1 && fabs(p[0].lat - 35.78) < 0.1, "lat %f", n ? p[0].lat : 0);

    CHECK(!wx_parse("not json", &w), "rejects garbage");
    CHECK(!wx_parse("{}", &w), "rejects empty object");
    CHECK(wx_parse_places("{\"generationtime_ms\":0.5}", p, WX_PLACES) == 0, "no results -> 0");

    char url[512];
    wx_geocode_url(url, sizeof(url), "San Jose");
    CHECK(strstr(url, "name=San%20Jose&") != NULL, "geocode url encodes space: %s", url);
    wx_forecast_url(url, sizeof(url), 35.78, -78.64, false);
    CHECK(strstr(url, "temperature_unit=fahrenheit") != NULL, "imperial url");
    wx_forecast_url(url, sizeof(url), 35.78, -78.64, true);
    CHECK(strstr(url, "fahrenheit") == NULL, "metric url");

    CHECK(!strcmp(wx_compass(0), "N") && !strcmp(wx_compass(358), "N") && !strcmp(wx_compass(225), "SW"), "compass");
    CHECK(!strcmp(wx_code_text(95), "THUNDERSTORM"), "code text");

    printf("%d/%d passed\n", s_count - s_fail, s_count);
    free(fc);
    free(gc);
    return s_fail ? 1 : 0;
}
