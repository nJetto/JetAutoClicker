/* JetAutoClicker engine: uinput virtual mouse + evdev hotkey listener.
 * No GUI dependencies. Callbacks fire from engine threads. */
#ifndef JET_ENGINE_H
#define JET_ENGINE_H

#include <stdbool.h>
#include <stddef.h>

/* Where clicks are sent: an external injector (xdg-desktop-portal), or the
 * kernel uinput virtual mouse (subject to libinput's 25 ms button debouncing,
 * so the button looks "held" above roughly 35 CPS on KWin/GNOME/etc). */
#define ENGINE_BACKEND_EXTERNAL 0
#define ENGINE_BACKEND_UINPUT   1

#define MIN_CPS 0.1
#define MAX_CPS 1000.0

/* Called (from an engine thread) when running state or device list changed. */
typedef void (*EngineChangeFn)(void *user);
/* Called (from an engine thread) when hotkey capture finished. */
typedef void (*EngineCaptureFn)(int key, bool cancelled, void *user);

/* Starts worker threads. Returns true if the virtual mouse could be created.
 * If false, engine_error() explains why (hotkey capture still works). */
bool        engine_init(EngineChangeFn on_change, EngineCaptureFn on_capture, void *user);
void        engine_shutdown(void);
const char *engine_error(void);

typedef void (*EngineInjectFn)(int btn_code, int state);   /* state: 1 press, 0 release */
void   engine_set_external(EngineInjectFn fn);
void   engine_set_external_ready(bool ready);
void   engine_set_backend(int backend);
bool   engine_backend_ready(void);
bool   engine_uinput_ok(void);

void   engine_set_cps(double cps);          /* clamped to [MIN_CPS, MAX_CPS] */
void   engine_set_button(int btn_code);     /* BTN_LEFT / BTN_RIGHT / BTN_MIDDLE */
void   engine_set_hold_mode(bool hold);     /* false = toggle, true = hold */
/* The hotkey is always a single key. Modifier keys (Ctrl/Alt/Shift/Meta) are
 * ignored: the hotkey fires no matter which modifiers are held at the moment,
 * so e.g. sprinting with Ctrl held does not block it. */
void   engine_set_hotkey(int key);
void   engine_get_hotkey(int *key);

void   engine_start_capture(void);          /* next non-modifier key press becomes the hotkey */
void   engine_cancel_capture(void);
bool   engine_is_capturing(void);

void   engine_set_running(bool on);
bool   engine_is_running(void);
unsigned long long engine_click_count(void);
int    engine_hotkey_device_count(void);

void   engine_hotkey_name(int key, char *out, size_t n);

#endif
