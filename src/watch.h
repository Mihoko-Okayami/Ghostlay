#ifndef WATCH_H
#define WATCH_H

#include <stdbool.h>
#include <windows.h>

void watch_init(void);
void watch_shutdown(void);
bool watch_update(double dt);

#ifdef GL_TESTS
int watch_render_samples(const WCHAR *dir);
int watch_selftest(void);
#endif

#endif
