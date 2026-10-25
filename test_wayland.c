/* Connection check for the wlr-virtual-pointer backend.
 * Builds and runs on a niri/sway session; it does not inject any clicks. */
#include "wayland.h"
#include <stdio.h>

int main(void)
{
    bool ok = wayland_init();
    printf("%s\n", wayland_status());
    wayland_close();
    return ok ? 0 : 1;
}
