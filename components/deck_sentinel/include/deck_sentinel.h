/*
 * deck_sentinel: the RF Sentinel carrier board on the Tab5's rear M5-Bus.
 *
 * The board carries a Stamp C5 (ESP32-C5) that sweeps a CC1101 (433 MHz
 * band), an nRF24L01+ (2.4 GHz carrier detect), reads a GPS and scans Wi-Fi,
 * then streams what it saw as $RFS sentences over a UART. The Tab5 powers
 * the board (EXT5V on the bus), keeps the CC1101 front end in receive and
 * parses the stream into one state record that the RF WATCH app draws.
 *
 * Tab5 side of the bus (hardware/tab5-board/README.md in rf-sentinel):
 *   G6  UART TX to the C5        G7  UART RX from the C5     115200 8N1
 *   G16 E07 RX_EN (out, high)    G17 E07 TX_EN (out, low, never with G16)
 *   G45 E07 GDO0 (in)            G52 E07 GDO2 (in)           G4 nRF24 IRQ (in, low)
 *   5 V on bus pin 28 comes from EXT5V_EN on the PI4IOE5V6408 at 0x43, P2.
 *
 * sentinel_start() runs from RF WATCH's on_enable, sentinel_stop() from its
 * on_disable, both on the LVGL task. The reader task fills the state;
 * sentinel_snapshot() copies it out under the lock. The simulator provides
 * a fake node that speaks the same sentences (sim/sentinel_sim.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENTINEL_SUB_CH        21     /* 433.0 .. 435.0 MHz in 0.1 MHz steps */
#define SENTINEL_SUB_FIRST_MHZ 433.0f
#define SENTINEL_SUB_STEP_MHZ  0.1f
#define SENTINEL_SUB_FLOOR_DBM (-120) /* what an idle channel reports */
#define SENTINEL_NRF_CH        126    /* 2400 .. 2525 MHz in 1 MHz steps */
#define SENTINEL_NRF_FIRST_MHZ 2400
#define SENTINEL_NRF_MAX_HITS  8      /* carrier seen in n of 8 sweeps */
#define SENTINEL_AP_MAX        8      /* strongest networks the C5 reports */
#define SENTINEL_EVT_MAX       8      /* event ring */
#define SENTINEL_LINK_MS       4000   /* no frame for this long = link down */

typedef struct {
    uint32_t ms;     /* sentinel_now_ms() when it arrived */
    bool     warn;   /* "warn" rather than "info" */
    char     src[6]; /* gps, sub, nrf, wifi, node */
    char     text[64];
} sentinel_event_t;

typedef struct {
    char    bssid[13]; /* 12 hex digits */
    uint8_t ch;
    int8_t  rssi;
    char    ssid[33];
} sentinel_ap_t;

typedef struct {
    /* Link */
    bool     powered;   /* sentinel_start() has run: EXT5V on, RX_EN high */
    bool     hello;     /* a HELLO arrived since the last start */
    uint32_t frames;    /* valid frames parsed */
    uint32_t bad;       /* lines dropped: bad checksum or not an $RFS frame */
    uint32_t last_ms;   /* sentinel_now_ms() of the last valid frame */
    char     fw[12];    /* C5 firmware version from HELLO */
    char     proto[4];  /* protocol version from HELLO */
    bool     cc_ok, nrf_ok, lora_ok, gps_ok, wifi_ok;
    uint32_t uptime_s;  /* C5 uptime */
    uint32_t heap;      /* C5 free heap, bytes */

    /* GNSS */
    uint8_t fix;        /* 0 none, 2 = 2D, 3 = 3D (as the GPS reports it) */
    uint8_t sats_used, sats_view;
    float   hdop, alt_m, cn0_avg;
    uint8_t cn0_max;
    double  lat, lon;
    char    utc[12];    /* hhmmss.ss or "0" before the first fix */

    /* Sub-GHz: RSSI in dBm per channel, and a slowly decaying peak */
    int8_t   sub[SENTINEL_SUB_CH];
    int8_t   sub_peak[SENTINEL_SUB_CH];
    uint32_t sub_sweeps;

    /* 2.4 GHz: carrier hits out of 8 sweeps per channel, and a decaying peak */
    uint8_t  nrf[SENTINEL_NRF_CH];
    uint8_t  nrf_peak[SENTINEL_NRF_CH];
    uint32_t nrf_sweeps;

    /* LoRa (only with the optional SX1262) */
    int   lora_pkts;
    float lora_rssi, lora_snr;

    /* Wi-Fi as the C5 hears it */
    uint16_t      wifi_total, wifi_24, wifi_5;
    int8_t        wifi_strongest;
    sentinel_ap_t ap[SENTINEL_AP_MAX];
    uint8_t       ap_n;    /* complete list from the last scan */
    uint8_t       ap_fill; /* rows received for the scan in progress */
    uint32_t      wifi_scans;

    /* Events, newest at (evt_head - 1) */
    sentinel_event_t evt[SENTINEL_EVT_MAX];
    uint8_t          evt_head, evt_count;
    uint32_t         warn_count;

    /* Radio lines wired to the Tab5 (levels as read; nrf_irq is active low) */
    bool gdo0, gdo2, nrf_irq;
} sentinel_state_t;

/* Power the board, put the CC1101 in receive and start reading. Safe to call
 * twice. On the device the reader task stays for the life of the firmware;
 * sentinel_stop() lowers RX_EN, cuts EXT5V and leaves it idle. */
void sentinel_start(void);
void sentinel_stop(void);
bool sentinel_running(void);

/* Copy the current state out (thread safe). */
void sentinel_snapshot(sentinel_state_t *out);

/* Milliseconds on the clock the state's timestamps use. */
uint32_t sentinel_now_ms(void);

/* A frame has arrived recently. */
bool sentinel_link(const sentinel_state_t *s);

/* Send a command to the C5: "CMD,HELLO", "CMD,PING", "CMD,QUIET", "CMD,STREAM",
 * "CMD,RATE,<ms>". Returns false when the board is off. */
bool sentinel_send(const char *payload);

/* Newest event first; NULL past the end. */
const sentinel_event_t *sentinel_event(const sentinel_state_t *s, int newest_index);

#ifdef __cplusplus
}
#endif
