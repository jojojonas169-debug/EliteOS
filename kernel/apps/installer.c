/*
 * Installer: copies the running system (UEFI loader, kernel, initrd) onto
 * a disk so the computer boots ZenithOS without the installation medium.
 *
 * Two ways: add ZenithOS to an existing FAT32 volume (files are kept, the
 * partition is marked as an EFI System Partition), or erase the disk and
 * create a fresh GPT with one FAT32 EFI System Partition.
 */
#include <wm.h>
#include <block.h>
#include <vfs.h>
#include <mm.h>
#include <sched.h>
#include <bootinfo.h>

extern struct bootinfo *boot_info;

/* ------------------------------------------------------------------------
 * the installation itself (also used by the shell's `install`)
 * ---------------------------------------------------------------------- */

static void step(struct install_progress *p, int k, const char *msg)
{
    p->step = k;
    p->pct = p->nsteps ? k * 100 / p->nsteps : 0;
    strlcpy(p->msg, msg, sizeof(p->msg));
    klog("install: %s", msg);
}

static int fail(struct install_progress *p, const char *msg)
{
    strlcpy(p->msg, msg, sizeof(p->msg));
    klog("install: failed: %s", msg);
    p->failed = true;
    p->done = true;
    return E_IO;
}

static bool on_disk(struct blockdev *d, struct blockdev *disk) { return d == disk || d->parent == disk; }

static struct fs_mount *mount_on(struct blockdev *disk)
{
    for (struct fs_mount *m = vfs_mounts(); m; m = m->next) {
        struct blockdev *d = blk_find(m->dev);
        if (d && on_disk(d, disk)) return m;
    }
    return NULL;
}

static int mount_somewhere(struct blockdev *d, char *path, size_t n)
{
    for (int k = 1; k < 10; k++) {
        if (k == 1) strlcpy(path, "/disk", n);
        else snprintf(path, n, "/disk%d", k);
        struct vfs_stat st;
        if (!vfs_stat(path, &st) && vfs_mount_of(path)) continue;
        if (!fat_mount(d, path)) return 0;
    }
    return E_EXIST;
}

static int copy_blob(const char *path, uint64_t phys, uint64_t size)
{
    char dir[VFS_PATH_MAX];
    vfs_dirname(path, dir, sizeof(dir));
    /* create every level of the directory */
    for (char *s = dir + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        vfs_mkdir(dir);
        *s = '/';
    }
    vfs_mkdir(dir);
    return vfs_write_file(path, P2V(phys), size);
}

int zenith_install(struct blockdev *disk, bool erase, struct install_progress *p)
{
    p->nsteps = 7;
    p->done = p->failed = false;
    if (!boot_info->loader_file_size || !boot_info->kernel_file_size || !boot_info->initrd_size)
        return fail(p, "The boot files are not available (was ZenithOS started by its own loader?)");
    if (disk->parent) disk = disk->parent;
    char root[64] = "";
    struct blockdev *target = NULL;

    if (erase) {
        step(p, 1, "Unmounting the disk");
        for (struct fs_mount *m; (m = mount_on(disk));) {
            char path[64];
            strlcpy(path, m->path, sizeof(path));
            if (vfs_umount(path)) return fail(p, "Could not unmount the disk");
        }
        step(p, 2, "Creating the partition table (GPT)");
        if (gpt_create_single(disk, "ZenithOS", true)) return fail(p, "Could not write the partition table");
        for (int i = 0; i < blk_count(); i++)
            if (blk_get(i)->parent == disk) target = blk_get(i);
        if (!target) return fail(p, "The new partition did not show up");
        step(p, 3, "Formatting the EFI System Partition (FAT32)");
        if (fat_mkfs(target, "ZENITHOS")) return fail(p, "Formatting failed (is the disk at least 64 MiB?)");
        if (mount_somewhere(target, root, sizeof(root))) return fail(p, "Could not mount the new file system");
    } else {
        step(p, 1, "Looking for a FAT32 volume");
        struct fs_mount *m = mount_on(disk);
        if (m) {
            target = blk_find(m->dev);
            strlcpy(root, m->path, sizeof(root));
        } else {
            for (int i = 0; i < blk_count() && !target; i++) {
                struct blockdev *d = blk_get(i);
                if (on_disk(d, disk) && fat_probe(d, NULL, 0)) target = d;
            }
            if (!target) return fail(p, "There is no FAT32 volume on this disk; choose \"Erase\" instead");
            if (mount_somewhere(target, root, sizeof(root))) return fail(p, "Could not mount the volume");
        }
        step(p, 2, "Marking the partition as bootable");
        if (part_make_bootable(target)) return fail(p, "Could not update the partition table");
        step(p, 3, "Keeping your files");
    }

    char path[VFS_PATH_MAX];
    step(p, 4, "Copying the boot loader");
    snprintf(path, sizeof(path), "%s/EFI/BOOT/BOOTX64.EFI", root);
    if (copy_blob(path, boot_info->loader_file_phys, boot_info->loader_file_size)) return fail(p, "Could not write the boot loader");
    step(p, 5, "Copying the kernel");
    snprintf(path, sizeof(path), "%s/zenith/kernel.elf", root);
    if (copy_blob(path, boot_info->kernel_file_phys, boot_info->kernel_file_size)) return fail(p, "Could not write the kernel");
    step(p, 6, "Copying the system image");
    snprintf(path, sizeof(path), "%s/zenith/initrd.tar", root);
    if (copy_blob(path, boot_info->initrd_phys, boot_info->initrd_size)) return fail(p, "Could not write the system image");
    char cfg[64];
    int n = snprintf(cfg, sizeof(cfg), "resolution=%ux%u\n", boot_info->fb.width, boot_info->fb.height);
    snprintf(path, sizeof(path), "%s/zenith/boot.cfg", root);
    vfs_write_file(path, cfg, (size_t)n);

    step(p, 7, "Writing everything to the disk");
    if (vfs_sync()) return fail(p, "Writing to the disk failed");
    char sz[24];
    blk_format_size(disk->sectors * SECTOR_SIZE, sz, sizeof(sz));
    snprintf(p->msg, sizeof(p->msg), "ZenithOS is installed on %s (%s, %s)", disk->name, disk->model, sz);
    klog("install: %s", p->msg);
    p->pct = 100;
    p->done = true;
    return 0;
}

/* ------------------------------------------------------------------------
 * the app
 * ---------------------------------------------------------------------- */

struct inst {
    int page;               /* 0 intro, 1 choose, 2 confirm erase, 3 progress */
    int disk;               /* index into blk registry */
    bool erase;
    struct install_progress prog;
    struct blockdev *target;
    bool running;
};

static int worker(void *arg)
{
    struct inst *in = arg;
    zenith_install(in->target, in->erase, &in->prog);
    in->running = false;
    return 0;
}

static void describe(struct blockdev *disk, char *out, size_t n, bool *has_fat)
{
    *has_fat = false;
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < blk_count(); i++) {
        struct blockdev *d = blk_get(i);
        if (!on_disk(d, disk)) continue;
        char label[16] = "";
        if (!fat_probe(d, label, sizeof(label))) continue;
        *has_fat = true;
        const char *mp = "";
        for (struct fs_mount *m = vfs_mounts(); m; m = m->next)
            if (!strcmp(m->dev, d->name)) mp = m->path;
        o += (size_t)snprintf(out + o, n - o, "%sFAT32 volume %s%s%s%s", o ? ", " : "", label[0] ? label : d->name,
                              mp[0] ? " (mounted at " : "", mp, mp[0] ? ")" : "");
        if (o >= n) break;
    }
    if (!out[0]) {
        bool parts = false;
        for (int i = 0; i < blk_count(); i++) if (blk_get(i)->parent == disk) parts = true;
        strlcpy(out, parts ? "Partitions without a FAT32 file system" : "Empty disk", n);
    }
}

static void check(surface_t *s, int x, int y, int state)
{
    /* 0 pending, 1 active, 2 done */
    if (state == 2) {
        gfx_circle(s, (float)x + 11, (float)y + 11, 11, theme.success);
        gfx_line_w(s, (float)x + 6, (float)y + 11.5f, (float)x + 10, (float)y + 15.5f, 2.2f, 0xFFFFFFFFu);
        gfx_line_w(s, (float)x + 10, (float)y + 15.5f, (float)x + 16.5f, (float)y + 7.5f, 2.2f, 0xFFFFFFFFu);
    } else if (state == 1) {
        gfx_ring(s, (float)x + 11, (float)y + 11, 10, 2.5f, theme.accent);
        float a = (float)(uptime_ms() % 1000) / 1000.0f * 6.2831853f;
        gfx_circle(s, (float)x + 11 + 6 * (float)k_cos(a), (float)y + 11 + 6 * (float)k_sin(a), 3, theme.accent);
    } else {
        gfx_ring(s, (float)x + 11, (float)y + 11, 10, 2, theme.surface2);
    }
}

static void inst_paint(app_t *a, surface_t *s)
{
    struct inst *in = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, theme.bg);

    /* left banner */
    int bw = 250;
    gfx_vgradient(s, 0, 0, bw, H, color_lerp(theme.accent, HEX(0x101328), 70), HEX(0x151827));
    gfx_circle(s, 40, (float)H - 20, 110, ALPHA(0xFFFFFF, 8));
    logo_draw(s, 28, 32, 64, false);
    gfx_text(s, font_title, 28, 112, "Install", 0xFFFFFFFFu);
    gfx_text(s, font_title, 28, 142, "ZenithOS", 0xFFFFFFFFu);
    static const char *stages[] = { "Welcome", "Choose a disk", "Confirm", "Install" };
    int stage = in->page == 3 ? 3 : in->page;
    for (int i = 0; i < 4; i++) {
        int y = 210 + i * 40;
        bool cur = i == stage, past = i < stage;
        gfx_circle(s, 40, (float)y + 9, 6, cur ? 0xFFFFFFFFu : past ? ALPHA(0xFFFFFF, 160) : ALPHA(0xFFFFFF, 50));
        gfx_text(s, cur ? font_ui_bold : font_ui, 58, y, stages[i], cur ? 0xFFFFFFFFu : ALPHA(0xFFFFFF, past ? 170 : 110));
    }

    int x = bw + 36, y = 34, cw = W - x - 36;
    int by = H - 62;
    switch (in->page) {
    case 0: {
        gfx_text(s, font_bold_lg, x, y, "Put ZenithOS on this computer", theme.text);
        y += 44;
        gfx_text_wrap(s, font_ui_lg, R(x, y, cw, 80),
                      "The installer copies the running system - boot loader, kernel and system image - to a disk. "
                      "Afterwards the computer starts ZenithOS straight from that disk, without the USB stick or ISO.",
                      theme.text_dim, 4);
        y += 100;
        static const struct { int icon; const char *t, *d; } pts[] = {
            { ICON_DISK, "Keep your files", "Add ZenithOS to an existing FAT32 volume" },
            { ICON_TRASH, "Or start fresh", "Erase a disk: new GPT and EFI System Partition" },
            { ICON_SETTINGS, "Settings travel along", "Wallpaper, accent colour and layout are kept on the disk" },
        };
        for (int i = 0; i < 3; i++) {
            int yy = y + i * 62;
            gfx_round_rect(s, x, yy, cw, 54, 10, theme.panel);
            icon_draw(s, pts[i].icon, x + 12, yy + 11, 32);
            gfx_text(s, font_ui_bold, x + 58, yy + 9, pts[i].t, theme.text);
            gfx_text(s, font_ui, x + 58, yy + 30, pts[i].d, theme.text_dim);
        }
        if (!blk_count())
            gfx_text(s, font_ui_md, x, by + 10, "No disks found. ZenithOS supports SATA (AHCI) disks.", theme.warning);
        else if (ui_button(u, R(W - 36 - 130, by, 130, 40), "Next", BTN_PRIMARY)) in->page = 1;
        break;
    }
    case 1: {
        gfx_text(s, font_bold_lg, x, y, "Where should ZenithOS go?", theme.text);
        y += 50;
        int shown = 0;
        bool sel_fat = false;
        for (int i = 0; i < blk_count(); i++) {
            struct blockdev *d = blk_get(i);
            if (d->parent) continue;
            if (in->disk < 0) in->disk = i;
            rect_t r = R(x, y + shown * 78, cw, 70);
            bool sel = in->disk == i;
            if (ui_list_item(u, r, sel)) in->disk = i;
            gfx_round_outline(s, r.x, r.y, r.w, r.h, 10, sel ? theme.accent : ALPHA(0xFFFFFF, 16));
            icon_draw(s, ICON_DISK, r.x + 14, r.y + 13, 44);
            char t[96], sz[24], desc[160];
            bool has_fat;
            blk_format_size(d->sectors * SECTOR_SIZE, sz, sizeof(sz));
            snprintf(t, sizeof(t), "%s  ·  %s  ·  %s", d->name, d->model, sz);
            describe(d, desc, sizeof(desc), &has_fat);
            if (sel) sel_fat = has_fat;
            gfx_text(s, font_ui_bold, r.x + 72, r.y + 14, t, theme.text);
            gfx_text_ellipsis(s, font_ui, r.x + 72, r.y + 38, r.w - 90, desc, theme.text_dim);
            shown++;
        }
        y += shown * 78 + 18;
        gfx_text(s, font_ui_bold, x, y, "How?", theme.text);
        y += 28;
        if (!sel_fat) in->erase = true;
        for (int k = 0; k < 2; k++) {
            bool erase = k == 1;
            rect_t r = R(x, y + k * 50, cw, 44);
            bool avail = erase || sel_fat;
            if (ui_list_item(u, r, in->erase == erase) && avail) in->erase = erase;
            gfx_ring(s, (float)r.x + 22, (float)r.y + 22, 8, 2, avail ? theme.text_dim : theme.surface2);
            if (in->erase == erase) gfx_circle(s, (float)r.x + 22, (float)r.y + 22, 4.5f, theme.accent);
            gfx_text(s, font_ui_md, r.x + 42, r.y + 13,
                     erase ? "Erase the disk and install" : "Install next to the files on the FAT32 volume",
                     avail ? (erase ? theme.danger : theme.text) : theme.text_faint);
        }
        if (ui_button(u, R(x, by, 110, 40), "Back", BTN_NORMAL)) in->page = 0;
        if (ui_button(u, R(W - 36 - 130, by, 130, 40), in->erase ? "Next" : "Install", BTN_PRIMARY)) {
            in->target = blk_get(in->disk);
            if (in->erase) in->page = 2;
            else {
                in->page = 3;
                in->running = true;
                thread_create("installer", worker, in);
            }
        }
        break;
    }
    case 2: {
        struct blockdev *d = in->target;
        gfx_text(s, font_bold_lg, x, y, "Erase this disk?", theme.text);
        y += 50;
        gfx_round_rect(s, x, y, cw, 110, 12, color_lerp(theme.danger, theme.bg, 200));
        gfx_round_outline(s, x, y, cw, 110, 12, ALPHA(0xFF5C7A, 90));
        icon_draw(s, ICON_DISK, x + 18, y + 22, 52);
        char t[96], sz[24];
        blk_format_size(d->sectors * SECTOR_SIZE, sz, sizeof(sz));
        snprintf(t, sizeof(t), "%s  ·  %s  ·  %s", d->name, d->model, sz);
        gfx_text(s, font_ui_bold, x + 88, y + 26, t, theme.text);
        gfx_text_wrap(s, font_ui, R(x + 88, y + 52, cw - 110, 50),
                      "Everything on this disk will be deleted. It gets a new GPT partition table with one "
                      "FAT32 EFI System Partition that holds ZenithOS and your files.", theme.text_dim, 2);
        if (ui_button(u, R(x, by, 110, 40), "Back", BTN_NORMAL)) in->page = 1;
        if (ui_button(u, R(W - 36 - 190, by, 190, 40), "Erase and install", BTN_DANGER)) {
            in->page = 3;
            in->running = true;
            thread_create("installer", worker, in);
        }
        break;
    }
    case 3: {
        struct install_progress *p = &in->prog;
        const char *title = !p->done ? "Installing..." : p->failed ? "Installation failed" : "All done!";
        gfx_text(s, font_bold_lg, x, y, title, theme.text);
        y += 50;
        static const char *names[] = { "", "Preparing the disk", "Partition table", "File system",
                                       "Boot loader", "Kernel", "System image", "Writing to disk" };
        for (int i = 1; i <= 7; i++) {
            int yy = y + (i - 1) * 34;
            int st = p->done && !p->failed ? 2 : i < p->step ? 2 : i == p->step ? (p->failed ? 0 : 1) : 0;
            check(s, x, yy, st);
            gfx_text(s, st == 1 ? font_ui_bold : font_ui_md, x + 34, yy + 2, names[i], st ? theme.text : theme.text_faint);
        }
        y += 7 * 34 + 12;
        int pw = cw;
        gfx_round_rect(s, x, y, pw, 8, 4, theme.surface2);
        gfx_round_rect(s, x, y, MAX(8, pw * p->pct / 100), 8, 4, p->failed ? theme.danger : theme.accent);
        gfx_text_ellipsis(s, font_ui, x, y + 18, cw, p->msg, p->failed ? theme.danger : theme.text_dim);
        if (p->done && !p->failed) {
            gfx_text(s, font_ui, x, by - 26, "Remove the installation medium, then restart.", theme.text_faint);
            if (ui_button(u, R(W - 36 - 150, by, 150, 40), "Restart now", BTN_PRIMARY)) app_launch("@reboot");
            if (ui_button(u, R(W - 36 - 150 - 120, by, 110, 40), "Close", BTN_NORMAL)) a->quit = true;
        } else if (p->done) {
            if (ui_button(u, R(x, by, 110, 40), "Back", BTN_NORMAL)) { in->page = 1; memset(p, 0, sizeof(*p)); }
        }
        if (!p->done) u->want_repaint = true;
        break;
    }
    }
}

static void inst_close(app_t *a)
{
    struct inst *in = a->data;
    if (in->running) return;        /* never leave a half-written disk behind */
    a->quit = true;
}

int installer_main(void *arg)
{
    UNUSED(arg);
    struct inst *in = kzalloc(sizeof(*in));
    in->disk = -1;
    app_t a = { 0 };
    a.data = in;
    a.win = wm_create("Install ZenithOS", 860, 560, WF_CENTER);
    if (!a.win) { kfree(in); return 1; }
    wm_set_icon(a.win, ICON_INSTALL);
    a.on_paint = inst_paint;
    a.on_close = inst_close;
    int r = app_run(&a);
    while (in->running) sched_sleep(50);
    kfree(in);
    return r;
}
