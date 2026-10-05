#include "weather_data.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "deck_icons.h"

void wx_forecast_url(char *out, size_t n, double lat, double lon, bool metric)
{
    snprintf(out, n,
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,"
             "wind_speed_10m,wind_direction_10m,pressure_msl"
             "&hourly=temperature_2m,precipitation_probability"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,"
             "sunrise,sunset&timezone=auto&forecast_days=7%s",
             lat, lon, metric ? "" : "&temperature_unit=fahrenheit&wind_speed_unit=mph");
}

void wx_geocode_url(char *out, size_t n, const char *query)
{
    /* Percent-encode the query. */
    char q[160];
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)query; *p && k + 4 < sizeof(q); p++) {
        if (isalnum(*p) || *p == '-' || *p == '.' || *p == '_') {
            q[k++] = (char)*p;
        } else {
            k += (size_t)snprintf(q + k, sizeof(q) - k, "%%%02X", *p);
        }
    }
    q[k] = '\0';
    snprintf(out, n, "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=%d&language=en&format=json", q,
             WX_PLACES);
}

/* ---- Parsing ------------------------------------------------------------- */

static double num(const cJSON *obj, const char *key, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static const char *str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static double arr_num(const cJSON *arr, int i, double def)
{
    const cJSON *v = cJSON_GetArrayItem(arr, i);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static const char *arr_str(const cJSON *arr, int i)
{
    const cJSON *v = cJSON_GetArrayItem(arr, i);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static void weekday_of(const char *date, char out[4])
{
    static const char *names[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    struct tm t = {0};
    if (sscanf(date, "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday) != 3) {
        strcpy(out, "---");
        return;
    }
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    t.tm_hour = 12; /* avoid DST edges */
    mktime(&t);
    strcpy(out, names[t.tm_wday % 7]);
}

bool wx_parse(const char *json, wx_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return false;

    const cJSON *cur   = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *units = cJSON_GetObjectItemCaseSensitive(root, "current_units");
    const cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    const cJSON *daily  = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (!cJSON_IsObject(cur) || !cJSON_IsObject(daily)) {
        cJSON_Delete(root);
        return false;
    }

    snprintf(out->observed, sizeof(out->observed), "%s", str(cur, "time"));
    out->temp     = (float)num(cur, "temperature_2m", 0);
    out->feels    = (float)num(cur, "apparent_temperature", out->temp);
    out->humidity = (int)num(cur, "relative_humidity_2m", 0);
    out->wind     = (float)num(cur, "wind_speed_10m", 0);
    out->wind_dir = (int)num(cur, "wind_direction_10m", 0);
    out->pressure = (float)num(cur, "pressure_msl", 0);
    out->code     = (int)num(cur, "weather_code", 0);
    out->is_day   = num(cur, "is_day", 1) != 0;
    snprintf(out->temp_unit, sizeof(out->temp_unit), "%s", str(units, "temperature_2m"));
    const char *wu = str(units, "wind_speed_10m");
    snprintf(out->wind_unit, sizeof(out->wind_unit), "%s", strcmp(wu, "mp/h") == 0 ? "mph" : wu);

    /* Hourly series start at local midnight; begin at the observed hour. */
    if (cJSON_IsObject(hourly)) {
        const cJSON *ht = cJSON_GetObjectItemCaseSensitive(hourly, "time");
        const cJSON *tt = cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m");
        const cJSON *pp = cJSON_GetObjectItemCaseSensitive(hourly, "precipitation_probability");
        int n = cJSON_GetArraySize(ht), start = 0;
        for (int i = 0; i < n; i++) {
            if (strncmp(arr_str(ht, i), out->observed, 13) == 0) { /* same "YYYY-MM-DDTHH" */
                start = i;
                break;
            }
        }
        for (int i = start; i < n && out->hours < WX_HOURS; i++) {
            int h                          = 0;
            sscanf(arr_str(ht, i) + 11, "%d", &h);
            out->hour_of_day[out->hours]   = h;
            out->hour_temp[out->hours]     = (float)arr_num(tt, i, 0);
            out->hour_pop[out->hours]      = (int)arr_num(pp, i, 0);
            out->hours++;
        }
    }

    const cJSON *dt = cJSON_GetObjectItemCaseSensitive(daily, "time");
    const cJSON *dc = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");
    const cJSON *dh = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
    const cJSON *dl = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
    const cJSON *dp = cJSON_GetObjectItemCaseSensitive(daily, "precipitation_probability_max");
    int nd = cJSON_GetArraySize(dt);
    for (int i = 0; i < nd && i < WX_DAYS; i++) {
        wx_day_t *d = &out->day[i];
        weekday_of(arr_str(dt, i), d->weekday);
        d->code = (int)arr_num(dc, i, 0);
        d->hi   = (float)arr_num(dh, i, 0);
        d->lo   = (float)arr_num(dl, i, 0);
        d->pop  = (int)arr_num(dp, i, 0);
        out->days++;
    }
    const char *sr = arr_str(cJSON_GetObjectItemCaseSensitive(daily, "sunrise"), 0);
    const char *ss = arr_str(cJSON_GetObjectItemCaseSensitive(daily, "sunset"), 0);
    if (strlen(sr) >= 16) snprintf(out->sunrise, sizeof(out->sunrise), "%.5s", sr + 11);
    if (strlen(ss) >= 16) snprintf(out->sunset, sizeof(out->sunset), "%.5s", ss + 11);

    out->valid = out->days > 0;
    cJSON_Delete(root);
    return out->valid;
}

int wx_parse_places(const char *json, wx_place_t *out, int max)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return 0;
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(root, "results");
    int n            = 0;
    const cJSON *r;
    cJSON_ArrayForEach(r, res)
    {
        if (n >= max) break;
        wx_place_t *p = &out[n++];
        const char *admin = str(r, "admin1");
        snprintf(p->name, sizeof(p->name), "%s", str(r, "name"));
        snprintf(p->label, sizeof(p->label), "%s%s%s, %s", p->name, admin[0] ? ", " : "", admin,
                 str(r, "country_code"));
        p->lat = num(r, "latitude", 0);
        p->lon = num(r, "longitude", 0);
    }
    cJSON_Delete(root);
    return n;
}

/* ---- WMO codes ----------------------------------------------------------- */

const char *wx_code_text(int c)
{
    switch (c) {
        case 0: return "CLEAR";
        case 1: return "MOSTLY CLEAR";
        case 2: return "PARTLY CLOUDY";
        case 3: return "OVERCAST";
        case 45: case 48: return "FOG";
        case 51: case 53: case 55: return "DRIZZLE";
        case 56: case 57: return "FREEZING DRIZZLE";
        case 61: return "LIGHT RAIN";
        case 63: return "RAIN";
        case 65: return "HEAVY RAIN";
        case 66: case 67: return "FREEZING RAIN";
        case 71: return "LIGHT SNOW";
        case 73: return "SNOW";
        case 75: return "HEAVY SNOW";
        case 77: return "SNOW GRAINS";
        case 80: case 81: return "SHOWERS";
        case 82: return "VIOLENT SHOWERS";
        case 85: case 86: return "SNOW SHOWERS";
        case 95: return "THUNDERSTORM";
        case 96: case 99: return "STORM + HAIL";
        default: return "UNKNOWN";
    }
}

const char *wx_code_icon(int c, bool day)
{
    if (c == 0 || c == 1) return day ? WX_SUNNY : WX_NIGHT;
    if (c == 2) return day ? WX_PARTLY : WX_NIGHT_PARTLY;
    if (c == 3) return WX_CLOUDY;
    if (c == 45 || c == 48) return WX_FOG;
    if (c >= 51 && c <= 63) return WX_RAIN;
    if (c == 65 || c == 81 || c == 82) return WX_POURING;
    if (c == 80) return WX_RAIN;
    if (c == 56 || c == 57 || c == 66 || c == 67) return WX_SLEET;
    if (c == 75 || c == 86) return WX_SNOW_HEAVY;
    if (c >= 71 && c <= 85) return WX_SNOW;
    if (c == 95) return WX_STORM;
    if (c == 96 || c == 99) return WX_HAIL;
    return WX_CLOUDY;
}

const char *wx_compass(int deg)
{
    static const char *dirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int i = (int)(((deg % 360) + 360 + 22) % 360 / 45);
    return dirs[i & 7];
}
