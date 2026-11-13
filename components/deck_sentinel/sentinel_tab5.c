/*
 * deck_sentinel on the Tab5: EXT5V, the E07 control lines, UART1 on the
 * M5-Bus, a reader task that feeds the parser and runs the link machine,
 * and a writer task that puts events, captured frames and a wardrive's rows
 * on storage.
 *
 * Two tasks because an SD card can stall for a tenth of a second while it
 * allocates clusters, and at 921600 baud that is nine kilobytes of UART
 * arriving. The reader never touches a file: the parser's sink copies each
 * event and frame into a queue and the writer drains it.
 *
 * Order matters at start: TX_EN low and RX_EN high first (the radios sit on
 * the bus 3V3 rail and are already powered), then EXT5V for the C5 and GPS,
 * then the UART. The C5 says HELLO once at boot; if EXT5V was already on
 * (the board driver leaves it high) that HELLO is long gone, so the link
 * machine (sentinel_host in the core) asks for one every few seconds until
 * it gets an answer. The same machine moves the link to 921600 baud once
 * the node is known to speak protocol 2, and back if that does not hold.
 */
#include "deck_sentinel.h"
#include "sentinel_proto.h"

#include <stdio.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_hal.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sentinel_drivelog.h"
#include "sentinel_host.h"

static const char *TAG = "sentinel";

#define PIN_TX      GPIO_NUM_6  /* M5-Bus 16, to the C5's G2 */
#define PIN_RX      GPIO_NUM_7  /* M5-Bus 15, from the C5's G7 */
#define PIN_RX_EN   GPIO_NUM_16 /* M5-Bus 2,  E07 RX_EN, 10k pull-down on the board */
#define PIN_TX_EN   GPIO_NUM_17 /* M5-Bus 4,  E07 TX_EN, 10k pull-down on the board */
#define PIN_GDO0    GPIO_NUM_45 /* M5-Bus 8,  E07 GDO0 */
#define PIN_GDO2    GPIO_NUM_52 /* M5-Bus 10, E07 GDO2 */
#define PIN_NRF_IRQ GPIO_NUM_4  /* M5-Bus 20, nRF24 IRQ, active low */

#define UART_PORT   UART_NUM_1
#define RX_BUF      8192 /* driver ring: 90 ms of a saturated link at 921600 baud */
#define TX_BUF      256
#define READ_CHUNK  512
#define TICK_MS     100  /* how often the link machine runs */
/* Events and frames waiting for the writer. A screen transition can keep
 * the writer off the CPU for a few tenths of a second, and a Bluetooth
 * capture brings about 180 frames a second: 128 slots is most of a second. */
#define QUEUE_DEPTH 128

/* One thing for the writer: an event with where it happened, a captured
 * frame, a row of a wardrive, or the opening or closing of its files (the
 * writer does those too, so they stay in order with the rows). */
typedef enum { ITEM_EVENT, ITEM_PACKET, ITEM_DRIVE_ROW, ITEM_DRIVE_OPEN, ITEM_DRIVE_CLOSE } item_kind_t;
typedef struct {
    item_kind_t kind;
    union {
        struct {
            sentinel_event_t e;
            uint8_t          fix;
            double           lat, lon;
        } ev;
        sentinel_pkt_t pkt;
        struct {
            sentinel_drive_file_t file;
            uint16_t              len;
            char                  text[SENTINEL_SURVEY_LINE_MAX];
        } row;
        struct {
            char app[16], fw[12];
        } open;
    } u;
} store_item_t;

static sentinel_state_t  s_state;
static sentinel_lines_t  s_lines;
static sentinel_host_t   s_host;  /* under s_lock, like the state */
static SemaphoreHandle_t s_lock;
static QueueHandle_t     s_queue;
static TaskHandle_t      s_task, s_writer;
static volatile bool     s_on;
static bool              s_uart;
static uint32_t          s_q_drop_events, s_q_drop_packets; /* the queue was full */
static uint32_t          s_q_drop_rows, s_drive_lost_base;  /* the same for a wardrive's rows; losses before this drive */
static volatile bool     s_drive_wanted; /* a drive is meant to be running: what the writer holds its files open for */
/* The queue's storage: 53 KB, so in PSRAM (nothing here runs in an interrupt). */
static uint8_t       s_queue_mem[QUEUE_DEPTH * sizeof(store_item_t)] DECK_PSRAM_BSS;
static StaticQueue_t s_queue_ctl;
/* The Bluetooth devices in earshot and the track watch: under s_lock, like the state. */
static sentinel_blehost_t s_ble DECK_PSRAM_BSS;
/* And what the node has reported about Wi-Fi: access points, channels, the waterfall, drones. */
static sentinel_wifihost_t s_wifi DECK_PSRAM_BSS;
/* The two swept bands: history, statistics, bursts, the jam watch. And a second copy to write a site file from. */
static sentinel_bandhost_t s_band DECK_PSRAM_BSS, s_band_site DECK_PSRAM_BSS;

uint32_t sentinel_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void lines_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_RX_EN) | (1ULL << PIN_TX_EN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_set_level(PIN_TX_EN, 0); /* levels before direction: never a TX_EN glitch */
    gpio_set_level(PIN_RX_EN, 0);
    gpio_config(&out);

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_GDO0) | (1ULL << PIN_GDO2) | (1ULL << PIN_NRF_IRQ),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);
}

/* The CC1101 front end: RX_EN high, TX_EN low. Both low when off. */
static void front_end(bool receive)
{
    gpio_set_level(PIN_TX_EN, 0);
    gpio_set_level(PIN_RX_EN, receive ? 1 : 0);
}

static bool uart_init(void)
{
    if (s_uart) return true;
    uart_config_t cfg = {
        .baud_rate  = SENTINEL_BAUD_DEFAULT,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(UART_PORT, RX_BUF, TX_BUF, 0, NULL, 0);
    if (err == ESP_OK) err = uart_param_config(UART_PORT, &cfg);
    if (err == ESP_OK) err = uart_set_pin(UART_PORT, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    /* The driver's interrupt fires by default when 120 of the FIFO's 128 bytes
     * are full. At 921600 baud the last 8 fill in 87 microseconds: an
     * interrupt a little late loses bytes, and one lost stretch is two frames
     * with a bad checksum (seen on the bench: a pair every few minutes).
     * Fire at 32 instead, which leaves a millisecond. sdkconfig.defaults also
     * keeps the UART interrupt in RAM so a flash write cannot hold it off. */
    if (err == ESP_OK) err = uart_set_rx_full_threshold(UART_PORT, 32);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART1 on G6/G7 failed: %s", esp_err_to_name(err));
        uart_driver_delete(UART_PORT);
        return false;
    }
    s_uart = true;
    return true;
}

/* ---- Storage: the parser's sink fills a queue, the writer empties it ------ */

/* Both run in the reader task with s_lock held, so they only copy. */
static void sink_event(void *user, const sentinel_event_t *e)
{
    (void)user;
    static store_item_t it; /* static: 280 bytes the reader's stack need not carry */
    it.kind   = ITEM_EVENT;
    it.u.ev.e = *e;
    it.u.ev.fix  = s_state.fix;
    it.u.ev.lat  = s_state.lat;
    it.u.ev.lon  = s_state.lon;
    if (!s_queue || xQueueSend(s_queue, &it, 0) != pdTRUE) s_q_drop_events++;
}

static void sink_packet(void *user, const sentinel_pkt_t *p)
{
    (void)user;
    static store_item_t it;
    it.kind  = ITEM_PACKET;
    it.u.pkt = *p;
    if (!s_queue || xQueueSend(s_queue, &it, 0) != pdTRUE) s_q_drop_packets++;
}

static const sentinel_sink_t s_sink;

/* A row of a wardrive, on its way to the card. Reader task, s_lock held.
 * False when the queue has no room: the drive rules then take the row back
 * and write the device at its next sighting. */
static bool drive_row(sentinel_drive_file_t file, const char *row, int len)
{
    static store_item_t it;
    if (len <= 0 || (size_t)len > sizeof(it.u.row.text)) return false;
    it.kind       = ITEM_DRIVE_ROW;
    it.u.row.file = file;
    it.u.row.len  = (uint16_t)len;
    memcpy(it.u.row.text, row, (size_t)len);
    if (s_queue && xQueueSend(s_queue, &it, 0) == pdTRUE) return true;
    s_q_drop_rows++;
    return false;
}

/* A Bluetooth device row: the table, the track watch, and an event when the
 * watch fires (which comes straight back through sink_event). */
static void sink_device(void *user, const sentinel_bdv_t *d)
{
    (void)user;
    uint32_t now = sentinel_now_ms();
    sentinel_blehost_device(&s_ble, &s_state, d, now, &s_sink);
    sentinel_drivelog_device(&s_state, d, now, drive_row);
}

/* Wi-Fi rows, the same way. */
static void sink_channel(void *user, const sentinel_wch_t *c)
{
    (void)user;
    sentinel_wifihost_channel(&s_wifi, c);
}

static void sink_ap(void *user, const sentinel_wbs_t *a)
{
    (void)user;
    uint32_t now = sentinel_now_ms();
    sentinel_wifihost_ap(&s_wifi, a, now);
    sentinel_drivelog_ap(&s_state, a, now, drive_row);
}

static void sink_drone(void *user, const sentinel_drn_t *d)
{
    (void)user;
    sentinel_wifihost_drone(&s_wifi, &s_state, d, sentinel_now_ms(), &s_sink);
}

static void sink_burst(void *user, const sentinel_brs_t *b)
{
    (void)user;
    /* A row from the frequency just left can still be on the wire: it is not part of the new list. */
    if (b->khz == s_host.want_dwell_khz) sentinel_bursthost_add(&s_band.bursts, b, sentinel_now_ms());
}

static const sentinel_sink_t s_sink = {
    .event = sink_event, .packet = sink_packet, .device = sink_device, .channel = sink_channel, .ap = sink_ap, .drone = sink_drone,
    .burst = sink_burst,
};

/* The addresses the user marked as their own, kept across restarts. */
static void mine_load(void)
{
    uint8_t buf[SENTINEL_MINE_MAX * 6];
    size_t  len = sizeof(buf);
    s_ble.watch.mine_n = 0;
    if (!hal_cfg_get_blob("rf_mine", buf, &len) || len % 6 || len > sizeof(buf)) return;
    memcpy(s_ble.watch.mine, buf, len);
    s_ble.watch.mine_n = (int)(len / 6);
}

/* Writer task: make the drive's two files and say how it went. */
static void drive_open(const char *app, const char *fw)
{
    sentinel_wigle_id_t id = {.app = app, .model = "Tab5", .release = fw, .board = "ESP32-P4", .brand = "M5Stack"};
    char                note[64];
    if (sentinel_store_drive_open(&id)) {
        sentinel_store_stats_t st;
        sentinel_store_stats(&st);
        snprintf(note, sizeof(note), "wardrive started: %s", st.drive_file);
        sentinel_note(false, note);
    } else {
        sentinel_note(true, "wardrive: no file on the SD card, nothing is written");
    }
}

static void writer(void *arg)
{
    (void)arg;
    static store_item_t it;
    for (;;) {
        sentinel_store_stats_t before;
        sentinel_store_stats(&before);
        if (xQueueReceive(s_queue, &it, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (it.kind) {
                case ITEM_EVENT: sentinel_store_event(&it.u.ev.e, it.u.ev.fix, it.u.ev.lat, it.u.ev.lon); break;
                case ITEM_PACKET: sentinel_store_packet(&it.u.pkt); break;
                case ITEM_DRIVE_ROW: sentinel_store_drive_row(it.u.row.file, it.u.row.text, it.u.row.len); break;
                case ITEM_DRIVE_OPEN: drive_open(it.u.open.app, it.u.open.fw); break;
                case ITEM_DRIVE_CLOSE: sentinel_store_drive_close(); break;
            }
        } else if (s_on) {
            sentinel_store_flush(); /* a quiet second: get the capture and the drive onto the card */
            /* The request to close a drive's files can be lost to a full queue. Nothing
             * is waiting now, so if no drive is wanted its files have no reason to be open. */
            if (!s_drive_wanted) sentinel_store_drive_close();
        } else {
            sentinel_store_close(); /* board off: the session's captures are complete */
            sentinel_store_drive_close();
        }
        /* The card stopped taking a drive's rows (pulled, full, failing): say so once. */
        sentinel_store_stats_t after;
        sentinel_store_stats(&after);
        if (before.drive_open && !after.drive_open && s_drive_wanted)
            sentinel_note(true, "wardrive: the SD card stopped taking rows");
    }
}

/* ---- The link ------------------------------------------------------------ */

static void reader(void *arg)
{
    (void)arg;
    static char                chunk[READ_CHUNK];
    static sentinel_host_out_t out;
    uint32_t                   ticked = 0;
    for (;;) {
        if (!s_on) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        int n = uart_read_bytes(UART_PORT, chunk, sizeof(chunk), pdMS_TO_TICKS(50));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t now = sentinel_now_ms();
        if (n > 0) sentinel_proto_feed(&s_lines, &s_state, chunk, (size_t)n, now, &s_sink);
        s_state.gdo0    = gpio_get_level(PIN_GDO0) != 0;
        s_state.gdo2    = gpio_get_level(PIN_GDO2) != 0;
        s_state.nrf_irq = gpio_get_level(PIN_NRF_IRQ) != 0;
        bool tick       = (uint32_t)(now - ticked) >= TICK_MS;
        if (tick) {
            ticked = now;
            sentinel_host_tick(&s_host, &s_state, now, &out);
            sentinel_drivelog_tick(&s_state, now, drive_row);
            sentinel_bandhost_tick(&s_band, &s_state, now, &s_sink); /* may raise the jam watch's warning */
            if (out.note[0]) sentinel_push_event(&s_state, out.note_warn, "deck", out.note, now, &s_sink);
            if (out.set_baud) s_lines.len = 0; /* half a line at the old rate is noise at the new one */
        }
        xSemaphoreGive(s_lock);
        if (!tick) continue;
        if (out.set_baud) {
            /* Order matters: what was sent at the old rate must leave first. */
            uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(200));
            uart_set_baudrate(UART_PORT, out.set_baud);
            uart_flush_input(UART_PORT);
            ESP_LOGI(TAG, "UART1 now %u baud", (unsigned)out.set_baud);
        }
        for (int i = 0; i < out.n_send; i++) sentinel_send(out.send[i]);
        if (out.note[0]) ESP_LOGI(TAG, "%s", out.note);
    }
}

void sentinel_start(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (s_on) return;
    if (!s_queue) s_queue = xQueueCreateStatic(QUEUE_DEPTH, sizeof(store_item_t), s_queue_mem, &s_queue_ctl);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sentinel_state_init(&s_state);
    s_lines.len = 0;
    /* "rf_baud" is an escape hatch: set it to 115200 to never negotiate. */
    sentinel_host_init(&s_host, (uint32_t)hal_cfg_get_i32("rf_baud", SENTINEL_BAUD_FAST));
    sentinel_blehost_init(&s_ble);
    sentinel_wifihost_init(&s_wifi);
    sentinel_bandhost_init(&s_band, sentinel_now_ms());
    mine_load();
    xSemaphoreGive(s_lock);

    lines_init();
    front_end(true);
    hal_ext5v(true);
    if (!uart_init()) {
        front_end(false);
        hal_ext5v(false); /* nothing will read the board: do not leave it powered */
        return;
    }
    uart_set_baudrate(UART_PORT, SENTINEL_BAUD_DEFAULT); /* a freshly powered node listens here */
    uart_flush_input(UART_PORT);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.powered = true;
    xSemaphoreGive(s_lock);
    s_on = true;
    if (!s_task && xTaskCreatePinnedToCore(reader, "sentinel", 6144, NULL, 4, &s_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "reader task not started");
        s_task = NULL;
    }
    if (s_queue && !s_writer && xTaskCreatePinnedToCore(writer, "sentinel_log", 8192, NULL, 3, &s_writer, 0) != pdPASS) {
        ESP_LOGE(TAG, "writer task not started: nothing will be logged");
        s_writer = NULL;
    }
    ESP_LOGI(TAG, "board on: EXT5V, RX_EN high, UART1 %d baud", SENTINEL_BAUD_DEFAULT);
}

void sentinel_stop(void)
{
    if (!s_on) return;
    sentinel_drive_stop(); /* a drive ends with the board */
    s_on = false;
    front_end(false);
    hal_ext5v(false);
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_state.powered = false;
        sentinel_host_arm(&s_host, false); /* the node lost power; nothing to hold */
        sentinel_arm_set(&s_state.arm, false, 0, 0);
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "board off: EXT5V cut, RX_EN low");
}

bool sentinel_running(void) { return s_on; }

void sentinel_snapshot(sentinel_state_t *out)
{
    if (!s_lock) {
        sentinel_state_init(out);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, &s_state, sizeof(*out));
    xSemaphoreGive(s_lock);
}

void sentinel_arm(bool on)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sentinel_host_arm(&s_host, on);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "arm lease %s", on ? "wanted" : "dropped");
}

void sentinel_note(bool warn, const char *text)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sentinel_push_event(&s_state, warn, "deck", text, sentinel_now_ms(), &s_sink);
    xSemaphoreGive(s_lock);
}

void sentinel_tool(const char *mode)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sentinel_host_mode(&s_host, mode);
    xSemaphoreGive(s_lock);
}

void sentinel_capture(bool on)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sentinel_host_capture(&s_host, on);
    xSemaphoreGive(s_lock);
}

void sentinel_wifi_copy(sentinel_wifihost_t *out)
{
    if (!s_lock) {
        sentinel_wifihost_init(out);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sentinel_wifihost_expire(&s_wifi, sentinel_now_ms());
    memcpy(out, &s_wifi, sizeof(*out));
    xSemaphoreGive(s_lock);
    sentinel_wifihost_sort(out); /* on the caller's copy, outside the lock */
}

void sentinel_honeypot(uint8_t channel, const char *ssid)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sentinel_host_honeypot(&s_host, channel, ssid);
    sentinel_host_mode(&s_host, "hpot");
    xSemaphoreGive(s_lock);
}

void sentinel_ble_table(sentinel_bletab_t *out)
{
    out->n = 0;
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sentinel_bletab_expire(&s_ble.tab, sentinel_now_ms());
    memcpy(out, &s_ble.tab, sizeof(*out));
    xSemaphoreGive(s_lock);
    sentinel_bletab_sort(out); /* on the caller's copy, outside the lock */
}

bool sentinel_ble_track(const uint8_t addr[6], sentinel_track_t *out)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const sentinel_track_t *t = sentinel_trackwatch_find(&s_ble.watch, addr);
    if (t) *out = *t;
    xSemaphoreGive(s_lock);
    return t != NULL;
}

bool sentinel_ble_mine(const uint8_t addr[6])
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool mine = sentinel_trackwatch_mine(&s_ble.watch, addr);
    xSemaphoreGive(s_lock);
    return mine;
}

void sentinel_ble_set_mine(const uint8_t addr[6], bool mine)
{
    if (!s_lock) return;
    uint8_t buf[SENTINEL_MINE_MAX * 6];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sentinel_trackwatch_set_mine(&s_ble.watch, addr, mine);
    size_t len = (size_t)s_ble.watch.mine_n * 6;
    memcpy(buf, s_ble.watch.mine, len);
    xSemaphoreGive(s_lock);
    hal_cfg_set_blob("rf_mine", len ? buf : NULL, len); /* settings writes can be slow: not under the lock */
}

void sentinel_stats(sentinel_store_stats_t *out)
{
    sentinel_store_stats(out);
    out->events_dropped += s_q_drop_events;
    out->packets_dropped += s_q_drop_packets;
    out->drive_dropped += s_q_drop_rows;
}

/* ---- The two swept bands -------------------------------------------------- */

void sentinel_band_copy(sentinel_bandhost_t *out)
{
    if (!s_lock) {
        sentinel_bandhost_init(out, 0);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, &s_band, sizeof(*out));
    xSemaphoreGive(s_lock);
}

void sentinel_selftest(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_state.tst, 0, sizeof(s_state.tst));
    s_state.tst_n = s_state.tst_passed = s_state.tst_failed = 0;
    s_state.tst_done = false;
    sentinel_host_selftest(&s_host);
    xSemaphoreGive(s_lock);
}

void sentinel_stations(const uint8_t bssid[6], uint8_t channel)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.sta_n = 0; /* a different access point is a different list */
    sentinel_host_stations(&s_host, bssid, channel);
    xSemaphoreGive(s_lock);
}

bool sentinel_deauth(const sentinel_deauth_t *t, const char **why)
{
    if (!s_lock) {
        if (why) *why = "the radio board is not running";
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.txd_sent = s_state.txd_asked = 0;
    s_state.txd_ms   = 0;
    bool ok = sentinel_host_deauth(&s_host, t, why);
    xSemaphoreGive(s_lock);
    return ok;
}

void sentinel_band_learn(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sentinel_host_learn(&s_host);
    xSemaphoreGive(s_lock);
}

void sentinel_band_listen(uint32_t khz)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t was = s_host.want_dwell_khz;
    sentinel_host_sub(&s_host, khz);
    if (s_host.want_dwell_khz && s_host.want_dwell_khz != was) sentinel_bursthost_init(&s_band.bursts); /* a new frequency, a new list */
    xSemaphoreGive(s_lock);
}

void sentinel_site_begin(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sentinel_bandhost_stats_reset(&s_band, sentinel_now_ms());
    xSemaphoreGive(s_lock);
}

/* LVGL task. The copy is taken under the lock; the file is written outside it. */
bool sentinel_site_write(const char *tag, char *name, size_t n)
{
    static sentinel_state_t st;
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(&s_band_site, &s_band, sizeof(s_band_site));
    memcpy(&st, &s_state, sizeof(st));
    xSemaphoreGive(s_lock);
    if (!sentinel_store_site_from(&s_band_site, &st, tag, sentinel_now_ms(), name, n)) return false;
    char note[64];
    snprintf(note, sizeof(note), "site survey: %.49s", name); /* the longest name, and an event's 63 characters */
    sentinel_note(false, note);
    return true;
}

/* ---- Wardriving ---------------------------------------------------------- */

/* Opening and closing go through the queue like the rows between them. From
 * the LVGL task, so it may wait a moment for room but never for the card. */
static bool drive_item(item_kind_t kind, const char *app, const char *fw)
{
    static store_item_t it;
    it.kind = kind;
    snprintf(it.u.open.app, sizeof(it.u.open.app), "%s", app ? app : "");
    snprintf(it.u.open.fw, sizeof(it.u.open.fw), "%s", fw ? fw : "");
    if (s_queue && xQueueSend(s_queue, &it, pdMS_TO_TICKS(500)) == pdTRUE) return true;
    ESP_LOGW(TAG, "drive: the writer's queue is full");
    return false;
}

void sentinel_drive_start(const char *app_version)
{
    if (!s_lock || !s_on) return;
    char                   fw[sizeof(s_state.fw)];
    sentinel_store_stats_t st;
    sentinel_stats(&st);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (sentinel_drivelog_on()) {
        xSemaphoreGive(s_lock);
        return;
    }
    memcpy(fw, s_state.fw, sizeof(fw));
    xSemaphoreGive(s_lock);
    /* The files first: rows queue up behind this. If even that cannot be
     * queued the card is not keeping up, and a drive with no files is no drive. */
    s_drive_wanted = true;
    if (!drive_item(ITEM_DRIVE_OPEN, app_version, fw)) {
        s_drive_wanted = false;
        sentinel_note(true, "wardrive not started: storage is busy, try again");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_drive_lost_base = st.drive_dropped;
    sentinel_host_mode(&s_host, "drive");
    sentinel_drivelog_begin(sentinel_now_ms());
    xSemaphoreGive(s_lock);
}

void sentinel_drive_stop(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was = sentinel_drivelog_on();
    sentinel_drivelog_end();
    if (was) sentinel_host_mode(&s_host, "watch");
    xSemaphoreGive(s_lock);
    if (!was) return;
    s_drive_wanted = false;
    drive_item(ITEM_DRIVE_CLOSE, NULL, NULL); /* if this is lost the writer closes the files in its next quiet second */
    sentinel_drive_info_t info;
    char                  note[64];
    sentinel_drive_info(&info);
    snprintf(note, sizeof(note), "wardrive ended: %u rows, %u wifi, %u ble", (unsigned)info.rows, (unsigned)info.wifi,
             (unsigned)info.ble);
    sentinel_note(false, note);
}

void sentinel_drive_info(sentinel_drive_info_t *out)
{
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sentinel_drivelog_info(out);
    xSemaphoreGive(s_lock);
    sentinel_store_stats_t st;
    sentinel_stats(&st);
    out->writing = st.drive_open;
    out->lost    = st.drive_dropped - s_drive_lost_base;
    memcpy(out->file, st.drive_file, sizeof(out->file));
}

bool sentinel_send(const char *payload)
{
    if (!s_on || !s_uart) return false;
    char frame[160];
    int  n = sentinel_proto_frame(frame, sizeof(frame), payload);
    return n > 0 && uart_write_bytes(UART_PORT, frame, (size_t)n) == n;
}
