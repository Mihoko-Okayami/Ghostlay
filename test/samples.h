#ifndef SAMPLES_H
#define SAMPLES_H

#include <stdbool.h>
#include <windows.h>

enum { SAMPLE_HISTORY = 5, SAMPLE_CARDS = 7, SAMPLE_TOASTS = 4, SAMPLE_TRACKERS = 6, SAMPLE_MANY = 11 };

struct sample_note {
	const WCHAR *title, *body, *source;
	int kind, icon, dev, pct, hour, minute;
};

struct sample_card {
	const WCHAR *name, *title, *body, *source;
	int kind, icon, dev, pct;
	float f;
};

struct sample_toast {
	const WCHAR *name;
	const char *title, *body;
	bool avatar;
	int count;
	float f;
};

struct sample_media {
	const WCHAR *title, *artist;
	int pos, dur;
};

struct sample_device {
	int role;
	float pct;
	bool charging;
};

extern const struct sample_note sample_history[SAMPLE_HISTORY];
extern const struct sample_card sample_cards[SAMPLE_CARDS];
extern const struct sample_toast sample_toasts[SAMPLE_TOASTS];
extern const struct sample_media sample_media, sample_media_long;
extern const float sample_levels[3];
extern const struct sample_device sample_trackers[SAMPLE_TRACKERS], sample_many[SAMPLE_MANY];

void sample_path(const WCHAR *name, WCHAR *out, int cap);

#endif
