#include "portal.h"

#include <gio/gio.h>
#include <unistd.h>

#define PORTAL_BUS  "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define RD_IFACE    "org.freedesktop.portal.RemoteDesktop"
#define REQ_IFACE   "org.freedesktop.portal.Request"
#define SES_IFACE   "org.freedesktop.portal.Session"

typedef void (*ResponseFn)(guint response, GVariant *results);
typedef struct { guint sub; ResponseFn fn; } Pending;

static GDBusConnection *conn;
static char            *session;          /* session handle (object path) */
static gint             ready;            /* atomic */
static char             status[256] = "Portal: not started";
static PortalStateFn    state_cb;
static void            *state_user;
static int              token_counter;

static void set_status(const char *msg, gboolean is_ready)
{
    g_strlcpy(status, msg, sizeof status);
    g_atomic_int_set(&ready, is_ready);
    if (state_cb)
        state_cb(state_user);
}

static void fail(const char *what, const char *detail)
{
    char buf[256];
    g_snprintf(buf, sizeof buf, "Portal: %s%s%s", what, detail ? ": " : "", detail ? detail : "");
    set_status(buf, FALSE);
}

/* ---- restore token (so the permission dialog is shown only once) ---- */

static char *token_file(void)
{
    return g_build_filename(g_get_user_config_dir(), "jetautoclicker", "portal_token", NULL);
}

static char *load_token(void)
{
    char *path = token_file(), *data = NULL;
    if (!g_file_get_contents(path, &data, NULL, NULL))
        data = NULL;
    g_free(path);
    if (data) g_strstrip(data);
    if (data && !*data) { g_free(data); data = NULL; }
    return data;
}

static void save_token(const char *tok)
{
    char *path = token_file(), *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_file_set_contents(path, tok, -1, NULL);
    g_free(dir); g_free(path);
}

/* ---- request / response plumbing ---- */

static char *new_token(void)
{
    return g_strdup_printf("jet%d_%d", (int)getpid(), ++token_counter);
}

static void on_response(GDBusConnection *c, const gchar *sender, const gchar *path,
                        const gchar *iface, const gchar *sig, GVariant *params, gpointer data)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)sig;
    Pending *p = data;
    ResponseFn fn = p->fn;
    guint response = 2;
    GVariant *results = NULL;
    g_variant_get(params, "(u@a{sv})", &response, &results);
    g_dbus_connection_signal_unsubscribe(conn, p->sub);   /* one-shot; Pending freed by notify */
    fn(response, results);
    g_variant_unref(results);
}

static void subscribe_request(const char *token, ResponseFn fn)
{
    /* Request path: /org/freedesktop/portal/desktop/request/<sender>/<token> */
    char *sender = g_strdup(g_dbus_connection_get_unique_name(conn) + 1);
    for (char *p = sender; *p; p++)
        if (*p == '.') *p = '_';
    char *path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);

    Pending *p = g_new0(Pending, 1);
    p->fn = fn;
    p->sub = g_dbus_connection_signal_subscribe(conn, PORTAL_BUS, REQ_IFACE, "Response", path,
                                                NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                on_response, p, g_free);
    g_free(path);
    g_free(sender);
}

static void on_call_done(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)data;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (r) { g_variant_unref(r); return; }
    if (err)
        g_dbus_error_strip_remote_error(err);   /* strips the D-Bus prefix in place */
    fail("call failed", err ? err->message : "unknown error");
    g_clear_error(&err);
}

static void portal_call(const char *method, GVariant *args)
{
    g_dbus_connection_call(conn, PORTAL_BUS, PORTAL_PATH, RD_IFACE, method, args, NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_call_done, NULL);
}

/* ---- setup sequence: CreateSession -> SelectDevices -> Start ---- */

static void on_session_closed(GDBusConnection *c, const gchar *s, const gchar *p, const gchar *i,
                              const gchar *sig, GVariant *params, gpointer d)
{
    (void)c; (void)s; (void)p; (void)i; (void)sig; (void)params; (void)d;
    set_status("Portal: session closed (restart the app to reconnect)", FALSE);
}

static void on_started(guint response, GVariant *results)
{
    if (response == 1) { fail("permission denied or dialog cancelled", NULL); return; }
    if (response != 0) { fail("Start failed", NULL); return; }

    guint32 devices = 0;
    if (g_variant_lookup(results, "devices", "u", &devices) && !(devices & 2)) {
        fail("pointer access was not granted", NULL);
        return;
    }
    const char *tok = NULL;
    if (g_variant_lookup(results, "restore_token", "&s", &tok) && tok && *tok)
        save_token(tok);
    set_status("Portal: ready", TRUE);
}

static void step_start(void)
{
    char *tok = new_token();
    subscribe_request(tok, on_started);
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(tok));
    set_status("Portal: waiting for you to allow remote input in the dialog...", FALSE);
    portal_call("Start", g_variant_new("(osa{sv})", session, "", &b));
    g_free(tok);
}

static void on_selected(guint response, GVariant *results)
{
    (void)results;
    if (response != 0) { fail("SelectDevices failed", NULL); return; }
    step_start();
}

static void step_select(void)
{
    char *tok = new_token();
    subscribe_request(tok, on_selected);
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(tok));
    g_variant_builder_add(&b, "{sv}", "types", g_variant_new_uint32(2));          /* 2 = pointer */
    g_variant_builder_add(&b, "{sv}", "persist_mode", g_variant_new_uint32(2));   /* until revoked */
    char *restore = load_token();
    if (restore)
        g_variant_builder_add(&b, "{sv}", "restore_token", g_variant_new_string(restore));
    portal_call("SelectDevices", g_variant_new("(oa{sv})", session, &b));
    g_free(restore);
    g_free(tok);
}

static void on_created(guint response, GVariant *results)
{
    if (response != 0) { fail("CreateSession failed", NULL); return; }
    const char *s = NULL;
    if (!g_variant_lookup(results, "session_handle", "&s", &s) &&
        !g_variant_lookup(results, "session_handle", "&o", &s)) {
        fail("no session handle returned", NULL);
        return;
    }
    g_free(session);
    session = g_strdup(s);
    g_dbus_connection_signal_subscribe(conn, PORTAL_BUS, SES_IFACE, "Closed", session, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_session_closed, NULL, NULL);
    step_select();
}

void portal_init(PortalStateFn on_state, void *user)
{
    state_cb = on_state;
    state_user = user;

    GError *err = NULL;
    conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!conn) {
        fail("no D-Bus session bus", err ? err->message : NULL);
        g_clear_error(&err);
        return;
    }

    char *tok = new_token(), *stok = new_token();
    subscribe_request(tok, on_created);
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(tok));
    g_variant_builder_add(&b, "{sv}", "session_handle_token", g_variant_new_string(stok));
    set_status("Portal: connecting...", FALSE);
    portal_call("CreateSession", g_variant_new("(a{sv})", &b));
    g_free(tok);
    g_free(stok);
}

bool portal_ready(void)        { return g_atomic_int_get(&ready) != 0; }
const char *portal_status(void) { return status; }

void portal_inject(int btn, int state)
{
    if (!g_atomic_int_get(&ready))
        return;
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    GDBusMessage *m = g_dbus_message_new_method_call(PORTAL_BUS, PORTAL_PATH, RD_IFACE,
                                                     "NotifyPointerButton");
    g_dbus_message_set_body(m, g_variant_new("(oa{sv}iu)", session, &opts,
                                             (gint32)btn, (guint32)state));
    g_dbus_message_set_flags(m, G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED);
    g_dbus_connection_send_message(conn, m, G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL);
    g_object_unref(m);
}

void portal_close(void)
{
    if (conn && session) {
        g_atomic_int_set(&ready, FALSE);
        g_dbus_connection_call(conn, PORTAL_BUS, session, SES_IFACE, "Close", NULL, NULL,
                               G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, NULL, NULL, NULL);
        g_dbus_connection_flush_sync(conn, NULL, NULL);
    }
}
