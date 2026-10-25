#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>

#include "audio.h"

#define SR 44100
#define TAU 6.2831853f

static const int lens[SFX_COUNT] = { SR * 55 / 100, SR * 42 / 100 };
static int16_t *pcm[SFX_COUNT];
static uint8_t *wav;

static float bell(float t, float f, float decay) {
	if (t < 0) return 0;
	return expf(-t * decay) * fminf(1, t * 300) * (sinf(TAU * f * t) + 0.3f * expf(-t * 12) * sinf(TAU * 2.76f * f * t) + 0.12f * sinf(TAU * 2 * f * t));
}

static float beep(float t) {
	if (t < 0 || t > 0.13f) return 0;
	return fminf(1, fminf(t, 0.13f - t) * 160) * (sinf(TAU * 587.3f * t) + 0.25f * sinf(TAU * 3 * 587.3f * t));
}

static float sample(int sfx, float t) {
	return sfx == SFX_NOTIFY ? bell(t, 880, 7) + 0.8f * bell(t - 0.13f, 1318.5f, 6.5f) : beep(t) + beep(t - 0.2f);
}

static void synth(int sfx, int16_t *o, int n) {
	float peak = 1e-6f;
	int i;
	for (i = 0; i < n; i++) {
		peak = fmaxf(peak, fabsf(sample(sfx, (float)i / SR)));
	}
	for (i = 0; i < n; i++) {
		o[i] = (int16_t)lrintf(sample(sfx, (float)i / SR) * fminf(1, (n - i) / (SR * 0.03f)) * 26000 / peak);
	}
}

void audio_init(void) {
	int mx = 0;
	size_t total = 44;
	int16_t *p;
	int i;
	if (wav) return;
	for (i = 0; i < SFX_COUNT; i++) {
		mx = lens[i] > mx ? lens[i] : mx;
		total += (size_t)lens[i] * 2;
	}
	if (!(wav = malloc(total + (size_t)mx * 2))) return;
	p = (int16_t *)(wav + 44 + (size_t)mx * 2);
	for (i = 0; i < SFX_COUNT; i++) {
		pcm[i] = p;
		synth(i, p, lens[i]);
		p += lens[i];
	}
}

void audio_shutdown(void) {
	if (!wav) return;
	PlaySoundW(NULL, NULL, 0);
	free(wav);
	wav = NULL;
}

void audio_play(int sfx, float volume) {
	float g;
	int n, i;
	uint32_t data, hdr[11] = { 0x46464952, 0, 0x45564157, 0x20746D66, 16, 0x10001, SR, SR * 2, 0x100002, 0x61746164, 0 };
	int16_t *o;
	if (!wav || sfx < 0 || sfx >= SFX_COUNT || !(volume > 0)) return;
	g = fminf(volume, 1);
	n = lens[sfx];
	data = (uint32_t)n * 2;
	hdr[1] = 36 + data;
	hdr[10] = data;
	o = (int16_t *)(wav + 44);
	PlaySoundW(NULL, NULL, 0);
	memcpy(wav, hdr, sizeof(hdr));
	for (i = 0; i < n; i++) {
		o[i] = (int16_t)lrintf(pcm[sfx][i] * g);
	}
	PlaySoundW((LPCWSTR)wav, NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}
