/*
 * deck_sentinel on the Tab5: EXT5V, the E07 control lines, UART1 on the
 * M5-Bus and a reader task that feeds the parser.
 *
 * Order matters at start: TX_EN low and RX_EN high first (the radios sit on
 * the bus 3V3 rail and are already powered), then EXT5V for the C5 and GPS,
 * then the UART. The C5 says HELLO once at boot; if EXT5V was already on
 * (the board driver leaves it high) that HELLO is long gone, so the task
 * asks for one every few seconds until it gets an answer.
 */
#include "deck_sentinel.h"
#include "sentinel_proto.h"

#include <string.h>

#include "deck_hal.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "sentinel";

#define PIN_TX      GPIO_NUM_6  /* M5-Bus 16, to the C5's G2 */
#define PIN_RX      GPIO_NUM_7  /* M5-Bus 15, from the C5's G7 */
#define PIN_RX_EN   GPIO_NUM_16 /* M5-Bus 2,  E07 RX_EN, 10k pull-down on the board */
#define PIN_TX_EN   GPIO_NUM_17 /* M5-Bus 4,  E07 TX_EN, 10k pull-down on the board */
#define PIN_GDO0    GPIO_NUM_45 /* M5-Bus 8,  E07 GDO0 */
#define PIN_GDO2    GPIO_NUM_52 /* M5-Bus 10, E07 GDO2 */
#define PIN_NRF_IRQ GPIO_NUM_4  /* M5-Bus 20, nRF24 IRQ, active low */

#define UART_PORT   UART_NUM_1
#define UART_BAUD   115200
#define RX_BUF      2048 /* driver ring: a burst of SUB + NRF + 8 WAP fits twice over */
#define TX_BUF      256
#define READ_CHUNK  256
#define HELLO_MS    5000 /* re-ask for HELLO this often until one arrives */

static sentinel_state_t  s_state;
static sentinel_lines_t  s_lines;
static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_task;
static volatile bool     s_on;
static bool              s_uart;

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
        .baud_rate  = UART_BAUD,
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

static void reader(void *arg)
{
    (void)arg;
    static char chunk[READ_CHUNK];
    uint32_t    hello_at = 0;
    for (;;) {
        if (!s_on) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        int n = uart_read_bytes(UART_PORT, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t now = sentinel_now_ms();
        if (n > 0) sentinel_proto_feed(&s_lines, &s_state, chunk, (size_t)n, now);
        s_state.gdo0    = gpio_get_level(PIN_GDO0) != 0;
        s_state.gdo2    = gpio_get_level(PIN_GDO2) != 0;
        s_state.nrf_irq = gpio_get_level(PIN_NRF_IRQ) != 0;
        bool ask        = !s_state.hello && (hello_at == 0 || (uint32_t)(now - hello_at) >= HELLO_MS);
        xSemaphoreGive(s_lock);
        if (ask) {
            hello_at = now;
            sentinel_send("CMD,HELLO");
        }
    }
}

void sentinel_start(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (s_on) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sentinel_state_init(&s_state);
    s_lines.len = 0;
    xSemaphoreGive(s_lock);

    lines_init();
    front_end(true);
    hal_ext5v(true);
    if (!uart_init()) {
        front_end(false);
        return;
    }
    uart_flush_input(UART_PORT);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.powered = true;
    xSemaphoreGive(s_lock);
    s_on = true;
    if (!s_task && xTaskCreatePinnedToCore(reader, "sentinel", 4096, NULL, 4, &s_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "reader task not started");
        s_task = NULL;
    }
    ESP_LOGI(TAG, "board on: EXT5V, RX_EN high, UART1 %d baud", UART_BAUD);
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

bool sentinel_send(const char *payload)
{
    if (!s_on || !s_uart) return false;
    char frame[160];
    int  n = sentinel_proto_frame(frame, sizeof(frame), payload);
    return n > 0 && uart_write_bytes(UART_PORT, frame, (size_t)n) == n;
}
