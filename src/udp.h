#ifndef UDP_H
#define UDP_H

#include <stdbool.h>
#include <windows.h>

enum { UDP_OFF, UDP_STARTING, UDP_LISTENING, UDP_IN_USE, UDP_ERROR };

struct udp_status {
	int state, port;
	WCHAR owner[64];
};

void udp_init(void);
void udp_config(bool on, int port);
void udp_status(struct udp_status *s);
void udp_shutdown(void);

#ifdef GL_TESTS
int udp_selftest(void);
#endif

#endif
