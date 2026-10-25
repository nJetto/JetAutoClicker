/* Click injection through the wlr-virtual-pointer protocol.
 *
 * niri, sway and other wlroots-based compositors implement
 * zwlr_virtual_pointer_manager_v1.  Events injected this way enter the
 * compositor above libinput, so the ~25 ms button debouncing that affects
 * uinput devices does not apply and high CPS works.  No extra permissions are
 * needed.
 *
 * The module opens its own Wayland connection, so it does not depend on GDK.
 * wayland_init()/wayland_close() must be called from the main thread;
 * wayland_inject() is thread-safe and may be called from the clicker thread. */
#ifndef JET_WAYLAND_H
#define JET_WAYLAND_H

#include <stdbool.h>

bool        wayland_init(void);
void        wayland_close(void);
bool        wayland_ready(void);
const char *wayland_status(void);
void        wayland_inject(int btn_code, int state);   /* state: 1 = press, 0 = release */

#endif
