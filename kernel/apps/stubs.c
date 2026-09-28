/* Temporary placeholders for apps that are not written yet. */
#include <wm.h>
#include <sched.h>

static void stub_paint(app_t *a, surface_t *s)
{
    gfx_clear(s, theme.bg);
    gfx_text_center(s, font_title, R(0, 0, s->w, s->h), a->data, theme.text_dim);
}

static int stub(const char *name)
{
    app_t a = { 0 };
    a.win = wm_create(name, 480, 300, WF_RESIZABLE);
    a.data = (void *)name;
    a.on_paint = stub_paint;
    return app_run(&a);
}

#define STUB(fn, title) __attribute__((weak)) int fn(void *arg) { UNUSED(arg); return stub(title); }
STUB(terminal_main, "Terminal")
STUB(files_main, "Files")
STUB(editor_main, "Text Editor")
STUB(sysmon_main, "System Monitor")
STUB(demo3d_main, "Zenith 3D")
STUB(mandel_main, "Mandelbrot")
STUB(tetris_main, "Blocks")
STUB(snake_main, "Snake")
STUB(paint_main, "Paint")
STUB(calc_main, "Calculator")
STUB(clock_main, "Clock")
STUB(imageview_main, "Images")
STUB(logview_main, "System Log")
STUB(settings_main, "Settings")
STUB(netinfo_main, "Network")
STUB(user_app_main, "Plasma")

__attribute__((weak)) process_t *proc_spawn(const char *path, int argc, char **argv, struct tty *tty, const char *cwd)
{
    UNUSED(path); UNUSED(argc); UNUSED(argv); UNUSED(tty); UNUSED(cwd);
    return NULL;
}
