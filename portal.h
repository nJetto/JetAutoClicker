/* Click injection through xdg-desktop-portal RemoteDesktop (KWin, GNOME, ...).
 * Events enter the compositor above libinput, so libinput's 25 ms button
 * debouncing does not apply and high CPS works. Needs a running GLib main loop
 * (the GTK loop) for the asynchronous setup; portal_inject() is thread-safe. */
#ifndef JET_PORTAL_H
#define JET_PORTAL_H

#include <stdbool.h>

typedef void (*PortalStateFn)(void *user);   /* called on the main thread */

void        portal_init(PortalStateFn on_state, void *user);
bool        portal_ready(void);
const char *portal_status(void);             /* main thread only */
void        portal_inject(int btn_code, int state);   /* state: 1 = press, 0 = release */
void        portal_close(void);

#endif
