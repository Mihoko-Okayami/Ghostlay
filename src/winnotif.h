#ifndef WINNOTIF_H
#define WINNOTIF_H

#include <stdbool.h>
#include <windows.h>

enum { WN_OFF, WN_STARTING, WN_ACTIVE, WN_BLOCKED, WN_UNAVAILABLE };

void winnotif_init(void);
void winnotif_config(bool on);
int winnotif_status(void);
void winnotif_shutdown(void);
int winnotif_toasts(int secs);

#ifdef GL_TESTS
int winnotif_selftest(void);
int winnotif_render_samples(const WCHAR *dir);
#endif

#endif
