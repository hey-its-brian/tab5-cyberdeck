/* deck_audio output for the simulator: SDL audio, so the player can be
 * heard on the Mac. Without an audio device (headless runs) it just paces
 * itself in real time. */
#include "aout.h"

#include <SDL.h>
#include <stdio.h>
#include <unistd.h>

static SDL_AudioDeviceID s_dev;
static uint32_t s_rate = 44100;
static int s_ch        = 2;
static int s_volume    = 60;

bool aout_open(uint32_t rate, int channels)
{
    aout_close();
    s_rate = rate;
    s_ch   = channels;
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return true; /* silent */
    SDL_AudioSpec want = {0}, have;
    want.freq          = (int)rate;
    want.format        = AUDIO_S16SYS;
    want.channels      = (Uint8)channels;
    want.samples       = 2048;
    s_dev              = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_dev) SDL_PauseAudioDevice(s_dev, 0);
    printf("[sim] audio %u Hz x%d %s\n", rate, channels, s_dev ? "on the Mac's output" : "(no device, silent)");
    return true;
}

void aout_write(const int16_t *pcm, size_t count)
{
    size_t frames = count / (size_t)s_ch;
    if (!s_dev) {
        usleep((useconds_t)(frames * 1000000ull / s_rate));
        return;
    }
    static int16_t buf[1152 * 2];
    if (count > sizeof(buf) / sizeof(buf[0])) count = sizeof(buf) / sizeof(buf[0]);
    for (size_t i = 0; i < count; i++) buf[i] = (int16_t)(pcm[i] * s_volume / 100);
    SDL_QueueAudio(s_dev, buf, (Uint32)(count * sizeof(int16_t)));
    /* Keep about 150 ms queued so pause and seek respond quickly. */
    while (SDL_GetQueuedAudioSize(s_dev) > s_rate * (Uint32)s_ch * 2 * 150 / 1000) usleep(5000);
}

void aout_close(void)
{
    if (s_dev) SDL_CloseAudioDevice(s_dev);
    s_dev = 0;
}

void aout_volume(int percent) { s_volume = percent; }
void aout_route(bool headphones) { (void)headphones; }
