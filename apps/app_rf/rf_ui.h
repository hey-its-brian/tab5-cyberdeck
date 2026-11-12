/*
 * Inside RF WATCH: what app_rf.c (the module, the mode gate, the chooser and
 * the navigation between them) and each tool screen share. A tool builds
 * itself into the parent it is given and tears itself down again; app_rf.c
 * decides which one is up and hands it the keys Esc did not claim.
 */
#pragma once

#include "deck_app.h"
#include "deck_input.h"
#include "deck_sentinel.h"
#include "lvgl.h"

/* Chosen at the gate when the app is opened, fixed until it is left. */
typedef enum { RF_MODE_SAFE, RF_MODE_RED } rf_mode_t;
rf_mode_t rf_mode(void);

/* A chamfered panel with a section heading, laid out as a column. */
lv_obj_t *rf_panel(lv_obj_t *parent, const char *title);

/* OVERVIEW: link and board health, GNSS, Wi-Fi, both sweeps, events. */
void rf_overview_build(lv_obj_t *parent);
void rf_overview_destroy(void);
bool rf_overview_key(const deck_key_t *k);

/* BLUETOOTH: the BLE devices in earshot, trackers and surveillance devices
 * labelled, capture to SD. Takes the node's radio full time while it is up. */
void rf_ble_build(lv_obj_t *parent);
void rf_ble_destroy(void);
bool rf_ble_key(const deck_key_t *k);

/* WI-FI: the waterfall, the access points, drones, and (RED TEAM only) the
 * honeypot. Makes the node hop every channel while it is up. */
void rf_wifi_build(lv_obj_t *parent);
void rf_wifi_destroy(void);
bool rf_wifi_key(const deck_key_t *k);

/* SUB-GHZ and 2.4 GHZ (rf_bands.c): the two swept bands as spectrum
 * analyzers with history, the burst listener on 433 MHz, a site survey to
 * a file, and what the jam watch thinks. */
void rf_sub_build(lv_obj_t *parent);
void rf_sub_destroy(void);
bool rf_sub_key(const deck_key_t *k);
void rf_nrf_build(lv_obj_t *parent);
void rf_nrf_destroy(void);
bool rf_nrf_key(const deck_key_t *k);

/* WARDRIVE: Wi-Fi and Bluetooth with the GPS position, written to the SD
 * card in WiGLE's format. Listening only. The drive ends with the screen. */
void rf_drive_build(lv_obj_t *parent);
void rf_drive_destroy(void);
bool rf_drive_key(const deck_key_t *k);

/* LOG: today's events file from storage, newest first. */
void rf_log_build(lv_obj_t *parent);
void rf_log_destroy(void);
bool rf_log_key(const deck_key_t *k);
