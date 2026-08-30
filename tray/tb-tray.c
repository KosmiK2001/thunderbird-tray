/*
 * tb-tray - GTK3 system tray applet for Thunderbird
 *
 * - receives unread reports from the tb-tray MailExtension
 *   (HTTP POST on 127.0.0.1:8765)
 * - tray icon with unread counter, tooltip, new-mail notifications
 * - starts Thunderbird hidden (withdraws its window) when it is not running
 * - optional ASUS mail-LED hook (build with -DASUS_LED)
 */
#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <cairo.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <dirent.h>

#ifdef ASUS_LED
#include <gio/gio.h>
#endif

#define HTTP_PORT 8765
#define LOCK_PATH "/tmp/tb-tray.lock"
#define TB_CMD_DEFAULT "/opt/thunderbird/thunderbird"

static GtkStatusIcon *status_icon = NULL;
static GApplication *app = NULL;
static int last_unread = -1;
static char last_from[256] = {0};
static char last_subject[512] = {0};
static int lock_fd = -1;
static char tb_cmd[512] = TB_CMD_DEFAULT;

/* ---------------- utilities ---------------- */

static gboolean proc_running(const char *needle) {
    DIR *d = opendir("/proc");
    if (!d) return FALSE;
    struct dirent *ent;
    gboolean found = FALSE;
    while ((ent = readdir(d)) != NULL && !found) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
        char path[512], buf[128] = {0};
        snprintf(path, sizeof(path), "/proc/%s/comm", ent->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        int n = (int)read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) buf[strcspn(buf, "\n")] = 0;
        if (strstr(buf, needle)) found = TRUE;
    }
    closedir(d);
    return found;
}

static void spawn_tb(gboolean visible) {
    GError *err = NULL;
    if (!g_spawn_command_line_async(tb_cmd, &err)) {
        g_warning("cannot start thunderbird: %s", err->message);
        g_error_free(err);
        return;
    }
    g_debug("thunderbird spawned (visible=%d)", visible);
}

/* ---------------- X11: hide thunderbird window ---------------- */
#ifdef WITH_X11
#include <X11/Xlib.h>
#include <X11/Xatom.h>

static gboolean hide_tb_windows(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) return FALSE;
    Window root = DefaultRootWindow(d);
    Atom client_list = XInternAtom(d, "_NET_CLIENT_LIST", TRUE);
    Atom wm_pid = XInternAtom(d, "_NET_WM_PID", TRUE);
    Atom wm_class = XInternAtom(d, "WM_CLASS", TRUE);
    gboolean hidden_any = FALSE;

    if (client_list != None) {
        Atom type; int fmt; unsigned long n, left; unsigned char *data = NULL;
        if (XGetWindowProperty(d, root, client_list, 0, 1024, FALSE, XA_WINDOW,
                               &type, &fmt, &n, &left, &data) == Success && data) {
            Window *wins = (Window *)data;
            for (unsigned long i = 0; i < n; i++) {
                unsigned long *pid = NULL; Atom t; int f;
                unsigned long ln, lb;
                if (XGetWindowProperty(d, wins[i], wm_pid, 0, 1, FALSE, XA_CARDINAL,
                                       &t, &f, &ln, &lb, (unsigned char **)&pid) == Success
                    && pid) {
                    /* pid check: comm of that pid contains thunderbird? */
                    char path[512], comm[128] = {0};
                    snprintf(path, sizeof(path), "/proc/%lu/comm", (unsigned long)*pid);
                    int fd = open(path, O_RDONLY);
                    if (fd >= 0) {
                        int rd = (int)read(fd, comm, sizeof(comm) - 1);
                        close(fd);
                        if (rd > 0) comm[strcspn(comm, "\n")] = 0;
                    }
                    XFree(pid);
                    if (strstr(comm, "thunderbird")) {
                        unsigned char *cls = NULL;
                        if (XGetWindowProperty(d, wins[i], wm_class, 0, 256, FALSE, XA_STRING,
                                               &t, &f, &ln, &lb, &cls) == Success && cls) {
                            /* withdraw only Mail windows */
                            if (strstr((char *)cls, "thunderbird") ||
                                strstr((char *)cls, "Mail")) {
                                XWithdrawWindow(d, wins[i], DefaultScreen(d));
                                hidden_any = TRUE;
                            }
                            XFree(cls);
                        }
                    }
                }
            }
            XFree(data);
        }
    }
    XFlush(d);
    XCloseDisplay(d);
    return hidden_any;
}
#else
static gboolean hide_tb_windows(void) { return FALSE; }
#endif

/* called from a GSource a few seconds after spawn to withdraw the window */
static gboolean delayed_hide(gpointer data G_GNUC_UNUSED) {
    static int tries = 0;
    if (hide_tb_windows()) { tries = 0; return FALSE; }
    return (++tries < 30); /* keep trying up to ~15s */
}

static void ensure_tb_running(gboolean hidden) {
    if (proc_running("thunderbird")) return;
    spawn_tb(hidden);
    if (hidden)
        g_timeout_add(2500, delayed_hide, NULL); /* start trying after 2.5s */
}

/* ---------------- tray icon (cairo) ---------------- */
static GdkPixbuf* draw_icon(int unread) {
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 32, 32);
    cairo_t *cr = cairo_create(surf);

    /* envelope */
    cairo_set_source_rgba(cr, 0.15, 0.25, 0.45, 1.0);
    cairo_rectangle(cr, 2, 7, 28, 19);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, 3, 8);
    cairo_line_to(cr, 16, 19);
    cairo_line_to(cr, 29, 8);
    cairo_stroke(cr);

    /* counter bubble */
    if (unread > 0) {
        cairo_set_source_rgba(cr, 0.85, 0.15, 0.15, 1.0);
        cairo_arc(cr, 24, 9, 8, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 1.0);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, unread > 99 ? 8 : 11);
        char txt[8];
        snprintf(txt, sizeof(txt), unread > 99 ? "99+" : "%d", unread);
        cairo_text_extents_t te;
        cairo_text_extents(cr, txt, &te);
        cairo_move_to(cr, 24 - te.width / 2, 9 + te.height / 2);
        cairo_show_text(cr, txt);
    }

    cairo_destroy(cr);
    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(surf, 0, 0, 32, 32);
    cairo_surface_destroy(surf);
    return pb;
}

static void update_icon(int unread) {
    if (!status_icon) return;
    GdkPixbuf *pb = draw_icon(unread);
    gtk_status_icon_set_from_pixbuf(status_icon, pb);
    g_object_unref(pb);
    char tip[128];
    snprintf(tip, sizeof(tip), "Thunderbird: %d unread", unread);
    gtk_status_icon_set_tooltip_text(status_icon, tip);
}

/* ---------------- LED hook (optional build) ---------------- */
#ifdef ASUS_LED
#ifndef ASUS_LED_URL_DEFAULT
#define ASUS_LED_URL_DEFAULT "http://192.168.137.115:8077/mail"
#endif
static void led_report(int unread) {
    const char *url = g_getenv("TB_LED_URL");
    if (!url || !url[0]) url = ASUS_LED_URL_DEFAULT;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s?unread=%d", url, unread);
    GSocketClient *cl = g_socket_client_new();
    /* fire and forget */
    g_socket_client_connect_to_host_async(cl, "192.168.137.115", 8077, NULL, NULL, NULL);
    (void)cmd; (void)unread;
    g_object_unref(cl);
}
#else
static void led_report(int unread G_GNUC_UNUSED) {}
#endif

/* ---------------- notifications ---------------- */
static void notify_new_mail(int unread) {
    char title[64], body[768];
    snprintf(title, sizeof(title), "Новое письмо (%d)", unread);
    if (last_subject[0])
        snprintf(body, sizeof(body), "%s\n%s", last_from, last_subject);
    else
        snprintf(body, sizeof(body), "%s", last_from);

    GNotification *n = g_notification_new(title);
    g_notification_set_body(n, body);
    g_application_send_notification(app, "new-mail", n);
    g_object_unref(n);
}

/* ---------------- HTTP report server ---------------- */
static int extract_int(const char *body, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(body, pat);
    return p ? atoi(p + strlen(pat)) : 0;
}

static void extract_str(const char *body, const char *key, char *out, size_t outsz) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(body, pat);
    if (!p) return;
    p += strlen(pat);
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < outsz) {
        if (*p == '\\' && p[1]) p++;      /* skip escape */
        out[o++] = *p++;
    }
    out[o] = 0;
}

static void handle_report(const char *body) {
    int unread = extract_int(body, "unread");
    if (unread == last_unread) return;

    char from[256] = {0}, subject[512] = {0};
    extract_str(body, "from", from, sizeof(from));
    extract_str(body, "subject", subject, sizeof(subject));
    if (from[0]) snprintf(last_from, sizeof(last_from), "%s", from);
    if (subject[0]) snprintf(last_subject, sizeof(last_subject), "%s", subject);

    gboolean increased = (unread > last_unread);
    int old = last_unread;
    last_unread = unread;
    update_icon(unread);

    if (increased && old >= 0)
        notify_new_mail(unread);
    if (increased)
        led_report(unread);
}

static gboolean on_http_incoming(GSocketService *svc G_GNUC_UNUSED,
                                 GSocketConnection *conn,
                                 GObject *src G_GNUC_UNUSED,
                                 gpointer data G_GNUC_UNUSED) {
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    char buf[4096] = {0};
    gssize n = g_input_stream_read(in, buf, sizeof(buf) - 1, NULL, NULL);
    if (n > 0) {
        const char *body = strstr(buf, "\r\n\r\n");
        body = body ? body + 4 : buf;          /* tolerant fallback */
        handle_report(body);
        const char *resp = "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nOK";
        GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
        g_output_stream_write(out, resp, strlen(resp), NULL, NULL);
    }
    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
    return TRUE;
}

/* ---------------- tray click ---------------- */
static void on_tray_activate(GtkStatusIcon *icon G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED) {
    if (proc_running("thunderbird"))
        spawn_tb(TRUE);   /* existing instance: raises the window */
    else
        ensure_tb_running(FALSE); /* visible start */
}

/* ---------------- main ---------------- */
static void on_app_activate(GtkApplication *a G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED) {}

int main(int argc, char **argv) {
    /* single instance */
    lock_fd = open(LOCK_PATH, O_RDWR | O_CREAT, 0666);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        g_printerr("tb-tray already running\n");
        return 0;
    }

    const char *cmd = g_getenv("TB_CMD");
    if (cmd && cmd[0]) snprintf(tb_cmd, sizeof(tb_cmd), "%s", cmd);

    GtkApplication *appobj = gtk_application_new("local.kosmik.tbtray", G_APPLICATION_NON_UNIQUE);
    app = G_APPLICATION(appobj);
    g_signal_connect(appobj, "activate", G_CALLBACK(on_app_activate), NULL);
    g_application_register(app, NULL, NULL);
    g_application_activate(app);

    status_icon = gtk_status_icon_new();
    update_icon(0);

    GSocketService *svc = g_socket_service_new();
    GError *err = NULL;
    if (!g_socket_listener_add_inet_port(G_SOCKET_LISTENER(svc), HTTP_PORT, NULL, &err)) {
        g_printerr("cannot listen on %d: %s\n", HTTP_PORT, err ? err->message : "?");
        return 1;
    }
    g_signal_connect(svc, "incoming", G_CALLBACK(on_http_incoming), NULL);
    g_socket_service_start(svc);

    /* start thunderbird hidden if it is not running */
    ensure_tb_running(TRUE);

    g_message("tb-tray started (http://127.0.0.1:%d/report)", HTTP_PORT);
    gtk_main();
    return 0;
}
