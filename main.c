/*
 * JetAutoClicker - a GTK3 autoclicker for Linux (Wayland / X11)
 *
 * Clicks are injected through the wlr virtual pointer (niri/sway), the
 * xdg-desktop-portal RemoteDesktop interface (KWin/GNOME), or a kernel uinput
 * virtual mouse. The global hotkey is read straight from evdev
 * (/dev/input/event*), because Wayland does not allow normal applications to
 * grab global keys.
 */
#include <gtk/gtk.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "portal.h"
#include "wayland.h"

#define APP_NAME "JetAutoClicker"

#ifndef APP_VERSION
#define APP_VERSION "unknown"
#endif

enum { RATE_CPS = 0, RATE_INTERVAL = 1 };

/* Values stored in the "Click method" combo box and in config.ini.  The first
 * two values used to mean Portal and uinput; configs from older versions are
 * migrated in load_config(). */
enum { BE_AUTO = 0, BE_PORTAL = 1, BE_WAYLAND = 2, BE_UINPUT = 3 };

static GtkWidget *win, *cb_mode, *spin, *lbl_info, *cb_btn, *cb_act, *cb_backend;
static GtkWidget *btn_hk, *btn_run, *lbl_status, *lbl_actual, *lbl_hint;

static gboolean ui_lock = FALSE;           /* suppress callbacks while we set widgets */
static gboolean uinput_ok = FALSE;
static gboolean wayland_ok = FALSE;
static gboolean portal_started = FALSE;
static int      cur_mode = RATE_CPS;
static gint64   last_capture_us = 0;
static gint64   last_tick_us = 0;
static unsigned long long last_clicks = 0;

/* settings loaded from config (applied when the UI is built) */
static double cfg_value = 10.0;
static int    cfg_btn = 0, cfg_act = 0, cfg_backend = BE_AUTO;

static const int BUTTONS[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };

/* ------------------------------------------------------------------ */
/* config                                                             */
/* ------------------------------------------------------------------ */

static char *config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "jetautoclicker", "config.ini", NULL);
}

static int kf_int(GKeyFile *kf, const char *k, int def)
{
    GError *e = NULL;
    int v = g_key_file_get_integer(kf, "main", k, &e);
    if (e) { g_error_free(e); return def; }
    return v;
}

static double kf_dbl(GKeyFile *kf, const char *k, double def)
{
    GError *e = NULL;
    double v = g_key_file_get_double(kf, "main", k, &e);
    if (e) { g_error_free(e); return def; }
    return v;
}

static void load_config(void)
{
    char *path = config_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        cur_mode  = kf_int(kf, "rate_mode", RATE_CPS) == RATE_INTERVAL ? RATE_INTERVAL : RATE_CPS;
        cfg_value = kf_dbl(kf, "rate_value", 10.0);
        cfg_btn   = kf_int(kf, "button", 0);
        cfg_act   = kf_int(kf, "hold_mode", 0);
        if (g_key_file_has_key(kf, "main", "backend_mode", NULL)) {
            cfg_backend = kf_int(kf, "backend_mode", BE_AUTO);
        } else {
            /* 1.0.x configs: 0 = Portal, 1 = uinput.  Portal was the old
             * default, so map it to Auto: KDE/GNOME still pick the portal,
             * while niri/sway now pick the working virtual pointer. */
            cfg_backend = kf_int(kf, "backend", ENGINE_BACKEND_EXTERNAL) == ENGINE_BACKEND_UINPUT
                          ? BE_UINPUT : BE_AUTO;
        }
        if (cfg_backend < BE_AUTO || cfg_backend > BE_UINPUT) cfg_backend = BE_AUTO;
        int key   = kf_int(kf, "hotkey", KEY_F6);
        if (cfg_btn < 0 || cfg_btn > 2) cfg_btn = 0;
        if (cfg_act < 0 || cfg_act > 1) cfg_act = 0;
        if (key > 0 && key < KEY_MAX)
            engine_set_hotkey(key);        /* single key; any old modifiers in the config are dropped */
    }
    g_key_file_free(kf);
    g_free(path);
}

static void save_config(void)
{
    char *path = config_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);

    int key;
    engine_get_hotkey(&key);
    int backend = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_backend));
    if (backend < BE_AUTO || backend > BE_UINPUT) backend = BE_AUTO;

    GKeyFile *kf = g_key_file_new();
    g_key_file_set_integer(kf, "main", "rate_mode", cur_mode);
    g_key_file_set_double (kf, "main", "rate_value", gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin)));
    g_key_file_set_integer(kf, "main", "button", gtk_combo_box_get_active(GTK_COMBO_BOX(cb_btn)));
    g_key_file_set_integer(kf, "main", "hold_mode", gtk_combo_box_get_active(GTK_COMBO_BOX(cb_act)));
    g_key_file_set_integer(kf, "main", "backend_mode", backend);
    /* Keep the old key so that 1.0.x can still read the file. */
    g_key_file_set_integer(kf, "main", "backend",
                           backend == BE_UINPUT ? ENGINE_BACKEND_UINPUT : ENGINE_BACKEND_EXTERNAL);
    g_key_file_set_integer(kf, "main", "hotkey", key);

    gsize len;
    char *data = g_key_file_to_data(kf, &len, NULL);
    g_file_set_contents(path, data, (gssize)len, NULL);

    g_free(data); g_key_file_free(kf); g_free(dir); g_free(path);
}

/* ------------------------------------------------------------------ */
/* settings -> engine                                                 */
/* ------------------------------------------------------------------ */

static void refresh_ui(void);
static void on_portal_state(void *user);

/* Whether the desktop is a wlroots compositor that may not implement the
 * RemoteDesktop portal (niri, sway, ...).  Used only by the Auto mode when
 * the wlr-virtual-pointer protocol is unavailable. */
static gboolean is_wlroots_desktop(void)
{
    const char *desk = g_getenv("XDG_CURRENT_DESKTOP");
    if (!desk)
        return FALSE;
    static const char *const names[] = {
        "niri", "sway", "Hyprland", "river", "wayfire", "labwc", "wlroots"
    };
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
        if (g_strstr_len(desk, -1, names[i]))
            return TRUE;
    return FALSE;
}

/* Translate the combo box selection into an engine backend.  Auto picks the
 * best backend that is actually available in this session. */
static int resolved_backend(void)
{
    int mode = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_backend));
    if (mode < BE_AUTO || mode > BE_UINPUT)
        mode = BE_AUTO;

    if (mode == BE_PORTAL)  return ENGINE_BACKEND_EXTERNAL;
    if (mode == BE_WAYLAND) return ENGINE_BACKEND_WAYLAND;
    if (mode == BE_UINPUT)  return ENGINE_BACKEND_UINPUT;

    if (wayland_ok)
        return ENGINE_BACKEND_WAYLAND;
    if (is_wlroots_desktop())
        return uinput_ok ? ENGINE_BACKEND_UINPUT : ENGINE_BACKEND_WAYLAND;
    if (!g_getenv("WAYLAND_DISPLAY") && !g_getenv("WAYLAND_SOCKET"))
        return uinput_ok ? ENGINE_BACKEND_UINPUT : ENGINE_BACKEND_EXTERNAL;
    return ENGINE_BACKEND_EXTERNAL;       /* KDE / GNOME: use the portal */
}

static void ensure_portal(void)
{
    if (!portal_started) {
        portal_started = TRUE;
        portal_init(on_portal_state, NULL);   /* async; may show a dialog */
    }
}

static double current_cps(void)
{
    double v = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));
    if (v <= 0) v = 1;
    double cps = (cur_mode == RATE_CPS) ? v : 1000.0 / v;
    if (cps < MIN_CPS) cps = MIN_CPS;
    if (cps > MAX_CPS) cps = MAX_CPS;
    return cps;
}

static void apply_settings(void)
{
    if (ui_lock)
        return;
    double cps = current_cps();
    engine_set_cps(cps);

    int bi = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_btn));
    if (bi < 0 || bi > 2) bi = 0;
    engine_set_button(BUTTONS[bi]);
    engine_set_hold_mode(gtk_combo_box_get_active(GTK_COMBO_BOX(cb_act)) == 1);

    int be = resolved_backend();
    if (be == ENGINE_BACKEND_EXTERNAL)
        ensure_portal();
    engine_set_backend(be);

    char buf[128];
    snprintf(buf, sizeof buf, "= %.1f CPS  |  %.3f ms between clicks", cps, 1000.0 / cps);
    gtk_label_set_text(GTK_LABEL(lbl_info), buf);
    refresh_ui();
}

static void configure_spin(int mode, double value)
{
    ui_lock = TRUE;
    if (mode == RATE_CPS) {
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(spin), MIN_CPS, MAX_CPS);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), 1);
        gtk_spin_button_set_increments(GTK_SPIN_BUTTON(spin), 1, 10);
    } else {
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(spin), 1000.0 / MAX_CPS, 1000.0 / MIN_CPS);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), 1);
        gtk_spin_button_set_increments(GTK_SPIN_BUTTON(spin), 1, 10);
    }
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
    ui_lock = FALSE;
}

/* ------------------------------------------------------------------ */
/* UI refresh                                                         */
/* ------------------------------------------------------------------ */

static void refresh_ui(void)
{
    gboolean run = engine_is_running();
    int be = resolved_backend();
    gtk_button_set_label(GTK_BUTTON(btn_run), run ? "Stop" : "Start");
    gtk_widget_set_sensitive(btn_run, engine_backend_ready());

    if (engine_is_capturing()) {
        gtk_button_set_label(GTK_BUTTON(btn_hk), "Press a key...  (Esc = cancel)");
    } else {
        char name[64]; int key;
        engine_get_hotkey(&key);
        engine_hotkey_name(key, name, sizeof name);
        gtk_button_set_label(GTK_BUTTON(btn_hk), name);
    }

    char buf[320];
    if (run) {
        snprintf(buf, sizeof buf, "Status: clicking");
    } else if (be == ENGINE_BACKEND_EXTERNAL) {
        snprintf(buf, sizeof buf, "%s", portal_status());
    } else if (be == ENGINE_BACKEND_WAYLAND) {
        snprintf(buf, sizeof buf, "%s", wayland_status());
    } else if (!uinput_ok) {
        snprintf(buf, sizeof buf, "Cannot create virtual mouse: %s", engine_error());
    } else {
        snprintf(buf, sizeof buf, "Status: idle (uinput)");
    }
    gtk_label_set_text(GTK_LABEL(lbl_status), buf);

    if (be == ENGINE_BACKEND_UINPUT && current_cps() > 30.0)
        gtk_label_set_text(GTK_LABEL(lbl_hint),
            "uinput: libinput (used by niri, KWin, ...) merges clicks closer than ~25 ms, so the "
            "button looks held down. Use a Wayland or Portal method above 30 CPS.");
    else if (be == ENGINE_BACKEND_WAYLAND && !wayland_ok)
        gtk_label_set_text(GTK_LABEL(lbl_hint),
            "This compositor does not support wlr-virtual-pointer. Choose Auto, Portal "
            "(KDE/GNOME) or uinput.");
    else if (engine_hotkey_device_count() == 0)
        gtk_label_set_text(GTK_LABEL(lbl_hint),
            "No readable keyboard found: global hotkey disabled.\n"
            "Add your user to the 'input' group (see README). The Start button still works.");
    else
        gtk_label_set_text(GTK_LABEL(lbl_hint), "");
}

static gboolean refresh_idle(gpointer d) { (void)d; refresh_ui(); return G_SOURCE_REMOVE; }

static gboolean capture_idle(gpointer d)
{
    (void)d;
    last_capture_us = g_get_monotonic_time();
    refresh_ui();
    return G_SOURCE_REMOVE;
}

/* portal state changes (main thread) */
static void on_portal_state(void *user)
{
    (void)user;
    engine_set_external_ready(portal_ready());
    if (win)
        refresh_ui();
}

/* engine callbacks (called from worker threads) */
static void on_engine_change(void *user) { (void)user; g_idle_add(refresh_idle, NULL); }
static void on_engine_capture(int key, bool cancelled, void *user)
{
    (void)key; (void)cancelled; (void)user;
    g_idle_add(capture_idle, NULL);
}

static gboolean on_tick(gpointer d)
{
    (void)d;
    gint64 now = g_get_monotonic_time();
    unsigned long long c = engine_click_count();
    char buf[96];
    if (engine_is_running() && now > last_tick_us && last_tick_us != 0) {
        double cps = (double)(c - last_clicks) * 1e6 / (double)(now - last_tick_us);
        snprintf(buf, sizeof buf, "Measured: %.1f CPS  (total clicks: %llu)", cps, c);
    } else {
        snprintf(buf, sizeof buf, "Measured: -  (total clicks: %llu)", c);
    }
    gtk_label_set_text(GTK_LABEL(lbl_actual), buf);
    last_clicks = c;
    last_tick_us = now;
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* widget callbacks                                                   */
/* ------------------------------------------------------------------ */

static void on_value_changed(GtkWidget *w, gpointer d) { (void)w; (void)d; apply_settings(); }

static void on_mode_changed(GtkComboBox *cb, gpointer d)
{
    (void)d;
    if (ui_lock)
        return;
    int m = gtk_combo_box_get_active(cb);
    if (m < 0 || m == cur_mode)
        return;
    double cps = current_cps();            /* uses the old mode */
    cur_mode = m;
    configure_spin(m, m == RATE_CPS ? cps : 1000.0 / cps);
    apply_settings();
}

static void on_run_clicked(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    engine_set_running(!engine_is_running());
    refresh_ui();
}

static void on_hotkey_clicked(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    /* The key that was just captured also reaches this window; if it was
     * Space/Enter it would re-activate this button, so ignore that. */
    if (g_get_monotonic_time() - last_capture_us < 400000)
        return;
    if (engine_hotkey_device_count() == 0)
        return;
    if (engine_is_capturing()) engine_cancel_capture();
    else                       engine_start_capture();
    refresh_ui();
}

static void on_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    save_config();
    engine_shutdown();
    wayland_close();
    if (portal_started)
        portal_close();
    gtk_main_quit();
}

/* ------------------------------------------------------------------ */
/* UI construction                                                    */
/* ------------------------------------------------------------------ */

static GtkWidget *label(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    return l;
}

static void build_ui(void)
{
    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), APP_NAME);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(win), 14);
    g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), NULL);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_add(GTK_CONTAINER(win), grid);

    cb_mode = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb_mode), "Clicks per second (CPS)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb_mode), "Interval between clicks (ms)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(cb_mode), cur_mode);

    spin = gtk_spin_button_new_with_range(MIN_CPS, MAX_CPS, 1);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
    configure_spin(cur_mode, cfg_value);

    lbl_info = label("");

    cb_btn = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb_btn), "Left");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb_btn), "Right");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb_btn), "Middle");
    gtk_combo_box_set_active(GTK_COMBO_BOX(cb_btn), cfg_btn);

    cb_act = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb_act), "Toggle (press to start / stop)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb_act), "Hold (click while key is held)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(cb_act), cfg_act);

    cb_backend = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb_backend), "Auto (recommended)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb_backend), "Portal (KDE / GNOME, any CPS)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb_backend), "Wayland virtual pointer (niri / sway)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb_backend), "uinput (up to ~30 CPS)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(cb_backend), cfg_backend);

    btn_hk  = gtk_button_new_with_label("F6");
    btn_run = gtk_button_new_with_label("Start");
    lbl_status = label("");
    lbl_actual = label("Measured: -");
    lbl_hint   = label("");
    gtk_label_set_line_wrap(GTK_LABEL(lbl_hint), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl_hint), 48);

    gtk_grid_attach(GTK_GRID(grid), label("Rate mode"),    0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), cb_mode,               1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), label("Value"),        0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), spin,                  1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lbl_info,              0, 2, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), label("Mouse button"), 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), cb_btn,                1, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), label("Click method"), 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), cb_backend,            1, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), label("Activation"),   0, 5, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), cb_act,                1, 5, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), label("Hotkey"),       0, 6, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_hk,                1, 6, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_run,               0, 7, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), lbl_status,            0, 8, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), lbl_actual,            0, 9, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), lbl_hint,              0, 10, 2, 1);

    g_signal_connect(cb_mode, "changed",       G_CALLBACK(on_mode_changed),  NULL);
    g_signal_connect(spin,    "value-changed", G_CALLBACK(on_value_changed), NULL);
    g_signal_connect(cb_btn,  "changed",       G_CALLBACK(on_value_changed), NULL);
    g_signal_connect(cb_act,  "changed",       G_CALLBACK(on_value_changed), NULL);
    g_signal_connect(cb_backend, "changed",    G_CALLBACK(on_value_changed), NULL);
    g_signal_connect(btn_hk,  "clicked",       G_CALLBACK(on_hotkey_clicked), NULL);
    g_signal_connect(btn_run, "clicked",       G_CALLBACK(on_run_clicked),   NULL);
}

int main(int argc, char **argv)
{
    /* Must match jetautoclicker.desktop so the compositor (Wayland app_id /
     * X11 class) associates the window with its menu entry and icon. */
    g_set_prgname("jetautoclicker");
    g_set_application_name(APP_NAME);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("%s %s\n", APP_NAME, APP_VERSION);
            return 0;
        }
    }

    gtk_init(&argc, &argv);
    gtk_window_set_default_icon_name("jetautoclicker");

    uinput_ok = engine_init(on_engine_change, on_engine_capture, NULL);
    engine_set_external(portal_inject);
    engine_set_wayland(wayland_inject);

    /* The wlr virtual pointer is available on niri/sway and needs no
     * permissions.  The portal is started lazily only when it is selected. */
    wayland_ok = wayland_init();
    engine_set_wayland_ready(wayland_ok);

    load_config();                 /* may set the hotkey in the engine */
    build_ui();
    apply_settings();              /* starts the portal if the chosen mode needs it */
    refresh_ui();

    g_timeout_add(500, on_tick, NULL);
    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
