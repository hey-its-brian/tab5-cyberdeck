/* deck_portal for the simulator: no real server, but the same states and a
 * few scripted events so the PORTAL screen can be exercised. */
#include "deck_portal.h"

#include <stdio.h>
#include <string.h>

#include "deck_net.h"
#include "lvgl.h"

static bool s_on;
static uint32_t s_started;
static int s_script;
static portal_event_t s_ev[32];
static uint32_t s_count;

static void event(bool error, const char *text)
{
    portal_event_t *e = &s_ev[s_count++ % 32];
    snprintf(e->text, sizeof(e->text), "%s", text);
    e->error = error;
}

bool portal_start(void)
{
    if (net_state() != NET_CONNECTED) return false;
    s_on      = true;
    s_started = lv_tick_get();
    s_script  = 0;
    event(false, "portal up at http://192.168.1.77");
    return true;
}

void portal_stop(void)
{
    if (!s_on) return;
    s_on = false;
    event(false, "portal down");
}

bool portal_running(void) { return s_on; }
const char *portal_pin(void) { return "271828"; }
const char *portal_host(void) { return "deck.local"; }
uint32_t portal_idle_s(void) { return s_on ? lv_tick_elaps(s_started) / 1000 : 0; }
bool portal_busy(void) { return false; }

int portal_events(portal_event_t *out, int max, uint32_t *cursor)
{
    int n = 0;
    while (*cursor < s_count && n < max) out[n++] = s_ev[(*cursor)++ % 32];
    return n;
}

bool portal_tick(void)
{
    /* Pretend a browser shows up and drops a few files. */
    static const char *script[] = {"login from browser", "received night-city-radio.mp3 (6144 KB)",
                                   "received samurai-demo.mp3 (4811 KB)", "received todo.md (2 KB)"};
    if (s_on && s_script < 4 && lv_tick_elaps(s_started) > 400u * (unsigned)(s_script + 1)) {
        event(false, script[s_script++]);
    }
    return false;
}
