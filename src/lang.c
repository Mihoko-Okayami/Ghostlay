#include <math.h>
#include <stdbool.h>
#include <wchar.h>
#include <windows.h>

#include "cfg.h"
#include "lang.h"
#include "main.h"

static const WCHAR *const en[S_COUNT] = {
	[S_APP_NAME] = L"Ghostlay",
	[S_CONFIGURATION] = L"Configuration",
	[S_AM] = L"AM",
	[S_PM] = L"PM",
	[S_W_HEADSET] = L"Headset",
	[S_W_TRACKERS] = L"Trackers",
	[S_W_LEFT] = L"Left",
	[S_W_RIGHT] = L"Right",
	[S_W_HISTORY] = L"History",
	[S_W_RECENT] = L"Recent notifications",
	[S_W_HIST_EMPTY] = L"No recent notifications",
	[S_W_HIST_KEPT] = L"The last 20 are kept here",
	[S_W_DND] = L"Do not disturb",
	[S_W_MISSED_ONE] = L"%d missed",
	[S_W_MISSED] = L"%d missed",
	[S_W_HANDED] = L"Held in hand",
	[S_W_LEFT_FOOT] = L"Left foot",
	[S_W_RIGHT_FOOT] = L"Right foot",
	[S_W_LEFT_SHOULDER] = L"Left shoulder",
	[S_W_RIGHT_SHOULDER] = L"Right shoulder",
	[S_W_LEFT_ELBOW] = L"Left elbow",
	[S_W_RIGHT_ELBOW] = L"Right elbow",
	[S_W_LEFT_KNEE] = L"Left knee",
	[S_W_RIGHT_KNEE] = L"Right knee",
	[S_W_LEFT_WRIST] = L"Left wrist",
	[S_W_RIGHT_WRIST] = L"Right wrist",
	[S_W_LEFT_ANKLE] = L"Left ankle",
	[S_W_RIGHT_ANKLE] = L"Right ankle",
	[S_W_WAIST] = L"Waist",
	[S_W_CHEST] = L"Chest",
	[S_W_CAMERA] = L"Camera",
	[S_W_KEYBOARD] = L"Keyboard",
	[S_N_LOW_BATTERY] = L"Low battery",
	[S_N_DEV_LEFT] = L"Left controller",
	[S_N_DEV_RIGHT] = L"Right controller",
	[S_N_DEV_TRACKER] = L"Tracker %d",
	[S_N_TEST_TITLE] = L"Test notification",
	[S_N_TEST_BODY] = L"This is where your notifications will appear",
	[S_N_MESSAGES] = L"%d messages",
	[S_P_GENERAL] = L"General",
	[S_P_WATCH] = L"Watch",
	[S_P_NOTIFICATIONS] = L"Notifications",
	[S_P_BATTERY] = L"Battery alerts",
	[S_P_CLOSE] = L"Close",
	[S_P_GRAB_HINT] = L"Hold the bar to move",
	[S_G_PREFERENCES] = L"Preferences",
	[S_G_LANGUAGE] = L"Language",
	[S_G_LANGUAGE_SUB] = L"Used for all Ghostlay text",
	[S_G_DEFAULT] = L"Default",
	[S_G_AUTOLAUNCH] = L"Launch with SteamVR",
	[S_G_AUTOLAUNCH_SUB] = L"Ghostlay starts and stops with SteamVR",
	[S_G_ABOUT] = L"About",
	[S_G_VERSION] = L"Version",
	[S_G_QUIT_SECTION] = L"Quit",
	[S_G_QUIT_SUB] = L"Ends Ghostlay for this SteamVR session",
	[S_G_QUIT] = L"Quit Ghostlay",
	[S_WP_PLACEMENT] = L"Placement",
	[S_WP_WRIST] = L"Wrist",
	[S_WP_WRIST_SUB] = L"Hand that wears the watch",
	[S_WP_SIZE] = L"Watch size",
	[S_WP_SIZE_SUB] = L"Default: 12.7 cm wide",
	[S_WP_ANGLE] = L"Viewing angle",
	[S_WP_ANGLE_SUB] = L"How far it can tilt before it hides",
	[S_WP_OPACITY] = L"Opacity",
	[S_WP_OPACITY_SUB] = L"Lower it to see through the watch",
	[S_WP_DISPLAY] = L"Display",
	[S_WP_TIME_FORMAT] = L"Time format",
	[S_WP_PREVIEW] = L"Preview: %ls",
	[S_WP_ACCENT] = L"Accent colour",
	[S_WP_ACCENT_SUB] = L"Time separator, buttons, highlights",
	[S_WP_CHOOSE] = L"Choose…",
	[S_WP_MEDIA] = L"Media controls",
	[S_WP_MEDIA_SUB] = L"Shows the current track and its controls",
	[S_C_WHEEL] = L"Colour wheel",
	[S_C_BRIGHTNESS] = L"Brightness",
	[S_C_SELECTED] = L"Selected colour",
	[S_C_READABLE] = L"Readable",
	[S_C_LOW_CONTRAST] = L"Low contrast",
	[S_C_HTML] = L"HTML code",
	[S_C_PRESETS] = L"Presets",
	[S_C_RESET] = L"Reset",
	[S_NP_SEND_TEST] = L"Test notification",
	[S_NP_POSITION] = L"Position",
	[S_NP_RESET_POS] = L"Reset position",
	[S_NP_DISTANCE] = L"Distance",
	[S_NP_HEIGHT] = L"Height below gaze",
	[S_NP_HOFF] = L"Horizontal offset",
	[S_NP_SIZE] = L"Size",
	[S_NP_MIN] = L"Minimum",
	[S_NP_MAX] = L"Maximum",
	[S_NP_NOTIF_SOUND] = L"Notification sound",
	[S_NP_VOLUME] = L"Volume",
	[S_NP_EXTERNAL] = L"External notifications",
	[S_NP_RECEIVE] = L"Receive notifications from other apps",
	[S_NP_RECEIVE_SUB] = L"XSOverlay-compatible apps such as VRCX or OyasumiVR",
	[S_NP_PORT] = L"UDP port",
	[S_NP_PORT_SUB] = L"Local port for incoming notifications",
	[S_NP_LISTENING] = L"Listening on",
	[S_NP_PORT_IN_USE] = L"Port in use by %ls",
	[S_NP_UDP_OFF] = L"Off",
	[S_NP_UDP_ERROR] = L"Port unavailable",
	[S_NP_STARTING] = L"Starting…",
	[S_NP_DURATION_SOUND] = L"Duration and sound",
	[S_NP_DISCORD] = L"Discord",
	[S_NP_DISCORD_SUB] = L"Shows Discord notifications",
	[S_NP_DISCORD_ACTIVE] = L"Active",
	[S_NP_DISCORD_BLOCKED] = L"Blocked by Windows privacy settings",
	[S_NP_DISCORD_UNAVAILABLE] = L"Unavailable: the notification database cannot be read",
	[S_BP_TEST] = L"Test alert",
	[S_BP_ALERTS] = L"Alerts",
	[S_BP_ALERTS_SUB] = L"Sent as a notification when a threshold is crossed",
	[S_BP_SOUND] = L"Alert sound",
	[S_BP_SOUND_SUB] = L"Plays the warning sound with each alert",
	[S_BP_THRESHOLDS] = L"Thresholds",
	[S_BP_THRESHOLDS_SUB] = L"Select a threshold to edit it",
	[S_BP_ADD] = L"Add threshold",
	[S_BP_NORMAL] = L"Normal",
	[S_BP_ABOVE] = L"above %ls",
	[S_BP_WARNING] = L"Warning",
	[S_BP_BELOW] = L"below a threshold",
	[S_BP_CRITICAL] = L"Critical",
	[S_BP_LOWEST] = L"lowest threshold",
	[S_BP_DEVICES] = L"Monitored devices",
	[S_BP_CONTROLLERS] = L"Controllers",
	[S_BP_NOTIFS_OFF] = L"Notifications are turned off",
	[S_KP_THRESHOLD] = L"Threshold",
	[S_KP_PORT_HINT] = L"Between 1024 and 65535",
	[S_KP_TIER_HINT] = L"5 – 95%",
	[S_KP_HEX_HINT] = L"6 hex digits",
	[S_KP_TAKEN] = L"This threshold already exists",
	[S_KP_CLEAR] = L"Clear",
	[S_KP_OK] = L"OK",
};

static const WCHAR *const fr[S_COUNT] = {
	[S_APP_NAME] = L"Ghostlay",
	[S_CONFIGURATION] = L"Configuration",
	[S_AM] = L"AM",
	[S_PM] = L"PM",
	[S_W_HEADSET] = L"Casque",
	[S_W_TRACKERS] = L"Trackers",
	[S_W_LEFT] = L"Gauche",
	[S_W_RIGHT] = L"Droite",
	[S_W_HISTORY] = L"Historique",
	[S_W_RECENT] = L"Notifications récentes",
	[S_W_HIST_EMPTY] = L"Aucune notification récente",
	[S_W_HIST_KEPT] = L"Les 20 dernières sont gardées ici",
	[S_W_DND] = L"Ne pas déranger",
	[S_W_MISSED_ONE] = L"%d manquée",
	[S_W_MISSED] = L"%d manquées",
	[S_W_HANDED] = L"En main",
	[S_W_LEFT_FOOT] = L"Pied G",
	[S_W_RIGHT_FOOT] = L"Pied D",
	[S_W_LEFT_SHOULDER] = L"Épaule G",
	[S_W_RIGHT_SHOULDER] = L"Épaule D",
	[S_W_LEFT_ELBOW] = L"Coude G",
	[S_W_RIGHT_ELBOW] = L"Coude D",
	[S_W_LEFT_KNEE] = L"Genou G",
	[S_W_RIGHT_KNEE] = L"Genou D",
	[S_W_LEFT_WRIST] = L"Poignet G",
	[S_W_RIGHT_WRIST] = L"Poignet D",
	[S_W_LEFT_ANKLE] = L"Cheville G",
	[S_W_RIGHT_ANKLE] = L"Cheville D",
	[S_W_WAIST] = L"Taille",
	[S_W_CHEST] = L"Poitrine",
	[S_W_CAMERA] = L"Caméra",
	[S_W_KEYBOARD] = L"Clavier",
	[S_N_LOW_BATTERY] = L"Batterie faible",
	[S_N_DEV_LEFT] = L"Manette gauche",
	[S_N_DEV_RIGHT] = L"Manette droite",
	[S_N_DEV_TRACKER] = L"Tracker %d",
	[S_N_TEST_TITLE] = L"Notification de test",
	[S_N_TEST_BODY] = L"C’est ici que s’afficheront vos notifications",
	[S_N_MESSAGES] = L"%d messages",
	[S_P_GENERAL] = L"Général",
	[S_P_WATCH] = L"Montre",
	[S_P_NOTIFICATIONS] = L"Notifications",
	[S_P_BATTERY] = L"Alertes batterie",
	[S_P_CLOSE] = L"Fermer",
	[S_P_GRAB_HINT] = L"Maintenez la barre pour déplacer",
	[S_G_PREFERENCES] = L"Préférences",
	[S_G_LANGUAGE] = L"Langue",
	[S_G_LANGUAGE_SUB] = L"Utilisée pour tous les textes de Ghostlay",
	[S_G_DEFAULT] = L"Par défaut",
	[S_G_AUTOLAUNCH] = L"Lancer avec SteamVR",
	[S_G_AUTOLAUNCH_SUB] = L"Ghostlay démarre et s’arrête avec SteamVR",
	[S_G_ABOUT] = L"À propos",
	[S_G_VERSION] = L"Version",
	[S_G_QUIT_SECTION] = L"Quitter",
	[S_G_QUIT_SUB] = L"Ferme Ghostlay pour cette session SteamVR",
	[S_G_QUIT] = L"Quitter Ghostlay",
	[S_WP_PLACEMENT] = L"Emplacement",
	[S_WP_WRIST] = L"Poignet",
	[S_WP_WRIST_SUB] = L"Main qui porte la montre",
	[S_WP_SIZE] = L"Taille de la montre",
	[S_WP_SIZE_SUB] = L"Par défaut : 12,7 cm de large",
	[S_WP_ANGLE] = L"Angle d’apparition",
	[S_WP_ANGLE_SUB] = L"Inclinaison tolérée avant de disparaître",
	[S_WP_OPACITY] = L"Opacité",
	[S_WP_OPACITY_SUB] = L"Baissez-la pour voir à travers la montre",
	[S_WP_DISPLAY] = L"Affichage",
	[S_WP_TIME_FORMAT] = L"Format de l’heure",
	[S_WP_PREVIEW] = L"Aperçu : %ls",
	[S_WP_ACCENT] = L"Couleur d’accent",
	[S_WP_ACCENT_SUB] = L"Séparateur de l’heure, boutons, surlignages",
	[S_WP_CHOOSE] = L"Choisir…",
	[S_WP_MEDIA] = L"Contrôles média",
	[S_WP_MEDIA_SUB] = L"Affiche le morceau en cours et ses contrôles",
	[S_C_WHEEL] = L"Roue chromatique",
	[S_C_BRIGHTNESS] = L"Luminosité",
	[S_C_SELECTED] = L"Couleur choisie",
	[S_C_READABLE] = L"Lisible",
	[S_C_LOW_CONTRAST] = L"Contraste faible",
	[S_C_HTML] = L"Code HTML",
	[S_C_PRESETS] = L"Préréglages",
	[S_C_RESET] = L"Réinitialiser",
	[S_NP_SEND_TEST] = L"Tester une notification",
	[S_NP_POSITION] = L"Position",
	[S_NP_RESET_POS] = L"Réinitialiser la position",
	[S_NP_DISTANCE] = L"Distance",
	[S_NP_HEIGHT] = L"Hauteur sous le regard",
	[S_NP_HOFF] = L"Décalage horizontal",
	[S_NP_SIZE] = L"Taille",
	[S_NP_MIN] = L"Minimum",
	[S_NP_MAX] = L"Maximum",
	[S_NP_NOTIF_SOUND] = L"Son de notification",
	[S_NP_VOLUME] = L"Volume",
	[S_NP_EXTERNAL] = L"Notifications externes",
	[S_NP_RECEIVE] = L"Recevoir les notifications d’autres applis",
	[S_NP_RECEIVE_SUB] = L"Applis compatibles XSOverlay comme VRCX ou OyasumiVR",
	[S_NP_PORT] = L"Port UDP",
	[S_NP_PORT_SUB] = L"Port local des notifications entrantes",
	[S_NP_LISTENING] = L"À l’écoute sur",
	[S_NP_PORT_IN_USE] = L"Port utilisé par %ls",
	[S_NP_UDP_OFF] = L"Désactivé",
	[S_NP_UDP_ERROR] = L"Port indisponible",
	[S_NP_STARTING] = L"Démarrage…",
	[S_NP_DURATION_SOUND] = L"Durée et son",
	[S_NP_DISCORD] = L"Discord",
	[S_NP_DISCORD_SUB] = L"Affiche les notifications Discord",
	[S_NP_DISCORD_ACTIVE] = L"Actif",
	[S_NP_DISCORD_BLOCKED] = L"Bloqué par les paramètres de confidentialité de Windows",
	[S_NP_DISCORD_UNAVAILABLE] = L"Indisponible : la base des notifications est illisible",
	[S_BP_TEST] = L"Tester une alerte",
	[S_BP_ALERTS] = L"Alertes",
	[S_BP_ALERTS_SUB] = L"Envoyées en notification quand un seuil est franchi",
	[S_BP_SOUND] = L"Son d’alerte",
	[S_BP_SOUND_SUB] = L"Joue le son d’avertissement à chaque alerte",
	[S_BP_THRESHOLDS] = L"Seuils",
	[S_BP_THRESHOLDS_SUB] = L"Sélectionnez un seuil pour le modifier",
	[S_BP_ADD] = L"Ajouter un seuil",
	[S_BP_NORMAL] = L"Normal",
	[S_BP_ABOVE] = L"au-dessus de %ls",
	[S_BP_WARNING] = L"Avertissement",
	[S_BP_BELOW] = L"sous un seuil",
	[S_BP_CRITICAL] = L"Critique",
	[S_BP_LOWEST] = L"seuil le plus bas",
	[S_BP_DEVICES] = L"Appareils surveillés",
	[S_BP_CONTROLLERS] = L"Manettes",
	[S_BP_NOTIFS_OFF] = L"Les notifications sont désactivées",
	[S_KP_THRESHOLD] = L"Seuil",
	[S_KP_PORT_HINT] = L"Entre 1024 et 65535",
	[S_KP_TIER_HINT] = L"5 – 95 %",
	[S_KP_HEX_HINT] = L"6 chiffres hexadécimaux",
	[S_KP_TAKEN] = L"Ce seuil existe déjà",
	[S_KP_CLEAR] = L"Effacer",
	[S_KP_OK] = L"OK",
};

static const WCHAR *const *const tables[LANG_COUNT] = { en, fr };
static const WCHAR *const names[LANG_COUNT] = { L"English", L"Français" };
static const WCHAR *const days[LANG_COUNT][7] = { { L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday" }, { L"Dimanche", L"Lundi", L"Mardi", L"Mercredi", L"Jeudi", L"Vendredi", L"Samedi" } };
static const WCHAR *const days_short[LANG_COUNT][7] = { { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" }, { L"Dim", L"Lun", L"Mar", L"Mer", L"Jeu", L"Ven", L"Sam" } };
static const WCHAR *const months[LANG_COUNT][12] = { { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" }, { L"Janv", L"Févr", L"Mars", L"Avr", L"Mai", L"Juin", L"Juil", L"Août", L"Sept", L"Oct", L"Nov", L"Déc" } };

static int lang(void) {
	return g_cfg.lang;
}

static int out(int k) {
	return k < 0 ? 0 : k;
}

const WCHAR *tr(enum str_id id) {
	return tables[lang()][id];
}

const WCHAR *lang_native_name(int l) {
	return names[l];
}

static const WCHAR *lang_day(int wday) {
	return days[lang()][wday];
}

static const WCHAR *lang_day_short(int wday) {
	return days_short[lang()][wday];
}

static const WCHAR *lang_month_short(int month) {
	return months[lang()][month - 1];
}

int fmt_time(WCHAR *b, int n, const SYSTEMTIME *t, bool h24) {
	if (h24) return out(swprintf(b, n, L"%02d:%02d", t->wHour, t->wMinute));
	return out(swprintf(b, n, L"%d:%02d", t->wHour % 12 ? t->wHour % 12 : 12, t->wMinute));
}

const WCHAR *fmt_ampm(const SYSTEMTIME *t) {
	return tr(t->wHour < 12 ? S_AM : S_PM);
}

void fmt_date_short(WCHAR *b, int n, const SYSTEMTIME *t) {
	swprintf(b, n, L"%ls %02d/%02d/%04d", lang_day_short(t->wDayOfWeek), t->wDay, t->wMonth, t->wYear);
}

void fmt_date_wd(WCHAR *b, int n, const SYSTEMTIME *t, int wd) {
	if (wd >= 2) {
		swprintf(b, n, L"%d %ls", t->wDay, lang_month_short(t->wMonth));
	} else {
		swprintf(b, n, L"%ls %d %ls", wd ? lang_day_short(t->wDayOfWeek) : lang_day(t->wDayOfWeek), t->wDay, lang_month_short(t->wMonth));
	}
}

void fmt_num(WCHAR *b, int n, double v, int decimals, const WCHAR *unit) {
	WCHAR *p;
	int k;
	if (fabs(v) < 0.5 * pow(10, -decimals)) {
		v = 0;
	}
	k = out(swprintf(b, n, L"%.*f", decimals, v));
	if (lang() == LANG_FR) {
		for (p = b; *p; p++) {
			if (*p == L'.') {
				*p = L',';
			}
		}
	}
	if (!unit || !*unit || k >= n) return;
	swprintf(b + k, n - k, unit[0] == L'°' || (unit[0] == L'%' && lang() != LANG_FR) ? L"%ls" : L" %ls", unit);
}

void fmt_hms(WCHAR *b, int n, double secs) {
	long s = secs > 0 ? (long)secs : 0;
	swprintf(b, n, L"%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
}

void fmt_clock(WCHAR *b, int n, double secs) {
	long s = secs > 0 ? (long)secs : 0;
	if (s >= 3600) {
		fmt_hms(b, n, secs);
	} else {
		swprintf(b, n, L"%ld:%02ld", s / 60, s % 60);
	}
}

#ifdef GL_TESTS
static int check(const WCHAR *got, const WCHAR *want, const char *what) {
	char g[256], w[256];
	if (!wcscmp(got, want)) return 0;
	to_utf8(got, g, sizeof(g));
	to_utf8(want, w, sizeof(w));
	glog("lang: %s gave \"%s\", expected \"%s\"", what, g, w);
	return 1;
}

int lang_selftest(void) {
	int fail = 0, saved = g_cfg.lang, i;
	WCHAR b[128];
	SYSTEMTIME t = { .wYear = 2026, .wMonth = 10, .wDayOfWeek = 5, .wDay = 23, .wHour = 21, .wMinute = 47, .wSecond = 7 };
	for (i = 0; i < S_COUNT; i++) {
		if (!en[i] || !fr[i]) {
			glog("lang: string %d missing (%s)", i, en[i] ? "fr" : "en");
			fail++;
		}
	}
	g_cfg.lang = LANG_EN;
	fmt_date_wd(b, 128, &t, 0);
	fail += check(b, L"Friday 23 Oct", "en date");
	fmt_date_short(b, 128, &t);
	fail += check(b, L"Fri 23/10/2026", "en short date");
	fmt_date_wd(b, 128, &t, 1);
	fail += check(b, L"Fri 23 Oct", "en short weekday date");
	fmt_date_wd(b, 128, &t, 2);
	fail += check(b, L"23 Oct", "en date without weekday");
	fmt_time(b, 128, &t, false);
	fail += check(b, L"9:47", "en 12 h time");
	fail += check(fmt_ampm(&t), L"PM", "en am/pm");
	fmt_num(b, 128, 78, 0, L"%");
	fail += check(b, L"78%", "en percent");
	fmt_num(b, 128, 0.55, 2, L"m");
	fail += check(b, L"0.55 m", "en metres");
	fmt_num(b, 128, -0.001, 0, L"cm");
	fail += check(b, L"0 cm", "en zero");
	fmt_num(b, 128, 13, 0, L"°");
	fail += check(b, L"13°", "en degrees");
	fmt_hms(b, 128, 4987.5);
	fail += check(b, L"1:23:07", "uptime");
	fmt_clock(b, 128, 161);
	fail += check(b, L"2:41", "media clock");
	g_cfg.lang = LANG_FR;
	fmt_date_wd(b, 128, &t, 0);
	fail += check(b, L"Vendredi 23 Oct", "fr date");
	fmt_time(b, 128, &t, true);
	fail += check(b, L"21:47", "fr 24 h time");
	fmt_num(b, 128, 78, 0, L"%");
	fail += check(b, L"78 %", "fr percent");
	fmt_num(b, 128, 0.55, 2, L"m");
	fail += check(b, L"0,55 m", "fr metres");
	fail += check(tr(S_W_LEFT), L"Gauche", "fr watch label");
	g_cfg.lang = saved;
	return fail;
}
#endif
