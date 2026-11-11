/*
 * deck_sentinel on the Tab5: EXT5V, the E07 control lines, UART1 on the
 * M5-Bus, a reader task that feeds the parser and runs the link machine,
 * and a writer task that puts events and captured frames on storage.
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

/* One thing for the writer: an event with where it happened, or a frame. */
typedef struct {
    bool is_packet;
    union {
        struct {
            sentinel_event_t e;
            uint8_t          fix;
            double           lat, lon;
        } ev;
        sentinel_pkt_t pkt;
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
/* The queue's storage: 36 KB, so in PSRAM (nothing here runs in an interrupt). */
static uint8_t       s_queue_mem[QUEUE_DEPTH * sizeof(store_item_t)] DECK_PSRAM_BSS;
static StaticQueue_t s_queue_ctl;
/* The Bluetooth devices in earshot and the track watch: under s_lock, like the state. */
static sentinel_blehost_t s_ble DECK_PSRAM_BSS;

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
    it.is_packet = false;
    it.u.ev.e    = *e;
    it.u.ev.fix  = s_state.fix;
    it.u.ev.lat  = s_state.lat;
    it.u.ev.lon  = s_state.lon;
    if (!s_queue || xQueueSend(s_queue, &it, 0) != pdTRUE) s_q_drop_events++;
}

static void sink_packet(void *user, const sentinel_pkt_t *p)
{
    (void)user;
    static store_item_t it;
    it.is_packet = true;
    it.u.pkt     = *p;
    if (!s_queue || xQueueSend(s_queue, &it, 0) != pdTRUE) s_q_drop_packets++;
}

static const sentinel_sink_t s_sink;

/* A Bluetooth device row: the table, the track watch, and an event when the
 * watch fires (which comes straight back through sink_event). */
static void sink_device(void *user, const sentinel_bdv_t *d)
{
    (void)user;
    sentinel_blehost_device(&s_ble, &s_state, d, sentinel_now_ms(), &s_sink);
}

static const sentinel_sink_t s_sink = {sink_event, sink_packet, sink_device, NULL};

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

static void writer(void *arg)
{
    (void)arg;
    static store_item_t it;
    for (;;) {
        if (xQueueReceive(s_queue, &it, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (it.is_packet) sentinel_store_packet(&it.u.pkt);
            else sentinel_store_event(&it.u.ev.e, it.u.ev.fix, it.u.ev.lat, it.u.ev.lon);
        } else if (s_on) {
            sentinel_store_flush(); /* a quiet second: get the capture onto the card */
        } else {
            sentinel_store_close(); /* board off: the session's captures are complete */
        }
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
}

bool sentinel_send(const char *payload)
{
    if (!s_on || !s_uart) return false;
    char frame[160];
    int  n = sentinel_proto_frame(frame, sizeof(frame), payload);
    return n > 0 && uart_write_bytes(UART_PORT, frame, (size_t)n) == n;
}
