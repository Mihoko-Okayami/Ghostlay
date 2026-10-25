#ifndef AUDIO_H
#define AUDIO_H

enum { SFX_NONE = -1, SFX_NOTIFY, SFX_ALERT, SFX_COUNT };

void audio_init(void);
void audio_shutdown(void);
void audio_play(int sfx, float volume);

#endif
