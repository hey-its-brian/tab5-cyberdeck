/*
 * Music player: MP3 files from the storage, decoded with minimp3 on a
 * background task and played through the speaker or the headphone jack.
 * Playback continues while other modules are in front.
 *
 * The queue is one folder's tracks (sorted by name). All functions are safe
 * to call from the LVGL task; they post to the player task and return.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLAYER_MAX_TRACKS 256
#define PLAYER_BANDS 16

typedef enum { PLAYER_STOPPED, PLAYER_PLAYING, PLAYER_PAUSED } player_state_t;
typedef enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE } player_repeat_t;

void player_init(void);

/* Replace the queue with the .mp3 files in `dir` (full path) and start
 * track `index` (or just load it with index < 0). */
bool player_open_dir(const char *dir, int index);
const char *player_dir(void);
int player_count(void);
const char *player_file(int index); /* file name in the queue */

void player_play(int index);
void player_toggle(void); /* play / pause; starts track 0 when stopped */
void player_stop(void);
void player_next(void);
void player_prev(void); /* restarts the track if more than 3 s in */
void player_seek(int delta_s);

void player_set_volume(int percent); /* 0..100, saved */
int player_volume(void);
void player_set_shuffle(bool on);
bool player_shuffle(void);
void player_set_repeat(player_repeat_t r);
player_repeat_t player_repeat(void);

player_state_t player_state(void);
int player_index(void);
uint32_t player_pos_ms(void);
uint32_t player_dur_ms(void); /* 0 when unknown */
const char *player_title(void);  /* ID3 title, or the file name */
const char *player_artist(void); /* ID3 artist, or "" */
bool player_headphones(void);
const char *player_error(void); /* last problem, "" if none */

/* Spectrum-ish levels 0..255 for the visualizer (decays when quiet). */
void player_levels(uint8_t out[PLAYER_BANDS]);

#ifdef __cplusplus
}
#endif
