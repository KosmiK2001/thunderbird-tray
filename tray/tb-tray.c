/*
 * tb-tray - GTK3 system tray applet for Thunderbird
 *
 * - HTTP-сервер 127.0.0.1:8765 принимает репорты от MailExtension
 * - иконка трея со счётчиком, tooltip с версией
 * - уведомление при увеличении непрочитанных
 * - ЛКМ: развернуть/свернуть окно Thunderbird; двойной ЛКМ: окно статистики
 *   (ящик — новых/всего, автоскрытие 5 сек)
 * - ПКМ: меню — Развернуть/Свернуть, Обновить (XTEST Shift+F5 в TB),
 *   About (версия + toolkit), Exit
 * - стартует TB скрытно (XWithdrawWindow), если он не запущен
 */
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <cairo.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <dirent.h>
#include <locale.h>
#include <signal.h>

#ifdef WITH_X11
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#endif

#define HTTP_PORT 8765
#define LOCK_PATH "/tmp/tb-tray.lock"
#define TB_CMD_DEFAULT "/opt/thunderbird/thunderbird"
#ifndef VERSION
#define VERSION "1.0.0"
#endif

typedef struct {
    char name[128];
    int unread;
    int total;
} BoxStat;

#define MAX_BOXES 16
static BoxStat boxes[MAX_BOXES];
static int n_boxes = 0;

static GtkStatusIcon *status_icon = NULL;
static GApplication *app = NULL;
static GtkWidget *stats_window = NULL;
static GtkWidget *stats_box = NULL;
static guint stats_timeout_id = 0;
static int last_unread = -1;
static char last_from[256] = {0};
static char last_subject[512] = {0};
static int lock_fd = -1;
static char tb_cmd[512] = TB_CMD_DEFAULT;
static gboolean pending_single = FALSE;
static guint pending_single_id = 0;
static gint64 last_activate_time = 0;

/* ---------------- forward decls ---------------- */
static void update_icon(int unread);
static void notify_new_mail(int unread);
static void led_report(int unread);

static gboolean delayed_hide(gpointer data G_GNUC_UNUSED);
static void ensure_tb_running(gboolean hidden);
static void tb_withdraw_all(void);

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

static void spawn_tb(gboolean hidden) {
    GError *err = NULL;
    if (!g_spawn_command_line_async(tb_cmd, &err)) {
        g_warning("cannot start thunderbird: %s", err->message);
        g_error_free(err);
        return;
    }
    if (hidden)
        g_timeout_add(2500, delayed_hide, NULL);
    g_debug("thunderbird spawned (hidden=%d)", hidden);
}

static gboolean delayed_hide(gpointer data G_GNUC_UNUSED);
static void ensure_tb_running(gboolean hidden);

/* защита от дурака: TB не найден вообще */
static gboolean scary_close(gpointer data) {
    GtkWidget *w = GTK_WIDGET(data);
    gtk_widget_destroy(w);
    gtk_main_quit();           /* полный выход апплета */
    return FALSE;
}

static gboolean find_tb_cmd(void) {
    if (g_getenv("TB_CMD") && g_getenv("TB_CMD")[0]) {
        snprintf(tb_cmd, sizeof(tb_cmd), "%s", g_getenv("TB_CMD"));
        return g_access(tb_cmd, X_OK) == 0;
    }
    const char *cands[] = { "/opt/thunderbird/thunderbird",
                            "/usr/bin/thunderbird",
                            "/usr/local/bin/thunderbird", NULL };
    for (int i = 0; cands[i]; i++) {
        if (g_access(cands[i], X_OK) == 0) {
            snprintf(tb_cmd, sizeof(tb_cmd), "%s", cands[i]);
            return TRUE;
        }
    }
    return FALSE;
}

static void scary_missing_tb(void) {
    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w), "tb-tray: ОШИБКА");
    gtk_window_set_position(GTK_WINDOW(w), GTK_WIN_POS_CENTER_ALWAYS);
    gtk_window_set_default_size(GTK_WINDOW(w), 520, 180);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(w), FALSE);
    GtkWidget *lbl = gtk_label_new(NULL);
    gchar *msg = g_strdup_printf(
        "<span foreground='red' size='20000' weight='bold'>%s</span>\n\n%s",
        _("MAIL CLIENT THUNDERBIRD NOT FOUND!"),
        _("Check your Thunderbird installation\n(or set the path in the TB_CMD variable).\nThe applet will close in 10 seconds."));
    gtk_label_set_markup(GTK_LABEL(lbl), msg);
    g_free(msg);
    gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_CENTER);
    gtk_container_add(GTK_CONTAINER(w), lbl);
    gtk_widget_show_all(w);
    g_timeout_add(10000, scary_close, w);
}

/* ---------------- X11: окна Thunderbird ---------------- */
#ifdef WITH_X11
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

/* ищем ВСЕ топ-окна TB через XQueryTree: работает и для управляемых,
 * и для withdrawn-окон (спрятанных), которых нет в _NET_CLIENT_LIST */
static int find_tb_windows(Display *d, Window root, Window *out, int max) {
    Atom wm_class = XInternAtom(d, "WM_CLASS", TRUE);
    Window root_ret, parent;
    Window *children = NULL;
    unsigned int nchild = 0;
    int found = 0;
    if (!XQueryTree(d, root, &root_ret, &parent, &children, &nchild) || !children)
        return 0;
    for (unsigned int i = 0; i < nchild && found < max; i++) {
        unsigned char *cls = NULL; Atom t; int f;
        unsigned long ln, lb;
        if (XGetWindowProperty(d, children[i], wm_class, 0, 256, FALSE, XA_STRING,
                               &t, &f, &ln, &lb, &cls) == Success && cls) {
            if (strstr((char *)cls, "thunderbird") || strstr((char *)cls, "Mail"))
                out[found++] = children[i];
            XFree(cls);
        }
    }
    if (children) XFree(children);
    return found;
}

static gboolean window_is_hidden(Display *d, Window w) {
    Atom state = XInternAtom(d, "_NET_WM_STATE", TRUE);
    Atom hidden_a = XInternAtom(d, "_NET_WM_STATE_HIDDEN", TRUE);
    unsigned char *prop = NULL; Atom t; int f;
    unsigned long ln, lb;
    gboolean is_hidden = FALSE;
    if (state != None && hidden_a != None &&
        XGetWindowProperty(d, w, state, 0, 64, FALSE, XA_ATOM,
                           &t, &f, &ln, &lb, &prop) == Success && prop) {
        Atom *atoms = (Atom *)prop;
        for (unsigned long k = 0; k < ln; k++)
            if (atoms[k] == hidden_a) is_hidden = TRUE;
        XFree(prop);
    }
    return is_hidden;
}

static void tb_activate(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) return;
    Window wins[16];
    int n = find_tb_windows(d, DefaultRootWindow(d), wins, 16);
    for (int i = 0; i < n; i++) {
        XMapRaised(d, wins[i]);
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.xclient.type = ClientMessage;
        ev.xclient.window = wins[i];
        ev.xclient.message_type = XInternAtom(d, "_NET_ACTIVE_WINDOW", FALSE);
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = 1;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(d, DefaultRootWindow(d), FALSE,
                   SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    }
    XFlush(d);
    XCloseDisplay(d);
}

static void tb_minimize(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) return;
    Window wins[16];
    int n = find_tb_windows(d, DefaultRootWindow(d), wins, 16);
    for (int i = 0; i < n; i++)
        XIconifyWindow(d, wins[i], DefaultScreen(d));
    XFlush(d);
    XCloseDisplay(d);
}

static gboolean tb_visible(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) return FALSE;
    Window wins[16];
    int n = find_tb_windows(d, DefaultRootWindow(d), wins, 16);
    gboolean visible = FALSE;
    for (int i = 0; i < n; i++) {
        XWindowAttributes wa;
        if (XGetWindowAttributes(d, wins[i], &wa) &&
            wa.map_state == IsViewable && !window_is_hidden(d, wins[i]))
            visible = TRUE;
    }
    XCloseDisplay(d);
    return visible;
}

static void tb_toggle(void) {
    if (tb_visible()) tb_withdraw_all();   /* окно исчезает полностью */
    else tb_activate();
}

static void tb_withdraw_all(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) return;
    Window wins[16];
    int n = find_tb_windows(d, DefaultRootWindow(d), wins, 16);
    for (int i = 0; i < n; i++)
        XWithdrawWindow(d, wins[i], DefaultScreen(d));
    XFlush(d);
    XCloseDisplay(d);
}

/* XTEST: Shift+F5 -> окно TB (Get messages for all accounts) */
static void tb_refresh_keys(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) return;
    int xtst = 0, fev = 0, fer = 0;
    if (!XQueryExtension(d, "XTEST", &xtst, &fev, &fer)) { XCloseDisplay(d); return; }
    Window wins[16];
    int n = find_tb_windows(d, DefaultRootWindow(d), wins, 16);
    if (n == 0) { XCloseDisplay(d); return; }
    /* сфокусировать окно TB, чтобы клавиши ушли туда */
    XSetInputFocus(d, wins[0], RevertToPointerRoot, CurrentTime);
    XFlush(d);
    KeyCode f5c = XKeysymToKeycode(d, XStringToKeysym("F5"));
    KeyCode shc = XKeysymToKeycode(d, XK_Shift_L);
    if (f5c && shc) {
        XTestFakeKeyEvent(d, shc, True, CurrentTime);
        XTestFakeKeyEvent(d, f5c, True, CurrentTime);
        XTestFakeKeyEvent(d, f5c, False, CurrentTime);
        XTestFakeKeyEvent(d, shc, False, CurrentTime);
        XFlush(d);
    }
    XCloseDisplay(d);
}
#else
static gboolean tb_visible(void) { return FALSE; }
static void tb_toggle(void) {}
static void tb_activate(void) {}
static void tb_withdraw_all(void) {}
static void tb_refresh_keys(void) {}
#endif

/* отложенное скрытие окна после скрытого запуска TB */
static gboolean delayed_hide(gpointer data G_GNUC_UNUSED) {
    static int tries = 0;
#ifdef WITH_X11
    Display *d = XOpenDisplay(NULL);
    if (d) {
        Window wins[16];
        int n = find_tb_windows(d, DefaultRootWindow(d), wins, 16);
        for (int i = 0; i < n; i++)
            XWithdrawWindow(d, wins[i], DefaultScreen(d));
        XCloseDisplay(d);
        if (n > 0) { tries = 0; return FALSE; }
    }
#endif
    return (++tries < 30);
}

static void ensure_tb_running(gboolean hidden) {
    if (proc_running("thunderbird")) return;
    spawn_tb(hidden);
}

/* завершить Thunderbird (штатный SIGTERM) */
static void term_tb(void) {
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
        char path[512], buf[128] = {0};
        snprintf(path, sizeof(path), "/proc/%s/comm", ent->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        int n = (int)read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[strcspn(buf, "\n")] = 0;
        if (strstr(buf, "thunderbird"))
            kill((pid_t)atoi(ent->d_name), SIGTERM);
    }
    closedir(d);
}

/* ---------------- tray icon (cairo) ---------------- */
static GdkPixbuf* draw_icon(int unread) {
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 32, 32);
    cairo_t *cr = cairo_create(surf);

    cairo_set_source_rgba(cr, 0.15, 0.25, 0.45, 1.0);
    cairo_rectangle(cr, 2, 7, 28, 19);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, 3, 8);
    cairo_line_to(cr, 16, 19);
    cairo_line_to(cr, 29, 8);
    cairo_stroke(cr);

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
    char tip[160];
    snprintf(tip, sizeof(tip), _("Thunderbird: %1$d unread | tb-tray v%2$s"), unread, VERSION);
    gtk_status_icon_set_tooltip_text(status_icon, tip);
}

/* ---------------- notifications ---------------- */
static void notify_new_mail(int unread) {
    char title[64], body[768];
    snprintf(title, sizeof(title), _("New mail (%d)"), unread);
    if (last_subject[0])
        snprintf(body, sizeof(body), "%s\n%s", last_from, last_subject);
    else
        snprintf(body, sizeof(body), "%s", last_from);

    GNotification *n = g_notification_new(title);
    g_notification_set_body(n, body);
    g_application_send_notification(app, "new-mail", n);
    g_object_unref(n);
}

/* ---------------- LED hook (optional) ---------------- */
#ifdef ASUS_LED
#ifndef ASUS_LED_URL_DEFAULT
#define ASUS_LED_URL_DEFAULT "http://192.168.137.115:8077/mail"
#endif
static void led_report(int unread) {
    const char *url = g_getenv("TB_LED_URL");
    if (!url || !url[0]) url = ASUS_LED_URL_DEFAULT;
    GSocketClient *cl = g_socket_client_new();
    g_socket_client_connect_to_host_async(cl, "192.168.137.115", 8077, NULL, NULL, NULL);
    (void)url; (void)unread;
    g_object_unref(cl);
}
#else
static void led_report(int unread G_GNUC_UNUSED) {}
#endif

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
        if (*p == '\\' && p[1]) p++;
        out[o++] = *p++;
    }
    out[o] = 0;
}

static void parse_boxes(const char *body) {
    n_boxes = 0;
    const char *p = body;
    while (n_boxes < MAX_BOXES) {
        p = strstr(p, "\"name\":\"");
        if (!p) break;
        p += 8;
        BoxStat *b = &boxes[n_boxes];
        size_t o = 0;
        while (*p && *p != '"' && o + 1 < sizeof(b->name)) {
            if (*p == '\\' && p[1]) p++;
            b->name[o++] = *p++;
        }
        b->name[o] = 0;
        const char *u = strstr(p, "\"unread\":");
        const char *t = strstr(p, "\"total\":");
        if (!u || !t || t < u) break;
        b->unread = atoi(u + 9);
        b->total = atoi(t + 8);
        n_boxes++;
        p = t + 8;
    }
}

static char close_action[16] = "hide";

static gboolean respawn_check(gpointer data G_GNUC_UNUSED) {
    /* closeAction=hide: если TB закрыли крестиком — перезапускаем скрытно */
    if (strcmp(close_action, "hide") == 0 && !proc_running("thunderbird")) {
        spawn_tb(TRUE);
        g_timeout_add(2500, delayed_hide, NULL);
    }
    return TRUE;
}

static void handle_report(const char *body) {
    parse_boxes(body);
    g_message("report received: unread=%d boxes=%d body_len=%zu",
              extract_int(body, "unread"), n_boxes, strlen(body));
    { char ca[16] = {0};
      extract_str(body, "closeAction", ca, sizeof(ca));
      if (ca[0]) snprintf(close_action, sizeof(close_action), "%s", ca); }
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
    char buf[8192] = {0};
    gssize n = g_input_stream_read(in, buf, sizeof(buf) - 1, NULL, NULL);
    if (n > 0) {
        const char *body = strstr(buf, "\r\n\r\n");
        body = body ? body + 4 : buf;
        handle_report(body);
        const char *resp = "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nOK";
        GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
        g_output_stream_write(out, resp, strlen(resp), NULL, NULL);
    }
    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
    return TRUE;
}

/* ---------------- stats window (double click) ---------------- */
static gboolean stats_hide(gpointer data G_GNUC_UNUSED) {
    if (stats_window) gtk_widget_hide(stats_window);
    stats_timeout_id = 0;
    return FALSE;
}

static void stats_show(void) {
    if (!stats_window) {
        stats_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(stats_window), "tb-tray");
        gtk_window_set_decorated(GTK_WINDOW(stats_window), FALSE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(stats_window), TRUE);
        gtk_container_set_border_width(GTK_CONTAINER(stats_window), 10);
        stats_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_container_add(GTK_CONTAINER(stats_window), stats_box);
    }
    /* перечитать список */
    GtkContainer *c = GTK_CONTAINER(stats_box);
    gtk_container_foreach(c, (GtkCallback)gtk_widget_destroy, NULL);
    if (n_boxes == 0)
        gtk_box_pack_start(GTK_BOX(stats_box),
                           gtk_label_new(_("No data — is the tb-tray reporter extension running?")),
                           FALSE, FALSE, 0);
    for (int i = 0; i < n_boxes; i++) {
        char line[256];
        snprintf(line, sizeof(line), _("%1$s — %2$d new / %3$d total"),
                 boxes[i].name, boxes[i].unread, boxes[i].total);
        gtk_box_pack_start(GTK_BOX(stats_box), gtk_label_new(line), FALSE, FALSE, 0);
    }
    gtk_widget_show_all(stats_window);
    if (stats_timeout_id) g_source_remove(stats_timeout_id);
    stats_timeout_id = g_timeout_add(5000, stats_hide, NULL);
}

/* ---------------- tray events ---------------- */
static gboolean single_click_now(gpointer data G_GNUC_UNUSED) {
    pending_single_id = 0;
    if (!pending_single) return FALSE;
    pending_single = FALSE;
    tb_toggle();
    return FALSE;
}

static gboolean on_tray_activate(GtkStatusIcon *icon G_GNUC_UNUSED,
                                 gpointer data G_GNUC_UNUSED) {
    gint64 now = g_get_monotonic_time();
    if (now - last_activate_time < 400000) {
        /* двойной клик: окно статистики */
        pending_single = FALSE;
        if (pending_single_id) { g_source_remove(pending_single_id); pending_single_id = 0; }
        last_activate_time = 0;
        stats_show();
        return FALSE;
    }
    last_activate_time = now;
    pending_single = TRUE;
    if (pending_single_id) g_source_remove(pending_single_id);
    pending_single_id = g_timeout_add(350, single_click_now, NULL);
    return FALSE;
}

/* ---------------- right-click menu ---------------- */
static void on_menu_toggle(GtkMenuItem *item G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED) {
    tb_toggle();
}

static void on_menu_refresh(GtkMenuItem *item G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED) {
    tb_refresh_keys();
}

static GtkWidget *about_dialog = NULL;

static gboolean about_close(gpointer data) {
    if (about_dialog) gtk_widget_destroy(GTK_WIDGET(data));
    about_dialog = NULL;
    return FALSE;
}

static void on_menu_about(GtkMenuItem *item G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED) {
    if (about_dialog) return;
    about_dialog = gtk_about_dialog_new();
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(about_dialog), "tb-tray");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(about_dialog), VERSION);
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(about_dialog),
                                  _("Thunderbird tray applet (GTK3 build)"));
    gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(about_dialog),
                                   "kosmik2001@gmail.com");
    gtk_widget_show_all(about_dialog);
    g_timeout_add(5000, about_close, about_dialog);
}

static void on_menu_exit(GtkMenuItem *item G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED) {
    term_tb();          /* закрываем и Thunderbird */
    gtk_main_quit();
}

static void on_tray_popup(GtkStatusIcon *icon G_GNUC_UNUSED, guint button,
                          guint activate_time, gpointer data G_GNUC_UNUSED) {
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item;

    item = gtk_menu_item_new_with_label(tb_visible() ? _("Minimize") : _("Restore"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_toggle), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label(_("Refresh"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_refresh), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label(_("About"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_about), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label(_("Exit"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_exit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
    (void)button; (void)activate_time;
}

static gboolean quit_cb(gpointer data G_GNUC_UNUSED) {
    term_tb();          /* при завершении апплета гасим и TB */
    gtk_main_quit();
    return FALSE;
}

/* ---------------- main ---------------- */
static void on_app_activate(GtkApplication *a G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED) {}

int main(int argc, char **argv) {
    lock_fd = open(LOCK_PATH, O_RDWR | O_CREAT, 0666);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        g_printerr("%s\n", _("tb-tray already running"));
        return 0;
    }

    const char *cmd = g_getenv("TB_CMD");
    if (cmd && cmd[0]) snprintf(tb_cmd, sizeof(tb_cmd), "%s", cmd);

    setlocale(LC_ALL, "");
    bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
    textdomain(GETTEXT_PACKAGE);

    gtk_init(&argc, &argv);

    if (!find_tb_cmd()) {
        scary_missing_tb();
        gtk_main();
        return 1;
    }

    GtkApplication *appobj = gtk_application_new("local.kosmik.tbtray", G_APPLICATION_NON_UNIQUE);
    app = G_APPLICATION(appobj);
    g_signal_connect(appobj, "activate", G_CALLBACK(on_app_activate), NULL);
    g_application_register(app, NULL, NULL);
    g_application_activate(app);

    status_icon = gtk_status_icon_new();
    update_icon(0);

    g_signal_connect(status_icon, "activate", G_CALLBACK(on_tray_activate), NULL);
    g_signal_connect(status_icon, "popup-menu", G_CALLBACK(on_tray_popup), NULL);

    GSocketService *svc = g_socket_service_new();
    GError *err = NULL;
    if (!g_socket_listener_add_inet_port(G_SOCKET_LISTENER(svc), HTTP_PORT, NULL, &err)) {
        g_printerr("cannot listen on %d: %s\n", HTTP_PORT, err ? err->message : "?");
        return 1;
    }
    g_signal_connect(svc, "incoming", G_CALLBACK(on_http_incoming), NULL);
    g_socket_service_start(svc);

    ensure_tb_running(TRUE);
    g_timeout_add(3000, respawn_check, NULL);

    g_unix_signal_add(SIGTERM, quit_cb, NULL);
    g_unix_signal_add(SIGINT, quit_cb, NULL);

    g_message("tb-tray started (http://127.0.0.1:%d/report)", HTTP_PORT);
    gtk_main();
    return 0;
}
