#ifndef PANEL_H
#define PANEL_H

#include <stdbool.h>
#include <windows.h>

void panel_init(void);
void panel_shutdown(void);
void panel_toggle(void);
bool panel_update(void);

#ifdef GL_TESTS
int panel_render_samples(const WCHAR *dir);
#endif

#endif
