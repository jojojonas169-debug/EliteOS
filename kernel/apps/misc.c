/* Images viewer, System Log, Settings, Network. */
#include <wm.h>
#include <mm.h>
#include <sched.h>
#include <vfs.h>
#include <cpu.h>
#include <net.h>
#include <audio.h>
#include <image.h>
#include "../wm/wm_internal.h"

/* ========================================================================
 * Images
 * ====================================================================== */

struct viewer {
    char dir[VFS_PATH_MAX];
    char files[64][64];
    int nfiles;
    int cur;
    surface_t *img;
    float zoom;         /* 0 = fit */
    int panx, pany;
    int dx, dy;
    bool drag;
};

static bool is_image(const char *n) { return image_is_supported(n); }

static void viewer_scan(struct viewer *v)
{
    struct vfs_dirent de[64];
    int n = vfs_list(v->dir, de, 64);
    v->nfiles = 0;
    for (int i = 0; i < n; i++)
        if (de[i].type == VN_FILE && is_image(de[i].name)) strlcpy(v->files[v->nfiles++], de[i].name, 64);
}

static void viewer_load(struct viewer *v)
{
    surface_free(v->img);
    v->img = NULL;
    v->zoom = 0;
    v->panx = v->pany = 0;
    if (v->cur < 0 || v->cur >= v->nfiles) return;
    char p[VFS_PATH_MAX + 64];
    snprintf(p, sizeof(p), "%s/%s", v->dir, v->files[v->cur]);
    v->img = image_load(p);
}

static void viewer_paint(app_t *a, surface_t *s)
{
    struct viewer *v = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, HEX(0x0D0F1A));
    rect_t area = R(0, 0, W, H - 56);
    if (v->img) {
        float fit = MIN((float)area.w / (float)v->img->w, (float)area.h / (float)v->img->h);
        if (fit > 1) fit = 1;
        float z = v->zoom > 0 ? v->zoom : fit;
        int dw = (int)((float)v->img->w * z), dh = (int)((float)v->img->h * z);
        rect_t d = R(area.x + (area.w - dw) / 2 + v->panx, area.y + (area.h - dh) / 2 + v->pany, dw, dh);
        surface_t sub = *s;
        sub.clip = rect_intersect(s->clip, area);
        gfx_shadow(&sub, d.x, d.y + 4, d.w, d.h, 2, 16, 140);
        gfx_blit_scaled(&sub, d, v->img, R(0, 0, v->img->w, v->img->h));
    } else {
        icon_draw(s, ICON_IMAGE, W / 2 - 40, area.h / 2 - 70, 80);
        gfx_text_center(s, font_ui_lg, R(0, area.h / 2 + 20, W, 24),
                        v->nfiles ? "Could not open this picture" : "No pictures yet - press Print Screen to take one!",
                        theme.text_dim);
    }
    /* bottom bar */
    gfx_fill(s, 0, H - 56, W, 56, theme.panel);
    if (ui_button(u, R(12, H - 46, 40, 36), "‹", BTN_SUBTLE) && v->nfiles) { v->cur = (v->cur + v->nfiles - 1) % v->nfiles; viewer_load(v); }
    if (ui_button(u, R(58, H - 46, 40, 36), "›", BTN_SUBTLE) && v->nfiles) { v->cur = (v->cur + 1) % v->nfiles; viewer_load(v); }
    char info[160];
    if (v->img)
        snprintf(info, sizeof(info), "%s   ·   %d x %d   ·   %d of %d", v->files[v->cur], v->img->w, v->img->h, v->cur + 1, v->nfiles);
    else
        snprintf(info, sizeof(info), "%s", v->dir);
    surface_t sub = *s;
    sub.clip = rect_intersect(s->clip, R(110, H - 56, W - 360, 56));
    gfx_text_ellipsis(&sub, font_ui, 112, H - 36, W - 380, info, theme.text_dim);
    if (ui_button(u, R(W - 236, H - 46, 70, 36), "Fit", v->zoom == 0 ? BTN_PRIMARY : BTN_SUBTLE)) { v->zoom = 0; v->panx = v->pany = 0; }
    if (ui_button(u, R(W - 160, H - 46, 70, 36), "100%", v->zoom == 1 ? BTN_PRIMARY : BTN_SUBTLE)) v->zoom = 1;
    if (ui_button(u, R(W - 84, H - 46, 72, 36), "Edit", BTN_SUBTLE) && v->img) {
        char *p = kmalloc(VFS_PATH_MAX + 64);
        snprintf(p, VFS_PATH_MAX + 64, "%s/%s", v->dir, v->files[v->cur]);
        extern int paint_main(void *);
        thread_create_ex("Paint", paint_main, p, 0, -1, NULL, 128 * 1024);
    }
}

static int viewer_event(app_t *a, struct gui_event *ev)
{
    struct viewer *v = a->data;
    switch (ev->type) {
    case EV_KEY_DOWN:
        if (ev->key == KEY_RIGHT && v->nfiles) { v->cur = (v->cur + 1) % v->nfiles; viewer_load(v); }
        if (ev->key == KEY_LEFT && v->nfiles) { v->cur = (v->cur + v->nfiles - 1) % v->nfiles; viewer_load(v); }
        break;
    case EV_MOUSE_WHEEL:
        if (v->img) {
            float fit = MIN((float)a->win->cw / (float)v->img->w, (float)(a->win->ch - 56) / (float)v->img->h);
            float z = v->zoom > 0 ? v->zoom : MIN(fit, 1.0f);
            z *= ev->wheel > 0 ? 1.25f : 0.8f;
            v->zoom = CLAMP(z, 0.05f, 16.0f);
        }
        break;
    case EV_MOUSE_DOWN: v->drag = ev->y < a->win->ch - 56; v->dx = ev->x; v->dy = ev->y; break;
    case EV_MOUSE_UP: v->drag = false; break;
    case EV_MOUSE_MOVE:
        if (v->drag) { v->panx += ev->x - v->dx; v->pany += ev->y - v->dy; v->dx = ev->x; v->dy = ev->y; }
        break;
    case EV_TIMER: {
        int old = v->nfiles;
        viewer_scan(v);
        if (v->nfiles != old && !v->img && v->nfiles) { v->cur = v->nfiles - 1; viewer_load(v); }
        return v->nfiles != old;
    }
    }
    return 1;
}

int imageview_main(void *arg)
{
    struct viewer *v = kzalloc(sizeof(*v));
    strcpy(v->dir, "/home/user/Pictures");
    const char *want = NULL;
    if (arg) {
        vfs_dirname(arg, v->dir, sizeof(v->dir));
        want = vfs_basename(arg);
    }
    viewer_scan(v);
    v->cur = v->nfiles ? v->nfiles - 1 : -1;
    if (want)
        for (int i = 0; i < v->nfiles; i++) if (!strcmp(v->files[i], want)) v->cur = i;
    viewer_load(v);
    kfree(arg);
    app_t a = { 0 };
    a.data = v;
    a.win = wm_create("Images", 820, 580, WF_RESIZABLE);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_IMAGE);
    wm_set_timer(a.win, 2000);
    a.on_paint = viewer_paint;
    a.on_event = viewer_event;
    int r = app_run(&a);
    surface_free(v->img);
    kfree(v);
    return r;
}

/* ========================================================================
 * System Log
 * ====================================================================== */

const char *memchr_(const char *s, char c, int n);

struct logview {
    const char *lines[2048];
    int lens[2048];
    char *buf;
    size_t len;
    int scroll;         /* lines from the bottom */
    char filter[48];
    bool follow;
};

static void logview_paint(app_t *a, surface_t *s)
{
    struct logview *l = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, HEX(0x0F1120));
    gfx_fill(s, 0, 0, W, 50, theme.panel);
    rect_t fr = R(12, 8, 300, 34);
    ui_textbox(u, fr, l->filter, sizeof(l->filter), "Filter...");
    ui_checkbox(u, R(330, 12, 150, 26), "Follow", &l->follow);
    char info[48];
    snprintf(info, sizeof(info), "%lu bytes", l->len);
    gfx_text_right(s, font_ui, W - 14, 18, info, theme.text_faint);

    /* collect matching lines (pointers into buf) */
    const char **lines = l->lines;
    int *lens = l->lens;
    int n = 0;
    const char *p = l->buf;
    while (p && *p && n < 2048) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        bool ok = !l->filter[0];
        if (!ok) {
            char tmp[256];
            int c = MIN(len, 255);
            memcpy(tmp, p, (size_t)c);
            tmp[c] = 0;
            ok = strstr_ci(tmp, l->filter) != NULL;
        }
        if (ok) { lines[n] = p; lens[n] = len; n++; }
        if (!e) break;
        p = e + 1;
    }
    int lh = font_height(font_mono) + 2;
    int view = (H - 60) / lh;
    if (u->wheel) { l->scroll = CLAMP(l->scroll + u->wheel * 3, 0, MAX(0, n - view)); l->follow = l->scroll == 0; }
    if (l->follow) l->scroll = 0;
    int first = MAX(0, n - view - l->scroll);
    for (int i = 0; i < view && first + i < n; i++) {
        const char *ln = lines[first + i];
        int y = 56 + i * lh;
        /* timestamp dim, rest bright */
        int ts = (ln[0] == '[') ? (int)(strchr(ln, ']') ? strchr(ln, ']') - ln + 1 : 0) : 0;
        if (ts > lens[first + i]) ts = 0;
        int x = gfx_text_n(s, font_mono, 12, y, ln, ts, theme.text_faint);
        const char *rest = ln + ts;
        int rl = lens[first + i] - ts;
        /* subsystem prefix in accent */
        const char *colon = memchr_(rest, ':', rl);
        if (colon && colon - rest < 14) {
            x = gfx_text_n(s, font_mono, x, y, rest, (int)(colon - rest) + 1, theme.accent2);
            rl -= (int)(colon - rest) + 1;
            rest = colon + 1;
        }
        gfx_text_n(s, font_mono, x, y, rest, rl, theme.text);
    }
}

const char *memchr_(const char *s, char c, int n)
{
    for (int i = 0; i < n; i++) if (s[i] == c) return s + i;
    return NULL;
}

static int logview_event(app_t *a, struct gui_event *ev)
{
    struct logview *l = a->data;
    if (ev->type == EV_TIMER) {
        size_t old = l->len;
        l->len = klog_read(l->buf, 65536);
        return l->len != old;
    }
    return 1;
}

int logview_main(void *arg)
{
    UNUSED(arg);
    struct logview *l = kzalloc(sizeof(*l));
    l->buf = kmalloc(65536);
    l->len = klog_read(l->buf, 65536);
    l->follow = true;
    app_t a = { 0 };
    a.data = l;
    a.win = wm_create("System Log", 860, 520, WF_RESIZABLE);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_LOG);
    wm_set_timer(a.win, 1000);
    a.on_paint = logview_paint;
    a.on_event = logview_event;
    int r = app_run(&a);
    kfree(l->buf);
    kfree(l);
    return r;
}

/* ========================================================================
 * Settings
 * ====================================================================== */

void wallpaper_thumb(surface_t *s, int style);

struct settings {
    int page;
    surface_t *thumbs[8];
    int tz_index;
};

static const struct { const char *name; int off; } zones[] = {
    { "UTC", 0 }, { "London (UTC+1, summer)", 60 }, { "Berlin (UTC+2, summer)", 120 }, { "Berlin (UTC+1, winter)", 60 },
    { "Moscow (UTC+3)", 180 }, { "Dubai (UTC+4)", 240 }, { "Delhi (UTC+5:30)", 330 }, { "Tokyo (UTC+9)", 540 },
    { "Sydney (UTC+10)", 600 }, { "New York (UTC-4, summer)", -240 }, { "Los Angeles (UTC-7, summer)", -420 },
};
extern int rtc_utc_offset_min;

static void section(surface_t *s, int x, int y, const char *title, const char *sub)
{
    gfx_text(s, font_bold_lg, x, y, title, theme.text);
    if (sub) gfx_text(s, font_ui, x, y + 30, sub, theme.text_dim);
}

static void settings_paint(app_t *a, surface_t *s)
{
    struct settings *st = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, theme.bg);
    int sbw = 210;
    gfx_fill(s, 0, 0, sbw, H, theme.panel);
    gfx_fill(s, sbw, 0, 1, H, ALPHA(0xFFFFFF, 12));
    static const struct { const char *n; int icon; } pages[] = {
        { "Personalization", ICON_PAINT }, { "Keyboard", ICON_TEXT }, { "Date & time", ICON_CLOCK },
        { "Network", ICON_NETWORK }, { "Sound", ICON_SOUND }, { "System", ICON_INFO },
    };
    gfx_text(s, font_title, 20, 18, "Settings", theme.text);
    for (int i = 0; i < (int)ARRAY_SIZE(pages); i++) {
        rect_t r = R(10, 62 + i * 42, sbw - 20, 38);
        if (ui_list_item(u, r, st->page == i)) st->page = i;
        icon_draw(s, pages[i].icon, r.x + 10, r.y + 8, 22);
        gfx_text(s, font_ui_md, r.x + 44, r.y + 11, pages[i].n, theme.text);
    }
    int x = sbw + 32, y = 24;
    switch (st->page) {
    case 0: {
        section(s, x, y, "Wallpaper", "Generated live by the compositor");
        y += 64;
        int tw = 150, th = 94;
        for (int i = 0; i < wallpaper_count; i++) {
            rect_t r = R(x + (i % 3) * (tw + 16), y + (i / 3) * (th + 36), tw, th);
            if (!st->thumbs[i]) {
                st->thumbs[i] = surface_new(tw, th);
                wallpaper_thumb(st->thumbs[i], i);
            }
            gfx_blit(s, r.x, r.y, st->thumbs[i], 0, 0, tw, th);
            bool sel = wallpaper_style == i;
            gfx_round_outline(s, r.x - 2, r.y - 2, r.w + 4, r.h + 4, 8, sel ? theme.accent : ALPHA(0xFFFFFF, ui_hover(u, r) ? 60 : 20));
            if (sel) gfx_round_outline(s, r.x - 3, r.y - 3, r.w + 6, r.h + 6, 9, theme.accent);
            gfx_text(s, font_ui, r.x, r.y + th + 8, wallpaper_names[i], sel ? theme.text : theme.text_dim);
            if (ui_hover(u, r) && u->mreleased && !sel) {
                wallpaper_style = i;
                app_launch("@wallpaper");
            }
        }
        y += 2 * (th + 36) + 10;
        section(s, x, y, "Accent colour", NULL);
        y += 40;
        for (int i = 0; i < accent_count; i++) {
            rect_t r = R(x + i * 52, y, 40, 40);
            gfx_circle(s, (float)r.x + 20, (float)r.y + 20, 18, accent_choices[i]);
            if (accent_index == i) gfx_ring(s, (float)r.x + 20, (float)r.y + 20, 22, 2.5f, theme.text);
            if (ui_hover(u, r) && u->mreleased) {
                theme_set_accent(i);
                wm_invalidate_all();
            }
        }
        y += 64;
        bool tr = ui_transparency, an = ui_animations;
        gfx_text(s, font_ui_md, x, y + 8, "Transparency effects", theme.text);
        if (ui_toggle(u, R(x + 300, y, 60, 36), &tr)) { ui_transparency = tr; wm_invalidate_all(); }
        y += 44;
        gfx_text(s, font_ui_md, x, y + 8, "Animations", theme.text);
        if (ui_toggle(u, R(x + 300, y, 60, 36), &an)) ui_animations = an;
        break;
    }
    case 1: {
        section(s, x, y, "Keyboard", "Also switchable from the taskbar (DE/US)");
        y += 70;
        for (int i = 0; i < 2; i++) {
            rect_t r = R(x, y + i * 50, 320, 44);
            if (ui_list_item(u, r, keyboard_layout == i)) { keyboard_layout = i; wm_invalidate_all(); }
            gfx_text(s, font_ui_md, r.x + 16, r.y + 13, keymap_name(i), theme.text);
        }
        y += 120;
        gfx_text(s, font_ui_bold, x, y, "Shortcuts", theme.text);
        static const char *sc[][2] = {
            { "Win", "Start menu" }, { "Ctrl+Alt+T", "Terminal" }, { "Alt+Tab", "Switch windows" },
            { "Alt+F4", "Close window" }, { "Win+D", "Show desktop" }, { "Win+E", "Files" },
            { "Win+Up / Down", "Maximize / restore" }, { "Print Screen", "Screenshot to ~/Pictures" },
            { "Ctrl+Alt+Del", "System Monitor" },
        };
        for (unsigned i = 0; i < ARRAY_SIZE(sc); i++) {
            int yy = y + 28 + (int)i * 26;
            gfx_round_rect(s, x, yy - 2, font_text_width(font_ui_md, sc[i][0]) + 16, 22, 6, theme.surface2);
            gfx_text(s, font_ui_md, x + 8, yy + 1, sc[i][0], theme.text);
            gfx_text(s, font_ui, x + 170, yy + 1, sc[i][1], theme.text_dim);
        }
        break;
    }
    case 2: {
        section(s, x, y, "Date & time", "The hardware clock runs in UTC");
        y += 70;
        struct datetime dt;
        rtc_now(&dt);
        char b[64];
        snprintf(b, sizeof(b), "%02d:%02d:%02d", dt.hour, dt.minute, dt.second);
        gfx_text(s, font_huge, x, y - 10, b, theme.text);
        y += 90;
        gfx_text(s, font_ui_bold, x, y, "Time zone", theme.text);
        y += 28;
        int colw = (W - x - 30) / 2;
        for (unsigned i = 0; i < ARRAY_SIZE(zones); i++) {
            rect_t r = R(x + (int)(i % 2) * colw, y + (int)(i / 2) * 38, colw - 10, 34);
            if (ui_list_item(u, r, rtc_utc_offset_min == zones[i].off && st->tz_index == (int)i)) {
                rtc_utc_offset_min = zones[i].off;
                st->tz_index = (int)i;
                wm_invalidate_all();
            }
            gfx_text(s, font_ui, r.x + 12, r.y + 9, zones[i].name, theme.text);
        }
        break;
    }
    case 3: {
        section(s, x, y, "Network", NULL);
        y += 50;
        struct net_info ni;
        if (!net_get_info(&ni)) {
            gfx_text(s, font_ui_lg, x, y, "No supported network adapter found.", theme.text_dim);
            gfx_text(s, font_ui, x, y + 26, "ZenithOS drives Intel, Realtek, AMD, NE2000, Tulip, virtio, VMware and USB adapters", theme.text_faint);
            gfx_text(s, font_ui, x, y + 46, "(type netcards in the Terminal). No Wi-Fi yet: plug in a phone and turn on USB tethering.", theme.text_faint);
            break;
        }
        char ip[20], gw[20], dns[20], b[64];
        ip_to_str(ni.ip, ip); ip_to_str(ni.gateway, gw); ip_to_str(ni.dns, dns);
        const char *k[] = { "Adapter", "MAC address", "Link", "IP address", "Gateway", "DNS" };
        char mac[24];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
        const char *vals[] = { ni.driver, mac, ni.link ? "Connected" : "Disconnected", ni.ip ? ip : "Not configured", gw, dns };
        for (int i = 0; i < 6; i++) {
            gfx_text(s, font_ui, x, y + i * 28, k[i], theme.text_dim);
            gfx_text(s, font_ui_md, x + 160, y + i * 28, vals[i], theme.text);
        }
        y += 6 * 28 + 12;
        snprintf(b, sizeof(b), "%s: received %lu packets, sent %lu", ni.ifname, ni.rx_packets, ni.tx_packets);
        gfx_text(s, font_ui, x, y, b, theme.text_faint);
        if (ui_button(u, R(x, y + 30, 180, 38), "Renew address (DHCP)", BTN_PRIMARY)) app_launch("netinfo");
        y += 90;
        int n = net_iface_count();
        if (n > 1) {
            gfx_text(s, font_ui_md, x, y, "All adapters", theme.text);
            y += 30;
            for (int i = 0; i < n; i++) {
                struct net_info a;
                if (!net_iface_get(i, &a)) continue;
                char line[128], aip[20];
                ip_to_str(a.ip, aip);
                snprintf(line, sizeof(line), "%s  %s", a.ifname, a.driver);
                gfx_text(s, font_ui, x, y, line, a.link ? theme.text : theme.text_faint);
                snprintf(line, sizeof(line), "%s%s", !a.link ? "disconnected" : a.ip ? aip : "configuring...",
                         a.is_default ? "  (default)" : "");
                gfx_text(s, font_ui, x + 420, y, line, a.link ? theme.text_dim : theme.text_faint);
                y += 24;
            }
        }
        break;
    }
    case 4: {
        section(s, x, y, "Sound", audio_available() ? hda_name() : "No sound card found (ZenithOS drives Intel HD Audio)");
        y += 74;
        if (!audio_available()) break;
        gfx_text(s, font_ui_md, x, y + 8, "Volume", theme.text);
        float v = (float)audio_volume();
        if (ui_slider(u, R(x + 120, y + 6, 300, 24), &v, 0, 100)) audio_set_volume((int)(v + 0.5f));
        char b[16];
        snprintf(b, sizeof(b), "%d%%", audio_volume());
        gfx_text(s, font_ui_md, x + 440, y + 8, b, theme.text_dim);
        y += 48;
        bool m = audio_muted();
        gfx_text(s, font_ui_md, x, y + 8, "Mute", theme.text);
        if (ui_toggle(u, R(x + 360, y, 60, 36), &m)) audio_set_muted(m);
        y += 60;
        gfx_text(s, font_ui_bold, x, y, "Try it", theme.text);
        y += 28;
        static const struct { const char *n; int snd; } tests[] = {
            { "Startup", SND_STARTUP }, { "Notification", SND_NOTIFY }, { "Success", SND_SUCCESS }, { "Error", SND_ERROR },
        };
        for (int i = 0; i < 4; i++)
            if (ui_button(u, R(x + i * 128, y, 120, 36), tests[i].n, BTN_NORMAL)) audio_sound(tests[i].snd);
        y += 56;
        if (ui_button(u, R(x, y, 200, 38), "Open Piano", BTN_PRIMARY)) app_launch("piano");
        u->want_repaint = u->want_repaint || audio_level() > 0.01f;
        break;
    }
    case 5: {
        section(s, x, y, "System", NULL);
        y += 50;
        char b[96];
        logo_draw(s, x, y, 72, false);
        gfx_text(s, font_title, x + 90, y + 8, "ZenithOS " ZENITH_VERSION " \"" ZENITH_CODENAME "\"", theme.text);
        snprintf(b, sizeof(b), "%s  ·  %d cores  ·  %lu MiB", cpu_info.brand, ncpus, (pmm_total_pages() * 4096) >> 20);
        gfx_text(s, font_ui, x + 90, y + 38, b, theme.text_dim);
        y += 110;
        if (ui_button(u, R(x, y, 150, 40), "About", BTN_NORMAL)) app_launch("about");
        if (ui_button(u, R(x + 160, y, 150, 40), "System Log", BTN_NORMAL)) app_launch("logs");
        y += 56;
        if (ui_button(u, R(x, y, 150, 40), "Restart", BTN_NORMAL)) app_launch("@reboot");
        if (ui_button(u, R(x + 160, y, 150, 40), "Shut down", BTN_DANGER)) app_launch("@poweroff");
        break;
    }
    }
}

static int settings_event(app_t *a, struct gui_event *ev)
{
    struct settings *st = a->data;
    if (ev->type == EV_TIMER) return st->page == 2 || st->page == 3;
    return 1;
}

int settings_main(void *arg)
{
    UNUSED(arg);
    struct settings *st = kzalloc(sizeof(*st));
    app_t a = { 0 };
    a.data = st;
    a.win = wm_create("Settings", 900, 600, 0);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_SETTINGS);
    wm_set_timer(a.win, 1000);
    a.on_paint = settings_paint;
    a.on_event = settings_event;
    int r = app_run(&a);
    for (int i = 0; i < 8; i++) surface_free(st->thumbs[i]);
    kfree(st);
    return r;
}

/* ========================================================================
 * Network
 * ====================================================================== */

struct netapp {
    volatile bool closed;
    char host[64];
    char log[16][96];
    int nlog;
    volatile bool busy;
    struct app *app;
};

static void netlog(struct netapp *n, const char *fmt, ...)
{
    char b[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n->nlog == 16) { memmove(n->log[0], n->log[1], sizeof(n->log[0]) * 15); n->nlog--; }
    strlcpy(n->log[n->nlog++], b, sizeof(n->log[0]));
    if (n->closed) return;
    struct gui_event ev = { 0 };
    ev.type = EV_TIMER;
    wm_post_event(n->app->win, &ev);
}

static int net_worker(void *arg)
{
    struct netapp *n = arg;
    char ip[20];
    if (!n->host[0]) {
        netlog(n, "Requesting an address via DHCP...");
        if (net_dhcp(4000)) {
            struct net_info ni;
            net_get_info(&ni);
            ip_to_str(ni.ip, ip);
            netlog(n, "Got address %s", ip);
        } else netlog(n, "No DHCP answer.");
    } else {
        uint32_t a;
        netlog(n, "Resolving %s...", n->host);
        if (!net_resolve(n->host, &a, 3000)) { netlog(n, "Could not resolve %s", n->host); n->busy = false; return 0; }
        ip_to_str(a, ip);
        for (int i = 0; i < 4; i++) {
            int rtt = net_ping(a, (uint16_t)i, 2000);
            if (rtt >= 0) netlog(n, "Reply from %s: seq=%d time=%d ms", ip, i, rtt);
            else netlog(n, "Request to %s timed out", ip);
            sched_sleep(400);
        }
    }
    n->busy = false;
    return 0;
}

static void net_paint(app_t *a, surface_t *s)
{
    struct netapp *n = a->data;
    ui_t *u = &a->ui;
    int W = s->w;
    gfx_clear(s, theme.bg);
    struct net_info ni;
    bool have = net_get_info(&ni);
    icon_draw(s, ICON_NETWORK, 24, 20, 40);
    gfx_text(s, font_title, 80, 22, have ? ni.driver : "No network adapter", theme.text);
    if (have) {
        char ip[20], b[128];
        ip_to_str(ni.ip, ip);
        snprintf(b, sizeof(b), "%s  ·  %s  ·  rx %lu / tx %lu packets", ni.link ? "Link up" : "Link down",
                 ni.ip ? ip : "no address", ni.rx_packets, ni.tx_packets);
        gfx_text(s, font_ui, 80, 48, b, theme.text_dim);
    }
    int y = 90;
    if (ui_button(u, R(24, y, 150, 38), "DHCP", BTN_PRIMARY) && !n->busy && have) {
        n->busy = true;
        n->host[0] = 0;
        thread_create("netapp", net_worker, n);
    }
    static char host[64] = "10.0.2.2";
    ui_textbox(u, R(190, y, W - 330, 38), host, sizeof(host), "host to ping");
    if (ui_button(u, R(W - 124, y, 100, 38), "Ping", BTN_NORMAL) && !n->busy && have) {
        n->busy = true;
        strlcpy(n->host, host, sizeof(n->host));
        thread_create("netapp", net_worker, n);
    }
    y += 56;
    gfx_round_rect(s, 24, y, W - 48, s->h - y - 20, 10, HEX(0x0F1120));
    for (int i = 0; i < n->nlog; i++) gfx_text(s, font_mono, 36, y + 12 + i * 18, n->log[i], theme.text);
    if (n->busy) gfx_text_right(s, font_ui, W - 36, y + 12, "working...", theme.accent2);
}

static int net_event(app_t *a, struct gui_event *ev)
{
    UNUSED(a); UNUSED(ev);
    return 1;
}

static void net_close(app_t *a)
{
    struct netapp *n = a->data;
    n->closed = true;
    while (n->busy) sched_sleep(50);
    a->quit = true;
}

int netinfo_main(void *arg)
{
    UNUSED(arg);
    struct netapp *n = kzalloc(sizeof(*n));
    app_t a = { 0 };
    n->app = &a;
    a.data = n;
    a.win = wm_create("Network", 640, 460, 0);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_NETWORK);
    wm_set_timer(a.win, 1000);
    a.on_paint = net_paint;
    a.on_event = net_event;
    a.on_close = net_close;
    int r = app_run(&a);
    kfree(n);
    return r;
}
