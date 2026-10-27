#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <objbase.h>
#include <winsock2.h>
#include <iphlpapi.h>

#include "audio.h"
#include "gfx.h"
#include "main.h"
#include "notif.h"
#include "udp.h"

struct ctx {
	HANDLE stop;
	SOCKET sk;
	int port;
	struct notif n;
	char buf[65536];
};

static SRWLOCK lk = SRWLOCK_INIT;
static struct worker wk;
static struct ctx *cx;
static bool wsa, want;
static int port_cfg;
static struct udp_status st;
static const char tags[] = " b i u s sub sup color size material quad mark br align alpha cspace font font-weight gradient indent line-height line-indent link lowercase uppercase smallcaps allcaps margin mspace nobr noparse page pos rotate space sprite strikethrough style voffset width ";

static const char *ws(const char *p, const char *e) {
	while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
		p++;
	}
	return p;
}

static const char *jnum(const char *p, const char *e, double *v) {
	double s = 1, r = 0, f = 1;
	int x = 0, xs = 1;
	const char *b;
	if (p < e && *p == '-') {
		s = -1;
		p++;
	}
	b = p;
	for (; p < e && *p >= '0' && *p <= '9'; p++) {
		r = r * 10 + (*p - '0');
	}
	if (p < e && *p == '.') {
		for (p++; p < e && *p >= '0' && *p <= '9'; p++) {
			r += (*p - '0') * (f /= 10);
		}
	}
	if (p == b) return NULL;
	if (p < e && (*p | 32) == 'e') {
		if (++p < e && (*p == '-' || *p == '+')) {
			xs = *p++ == '-' ? -1 : 1;
		}
		for (; p < e && *p >= '0' && *p <= '9'; p++) {
			x = x < 300 ? x * 10 + (*p - '0') : 300;
		}
	}
	*v = s * r * pow(10, xs * (x > 300 ? 300 : x));
	return p;
}

static const char *jlit(const char *p, const char *e, const char *w) {
	size_t n = strlen(w);
	return (size_t)(e - p) >= n && !memcmp(p, w, n) ? p + n : NULL;
}

static const char *jval(const char *p, const char *e, int depth) {
	double d;
	char close;
	p = ws(p, e);
	if (p >= e || depth > 32) return NULL;
	if (*p == '"') return json_str(p, e, NULL, NULL, 0);
	if (*p == 't') return jlit(p, e, "true");
	if (*p == 'f') return jlit(p, e, "false");
	if (*p == 'n') return jlit(p, e, "null");
	if (*p != '{' && *p != '[') return jnum(p, e, &d);
	close = *p == '{' ? '}' : ']';
	p = ws(p + 1, e);
	while (p && p < e && *p != close) {
		if (close == '}') {
			p = json_str(p, e, NULL, NULL, 0);
			p = p ? ws(p, e) : NULL;
			if (!p || p >= e || *p++ != ':') return NULL;
		}
		p = jval(p, e, depth + 1);
		p = p ? ws(p, e) : NULL;
		if (p && p < e && *p == ',') {
			p = ws(p + 1, e);
		} else if (p && p < e && *p != close) {
			return NULL;
		}
	}
	return p && p < e ? p + 1 : NULL;
}

static const WCHAR *tag(const WCHAR *p) {
	char n[24] = " ";
	int k = 1;
	const WCHAR *q = p + 1 + (p[1] == L'/');
	if (*q != L'#') {
		for (; k < 21 && (((*q | 32) >= L'a' && (*q | 32) <= L'z') || *q == L'-'); q++) {
			n[k++] = (char)(*q | 32);
		}
		n[k] = ' ';
		n[k + 1] = 0;
		if (k < 2 || !strstr(tags, n) || (*q != L'>' && *q != L'=' && *q != L' ')) return NULL;
	}
	while (*q && *q != L'>' && *q != L'<' && *q != L'\n' && q - p < 128) {
		q++;
	}
	return *q == L'>' ? q : NULL;
}

static void clean(WCHAR *s, bool lines) {
	WCHAR *d = s, nl = lines ? L'\n' : L' ', c;
	const WCHAR *p, *q;
	for (p = s; *p; p++) {
		c = *p;
		q = c == L'<' ? tag(p) : NULL;
		if (q) {
			if (p[1] != L'/' && (p[1] | 32) == L'b' && (p[2] | 32) == L'r' && (p[3] == L'>' || p[3] == L' ' || p[3] == L'=') && (lines || (d > s && d[-1] != L' '))) {
				*d++ = nl;
			}
			p = q;
		} else if (c != L'\r') {
			*d++ = c == L'\n' || c == 0x85 || c == 0x2028 || c == 0x2029 ? nl : c < 32 || (c >= 0x7F && c <= 0x9F) ? L' ' : c;
		}
	}
	*d = 0;
}

static int b64_decode(const char *in, int len, uint8_t *out, int cap) {
	uint32_t acc = 0;
	int bits = 0, n = 0, i, c, v;
	for (i = 0; i < len && in[i] != '='; i++) {
		c = in[i];
		v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
		if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
		if (v < 0) return -1;
		acc = acc << 6 | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (n >= cap) return -1;
			out[n++] = (uint8_t)(acc >> bits);
		}
	}
	return n;
}

static int b64_icon(const char *icon, const char *e, uint8_t *img) {
	char buf[65536];
	const char *s;
	int n;
	if (!json_str(icon, e, NULL, buf, sizeof(buf))) return NI_DEFAULT;
	s = strncmp(buf, "data:", 5) ? buf : strchr(buf, ',');
	s = s ? s + (s != buf) : buf;
	n = b64_decode(s, (int)strlen(s), (uint8_t *)buf, sizeof(buf));
	return n > 0 && img_decode_mem(buf, (size_t)n, img) ? NI_IMAGE : NI_DEFAULT;
}

static int path_icon(const char *icon, const char *e, uint8_t *img) {
	WCHAR w[MAX_PATH];
	if (!json_str(icon, e, w, NULL, MAX_PATH)) return NI_DEFAULT;
	if (!_wcsicmp(w, L"warning")) return NI_WARNING;
	if (!_wcsicmp(w, L"error")) return NI_ERROR;
	return img_local(w) && img_decode_file(w, img) ? NI_IMAGE : NI_DEFAULT;
}

static bool udp_parse(const char *json, int len, struct notif *out) {
	const char *p = json, *e = json + (len > 0 ? len : 0), *icon = NULL;
	double type = 1, v = 0;
	bool b64 = false;
	char k[24];
	WCHAR *dst, a[16];
	float *num;
	notif_blank(out, NO_UDP);
	out->timeout = 0.5f;
	out->volume = 0.7f;
	if (e - p >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) {
		p += 3;
	}
	p = ws(p, e);
	if (p >= e || *p++ != '{') return false;
	p = ws(p, e);
	while (p < e && *p != '}') {
		p = json_str(p, e, NULL, k, sizeof(k));
		p = p ? ws(p, e) : NULL;
		if (!p || p >= e || *p++ != ':') return false;
		p = ws(p, e);
		if (p >= e) return false;
		dst = !strcmp(k, "title") ? out->title : !strcmp(k, "content") ? out->body : !strcmp(k, "sourceApp") ? out->source : NULL;
		num = !strcmp(k, "timeout") ? &out->timeout : !strcmp(k, "volume") ? &out->volume : !strcmp(k, "opacity") ? &out->opacity : NULL;
		if (*p == '"' && dst) {
			p = json_str(p, e, dst, NULL, dst == out->title ? ARRAYSIZE(out->title) : dst == out->body ? ARRAYSIZE(out->body) : ARRAYSIZE(out->source));
		} else if (*p == '"' && !strcmp(k, "audioPath")) {
			memset(a, 0, sizeof(a));
			p = json_str(p, e, a, NULL, ARRAYSIZE(a));
			out->sfx = !a[0] ? SFX_NONE : !_wcsicmp(a, L"warning") || !_wcsicmp(a, L"error") ? SFX_ALERT : SFX_NOTIFY;
		} else if (*p == '"' && !strcmp(k, "icon")) {
			icon = p;
			p = json_str(p, e, NULL, NULL, 0);
		} else if ((*p == 't' || *p == 'f') && !strcmp(k, "useBase64Icon")) {
			b64 = *p == 't';
			p = jval(p, e, 0);
		} else if ((*p == '-' || (*p >= '0' && *p <= '9')) && (num || !strcmp(k, "messageType"))) {
			p = jnum(p, e, &v);
			if (num) {
				*num = (float)v;
			} else {
				type = v;
			}
		} else {
			p = jval(p, e, 0);
		}
		p = p ? ws(p, e) : NULL;
		if (!p) return false;
		if (p < e && *p == ',') {
			p = ws(p + 1, e);
		} else if (p < e && *p != '}') {
			return false;
		}
	}
	if (p >= e || type != 1) return false;
	clean(out->title, false);
	clean(out->body, true);
	clean(out->source, false);
	if (!wcscmp(out->source, out->title)) {
		out->source[0] = 0;
	}
	if (!out->title[wcsspn(out->title, L" \n")] && !out->body[wcsspn(out->body, L" \n")]) return false;
	out->volume = fminf(fmaxf(out->volume, 0), 1);
	out->opacity = out->opacity > 0 ? fminf(out->opacity, 1) : 1;
	if (icon && notif_has_room()) {
		out->icon = b64 ? b64_icon(icon, e, out->img) : path_icon(icon, e, out->img);
	}
	return true;
}

static bool status(int state, const WCHAR *owner) {
	static const char *const names[] = { "off", "starting", "listening on 127.0.0.1", "in use by ", "error" };
	char u[200];
	bool ch;
	AcquireSRWLockExclusive(&lk);
	ch = st.state != state || wcscmp(st.owner, owner);
	st.state = state;
	st.port = port_cfg;
	put(st.owner, ARRAYSIZE(st.owner), owner);
	ReleaseSRWLockExclusive(&lk);
	if (!ch) return false;
	to_utf8(owner, u, sizeof(u));
	glog("udp %d: %s%s", port_cfg, names[state], u);
	app_wake();
	return true;
}

static bool port_owner(int port, WCHAR *name, int cap) {
	static union { MIB_UDPTABLE_OWNER_PID v4; MIB_UDP6TABLE_OWNER_PID v6; uint8_t b[65536]; } t;
	DWORD pid = 0, n = MAX_PATH, sz, i;
	WCHAR path[MAX_PATH];
	HANDLE h;
	int f;
	for (f = 0; f < 2 && !pid; f++) {
		sz = sizeof(t);
		if (GetExtendedUdpTable(&t, &sz, FALSE, f ? AF_INET6 : AF_INET, UDP_TABLE_OWNER_PID, 0) != NO_ERROR) continue;
		for (i = 0; i < t.v4.dwNumEntries && !pid; i++) {
			if (ntohs((u_short)(f ? t.v6.table[i].dwLocalPort : t.v4.table[i].dwLocalPort)) == port) {
				pid = f ? t.v6.table[i].dwOwningPid : t.v4.table[i].dwOwningPid;
			}
		}
	}
	if (!pid) return false;
	h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (h && QueryFullProcessImageNameW(h, 0, path, &n)) {
		app_name(path, name, cap);
	} else {
		swprintf(name, cap, L"PID %lu", pid);
	}
	if (h) {
		CloseHandle(h);
	}
	return true;
}

static void receive(struct ctx *c, SOCKET s) {
	int r;
	bool ok;
	for (;;) {
		r = recvfrom(s, c->buf, sizeof(c->buf), 0, NULL, NULL);
		if (r < 0 && (WSAGetLastError() == WSAEMSGSIZE || WSAGetLastError() == WSAECONNRESET)) continue;
		if (r < 0) return;
		ok = udp_parse(c->buf, r, &c->n);
		if (WaitForSingleObject(c->stop, 0) != WAIT_TIMEOUT) return;
		if (ok) {
			notif_push(&c->n);
		}
	}
}

static DWORD WINAPI run(void *arg) {
	struct ctx *c = arg;
	WCHAR owner[64];
	BOOL one = TRUE;
	int rb = 1 << 20, err;
	struct sockaddr_in a;
	SOCKET s;
	bool go, mine;
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	do {
		memset(owner, 0, sizeof(owner));
		if (port_owner(c->port, owner, ARRAYSIZE(owner))) {
			status(UDP_IN_USE, owner);
			continue;
		}
		a = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons((u_short)c->port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
		s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (s == INVALID_SOCKET || setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof(one)) || bind(s, (struct sockaddr *)&a, sizeof(a))) {
			err = WSAGetLastError();
			if (s != INVALID_SOCKET) {
				closesocket(s);
			}
			if (status(port_owner(c->port, owner, ARRAYSIZE(owner)) ? UDP_IN_USE : UDP_ERROR, owner)) {
				glog("udp: bind 127.0.0.1:%d failed (%d)", c->port, err);
			}
			continue;
		}
		setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rb, sizeof(rb));
		AcquireSRWLockExclusive(&lk);
		go = WaitForSingleObject(c->stop, 0) == WAIT_TIMEOUT;
		c->sk = go ? s : INVALID_SOCKET;
		ReleaseSRWLockExclusive(&lk);
		if (!go) {
			closesocket(s);
			break;
		}
		status(UDP_LISTENING, L"");
		receive(c, s);
		AcquireSRWLockExclusive(&lk);
		mine = c->sk == s;
		c->sk = INVALID_SOCKET;
		ReleaseSRWLockExclusive(&lk);
		if (mine) {
			closesocket(s);
			status(UDP_ERROR, L"");
		}
	} while (WaitForSingleObject(c->stop, 10000) == WAIT_TIMEOUT);
	CoUninitialize();
	return 0;
}

static void stop(DWORD ms) {
	if (!wk.th) return;
	AcquireSRWLockExclusive(&lk);
	SetEvent(cx->stop);
	if (cx->sk != INVALID_SOCKET) {
		closesocket(cx->sk);
		cx->sk = INVALID_SOCKET;
	}
	ReleaseSRWLockExclusive(&lk);
	if (worker_stop(&wk, ms, "udp")) {
		CloseHandle(cx->stop);
		free(cx);
	}
	cx = NULL;
}

void udp_init(void) {
	WSADATA d;
	if (!wsa) {
		wsa = !WSAStartup(MAKEWORD(2, 2), &d);
	}
	port_cfg = 0;
	status(wsa ? UDP_OFF : UDP_ERROR, L"");
}

void udp_config(bool on, int port) {
	struct ctx *c;
	HANDLE ev;
	if (!wsa || (on == want && port == port_cfg)) return;
	stop(500);
	want = on;
	port_cfg = port;
	if (!on) {
		status(UDP_OFF, L"");
		return;
	}
	status(UDP_STARTING, L"");
	c = calloc(1, sizeof(*c));
	ev = c ? CreateEventW(NULL, TRUE, FALSE, NULL) : NULL;
	if (ev) {
		c->stop = ev;
		c->sk = INVALID_SOCKET;
		c->port = port;
	}
	if (worker_start(&wk, ev, run, c)) {
		cx = c;
		return;
	}
	if (ev) {
		CloseHandle(ev);
	}
	free(c);
	status(UDP_ERROR, L"");
}

void udp_status(struct udp_status *s) {
	AcquireSRWLockShared(&lk);
	*s = st;
	ReleaseSRWLockShared(&lk);
}

void udp_shutdown(void) {
	stop(2000);
	want = false;
	status(UDP_OFF, L"");
	if (wsa && !wk.stuck) {
		WSACleanup();
	}
	wsa = false;
}

#ifdef GL_TESTS
static volatile LONG got;
static WCHAR got_title[128];

static bool capture(const struct notif *n) {
	put(got_title, ARRAYSIZE(got_title), n->title);
	InterlockedIncrement(&got);
	app_wake();
	return true;
}

static int ck(bool ok, const char *what) {
	return st_ck(ok, "udp", what);
}

static bool parse(const char *j, struct notif *n) {
	return udp_parse(j, (int)strlen(j), n);
}

static bool wait_state(int state, struct udp_status *s) {
	double end;
	for (end = now_s() + 3; now_s() < end;) {
		udp_status(s);
		if (s->state == state) return true;
		WaitForSingleObject(g_app.wake, 50);
	}
	return false;
}

static int file_icon(struct notif *n) {
	static const char png[] = "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEUlEQVR4nGP4z8AAQv8ZYAwAQ84H+VjtZqAAAAAASUVORK5CYII=";
	WCHAR p[MAX_PATH];
	char u[MAX_PATH * 3], j[MAX_PATH * 3 + 64], *c;
	uint8_t b[128];
	DWORD k = GetTempPathW(MAX_PATH - 40, p), wr = 0;
	int len = b64_decode(png, (int)strlen(png), b, sizeof(b)), f = 0;
	HANDLE h;
	bool w;
	swprintf(p + k, MAX_PATH - k, L"ghostlay-icon-%lu.png", GetCurrentProcessId());
	h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
	w = h != INVALID_HANDLE_VALUE && len > 0 && WriteFile(h, b, (DWORD)len, &wr, NULL) && wr == (DWORD)len;
	if (h != INVALID_HANDLE_VALUE) {
		CloseHandle(h);
	}
	to_utf8(p, u, sizeof(u));
	for (c = u; *c; c++) {
		*c = *c == '\\' ? '/' : *c;
	}
	snprintf(j, sizeof(j), "{\"title\":\"x\",\"icon\":\"%s\"}", u);
	f += ck(w && parse(j, n) && n->icon == NI_IMAGE && n->img[3] == 255, "file icon");
	DeleteFileW(p);
	f += ck(parse(j, n) && n->icon == NI_DEFAULT, "missing file icon");
	*strrchr(j, '/') = 0;
	strcat(j, "\"}");
	f += ck(parse(j, n) && n->icon == NI_DEFAULT, "directory icon");
	return f;
}

static int free_port(SOCKET s) {
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	int len = sizeof(a);
	if (s == INVALID_SOCKET || bind(s, (struct sockaddr *)&a, len) || getsockname(s, (struct sockaddr *)&a, &len)) return 0;
	return ntohs(a.sin_port);
}

static int loopback(void) {
	static const char junk[] = "{\"title\":", msg[] = "{\"messageType\":1,\"title\":\"Loopback \\u00e9\",\"content\":\"ok\"}";
	struct udp_status s;
	struct sockaddr_in a;
	SOCKET c, busy, tmp;
	int f = 0, port, held;
	double end;
	udp_init();
	c = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	busy = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	tmp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	port = free_port(tmp);
	held = free_port(busy);
	if (tmp != INVALID_SOCKET) {
		closesocket(tmp);
	}
	f += ck(port && held && c != INVALID_SOCKET, "loopback sockets");
	notif_tap = capture;
	got = 0;
	udp_config(true, port);
	f += ck(wait_state(UDP_LISTENING, &s) && s.port == port, "listening state");
	a = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons((u_short)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	sendto(c, junk, (int)strlen(junk), 0, (struct sockaddr *)&a, sizeof(a));
	sendto(c, msg, (int)strlen(msg), 0, (struct sockaddr *)&a, sizeof(a));
	for (end = now_s() + 3; got < 1 && now_s() < end;) {
		WaitForSingleObject(g_app.wake, 50);
	}
	f += ck(got == 1 && !wcscmp(got_title, L"Loopback \u00e9"), "loopback datagram");
	udp_config(true, held);
	f += ck(wait_state(UDP_IN_USE, &s) && s.port == held && s.owner[0], "port in use detection");
	udp_config(false, held);
	f += ck(wait_state(UDP_OFF, &s), "disable");
	notif_tap = NULL;
	closesocket(c);
	closesocket(busy);
	udp_shutdown();
	return f;
}

int udp_selftest(void) {
	static struct notif n;
	static const WCHAR bad[] = { L'a', 0xFFFD, L'b', 0xFFFD, L'c', 0 };
	char big[1100];
	uint8_t b[16];
	int f = 0;
	f += ck(parse("{\"messageType\":1,\"index\":0,\"timeout\":3.5,\"height\":175.0,\"opacity\":0,\"volume\":0.25,\"audioPath\":\"\",\"title\":\"Hello\",\"content\":\"World\",\"useBase64Icon\":false,\"icon\":\"warning\",\"sourceApp\":\"VRCX\"}", &n) && !wcscmp(n.title, L"Hello") && !wcscmp(n.body, L"World") && !wcscmp(n.source, L"VRCX") && n.timeout == 3.5f && fabsf(n.volume - 0.25f) < 1e-6f && n.opacity == 1 && n.icon == NI_WARNING && n.sfx == SFX_NONE && n.bat_pct == -1, "xsoverlay fields");
	f += ck(parse("{\"title\":\"VRCX\",\"content\":\"Mihoko Okayami has left\",\"sourceApp\":\"VRCX\"}", &n) && !wcscmp(n.title, L"VRCX") && !n.source[0], "source equal to the title is dropped");
	f += ck(parse(" {\"title\":\"Caf\\u00e9 \\ud83d\\ude00 \\\"q\\\" \\\\ \\/\",\"content\":\"\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80\\r\\nline2\\tend\"} ", &n) && !wcscmp(n.title, L"Caf\u00e9 \U0001F600 \"q\" \\ /") && !wcscmp(n.body, L"\u00e9\u4e2d\U0001F600\nline2 end") && n.icon == NI_DEFAULT && n.sfx == SFX_NOTIFY && n.timeout == 0.5f && fabsf(n.volume - 0.7f) < 1e-6f && !n.source[0], "escapes, utf-8 and defaults");
	f += ck(parse("{\"title\":\"a\\ud800b\xFF" "c\"}", &n) && !wcscmp(n.title, bad), "invalid surrogate and utf-8");
	f += ck(!parse("{\"messageType\":2,\"title\":\"x\"}", &n) && parse("\xEF\xBB\xBF{\"messageType\":1.0,\"title\":\"bom\"}", &n) && !wcscmp(n.title, L"bom"), "messageType and byte order mark");
	f += ck(parse("{\"title\":\"<b>Hi</b> <color=#FF0000>there</color><#00FF00>!\",\"content\":\"1 < 2 <3 <size=20>big</size> <Mihoko> a<b\"}", &n) && !wcscmp(n.title, L"Hi there!") && !wcscmp(n.body, L"1 < 2 <3 big <Mihoko> a<b"), "rich text");
	f += ck(parse("{\"title\":\"A\\u2028B<br>C<BR>\",\"content\":\"x<br>y\\u0085z\\u2029<br=1>w\\u0090!\",\"sourceApp\":\"S<br>\\u0085T\"}", &n) && !wcscmp(n.title, L"A B C ") && !wcscmp(n.body, L"x\ny\nz\n\nw !") && !wcscmp(n.source, L"S  T"), "line breaks");
	f += ck(!parse("{\"title\":\"x\"", &n) && !parse("{\"title\":\"x\" \"content\":\"y\"}", &n) && !parse("[1]", &n) && !parse("", &n) && !parse("{\"title\":\"x\\q\"}", &n), "malformed json");
	f += ck(parse("{\"a\":{\"b\":[1,-2.5e3,true,null,{\"c\":\"}\"}],\"d\":{}},\"e\":[],\"title\":\"T\",\"volume\":2,\"timeout\":-1e1,\"opacity\":0.5,\"sourceApp\":null}", &n) && !wcscmp(n.title, L"T") && n.volume == 1 && fabsf(n.timeout + 10) < 1e-4f && n.opacity == 0.5f, "nested values and clamps");
	f += ck(!parse("{\"title\":\"<b></b>\",\"content\":\" \\n \"}", &n), "blank notification rejected");
	f += ck(parse("{\"title\":\"x\",\"icon\":\"\\\\\\\\server\\\\share\\\\a.png\"}", &n) && n.icon == NI_DEFAULT && parse("{\"title\":\"x\",\"icon\":\"ERROR\",\"audioPath\":\"warning\"}", &n) && n.icon == NI_ERROR && n.sfx == SFX_ALERT && parse("{\"title\":\"x\",\"audioPath\":\"C:\\\\a.ogg\"}", &n) && n.sfx == SFX_NOTIFY, "icon names, unc paths and audio");
	snprintf(big, sizeof(big), "{\"title\":\"%0300d\",\"content\":\"%0600d\",\"sourceApp\":\"%070d\"}", 0, 0, 0);
	f += ck(parse(big, &n) && wcslen(n.title) == 127 && wcslen(n.body) == 511 && wcslen(n.source) == 63, "truncation");
	snprintf(big, sizeof(big), "{\"title\":\"%0126d\\ud83d\\ude00\"}", 0);
	f += ck(parse(big, &n) && wcslen(n.title) == 126, "surrogate pair kept whole");
	f += ck(parse("{\"title\":\"x\",\"icon\":\"data:image\\/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEUlEQVR4nGP4z8AAQv8ZYAwAQ84H+VjtZqAAAAAASUVORK5CYII=\",\"useBase64Icon\":true}", &n) && n.icon == NI_IMAGE && n.img[3] == 255, "base64 png icon");
	f += ck(parse("{\"title\":\"x\",\"icon\":\"R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7\",\"useBase64Icon\":true}", &n) && n.icon == NI_DEFAULT, "gif icon refused, only png and jpeg");
	f += ck(parse("{\"title\":\"x\",\"useBase64Icon\":true,\"icon\":\"!!!\"}", &n) && n.icon == NI_DEFAULT, "bad base64 icon");
	f += ck(b64_decode("SGVsbG8=", 8, b, 16) == 5 && !memcmp(b, "Hello", 5) && b64_decode("SGVs\r\nbG8", 9, b, 16) == 5 && b64_decode("SGV*", 4, b, 16) == -1 && b64_decode("SGVsbG8=", 8, b, 3) == -1 && !b64_decode("", 0, b, 16), "base64");
	return f + file_icon(&n) + loopback();
}
#endif
