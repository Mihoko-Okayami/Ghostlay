#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "battery.h"
#include "notif.h"
#include "samples.h"

const struct sample_note sample_history[SAMPLE_HISTORY] = {
	{ L"VRCX", L"Mihoko Okayami Booped You!", L"", NK_NORMAL, NI_IMAGE, -1, -1, 21, 46 },
	{ L"", L"", L"", NK_CRITICAL, NI_BATTERY, BD_RIGHT, 12, 21, 41 },
	{ L"VRCX", L"Mihoko Okayami has requested an invite", L"", NK_NORMAL, NI_IMAGE, -1, -1, 21, 38 },
	{ L"", L"", L"", NK_NORMAL, NI_BATTERY, BD_LEFT, 24, 21, 31 },
	{ L"VRCX", L"Mihoko Okayami has left", L"", NK_NORMAL, NI_IMAGE, -1, -1, 21, 24 },
};

const struct sample_card sample_cards[SAMPLE_CARDS] = {
	{ L"vrcx", L"VRCX", L"Mihoko Okayami has requested an invite", L"", NK_NORMAL, NI_IMAGE, -1, -1, 0.42f },
	{ L"test", NULL, NULL, NULL, NK_NORMAL, NI_DEFAULT, -1, -1, 0.85f },
	{ L"critical", L"", L"", L"", NK_CRITICAL, NI_BATTERY, BD_RIGHT, 15, 0.88f },
	{ L"warning", L"", L"", L"", NK_NORMAL, NI_BATTERY, BD_TRACKER + 2, 25, 0.6f },
	{ L"long", L"Workflow finished with warnings", L"The release build for ghostlay completed, but three steps reported warnings. Open the run summary to review the annotations before the next version goes out, and remember to update the changelog so everyone knows what changed in this release.", L"GitHub Actions", NK_NORMAL, NI_WARNING, -1, -1, 0.3f },
	{ L"error", L"Connection lost", L"The OSC server stopped responding", L"VRCOSC", NK_NORMAL, NI_ERROR, -1, -1, 0.65f },
	{ L"title-only", L"Mihoko Okayami sent you a friend request", L"", L"VRCX", NK_NORMAL, NI_DEFAULT, -1, -1, 0.95f },
};

const struct sample_toast sample_toasts[SAMPLE_TOASTS] = {
	{ L"dm", "Mihoko-Okayami", "Are you joining tonight? We start at 9 &#x1F389;", true, 1, 0.62f },
	{ L"server", "Mihoko-Okayami (#vr-meetup, Test Server)", "Pushed the new build, can someone try it in VR before tonight?", true, 1, 0.45f },
	{ L"initial", "Mihoko-Okayami", "omw, 5 min &amp; I bring the snacks", false, 1, 0.8f },
	{ L"burst", "Mihoko-Okayami", "Okay, see you there!", true, 3, 0.7f },
};

const struct sample_media sample_media = { L"White Noise Bunny", L"Mihoko Okayami", 161, 257 };
const struct sample_media sample_media_long = { L"Bohemian Rhapsody (Remastered 2011)", L"Queen", 161, 355 };
const float sample_levels[3] = { 0.78f, 0.64f, 0.31f };

const struct sample_device sample_trackers[SAMPLE_TRACKERS] = {
	{ BR_WAIST, 0.82f, true },
	{ BR_LEFT_FOOT, 0.77f, false },
	{ BR_RIGHT_FOOT, 0.71f, false },
	{ BR_CAMERA, 0.58f, false },
	{ BR_NONE, 0.44f, false },
	{ BR_NONE, 0.22f, false },
};

const struct sample_device sample_many[SAMPLE_MANY] = {
	{ BR_CHEST, 0.86f, false },
	{ BR_WAIST, 0.82f, false },
	{ BR_LEFT_ELBOW, 0.74f, false },
	{ BR_RIGHT_ELBOW, 0.69f, false },
	{ BR_LEFT_KNEE, 0.63f, false },
	{ BR_RIGHT_KNEE, 0.58f, false },
	{ BR_LEFT_FOOT, 0.51f, false },
	{ BR_RIGHT_FOOT, 0.47f, false },
	{ BR_CAMERA, -1, false },
	{ BR_NONE, 0.31f, false },
	{ BR_NONE, 0.12f, false },
};

void sample_path(const WCHAR *name, WCHAR *out, int cap) {
	WCHAR rel[MAX_PATH];
	const char *f = __FILE__, *e;
	for (e = f + strlen(f); e > f && e[-1] != '/' && e[-1] != '\\'; e--) continue;
	swprintf(rel, MAX_PATH, L"%.*hs%ls", (int)(e - f), f, name);
	if (!GetFullPathNameW(rel, (DWORD)cap, out, NULL)) {
		out[0] = 0;
	}
}
