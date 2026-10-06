/*
 * Audio output used by the player: 16-bit interleaved PCM. aout_tab5.c
 * drives the ES8388 over I2S; the simulator's aout_sim.c uses SDL.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool aout_open(uint32_t rate, int channels); /* (re)open for this format */
void aout_write(const int16_t *pcm, size_t count); /* count int16 values; blocks until queued */
void aout_close(void);                         /* stop; amplifier off */
void aout_volume(int percent);                 /* 0..100 */
/* Called about twice a second while playing: follow the headphone jack. */
void aout_route(bool headphones);
