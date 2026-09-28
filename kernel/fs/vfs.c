/*
 * In-memory file system (ramfs) with a tar loader for the initrd,
 * device nodes, and path handling.
 */
#include <vfs.h>
#include <mm.h>
#include <dev.h>

static vnode_t *root;
static mutex_t vfs_mtx = MUTEX_INIT("vfs");
static size_t total_bytes;

static int64_t now(void) { return rtc_epoch(); }

static vnode_t *node_new(const char *name, int type)
{
    vnode_t *n = kzalloc(sizeof(*n));
    if (!n) return NULL;
    strlcpy(n->name, name, sizeof(n->name));
    n->type = type;
    n->mtime = now();
    return n;
}

static void node_attach(vnode_t *dir, vnode_t *n)
{
    n->parent = dir;
    /* keep children sorted: directories first, then by name */
    vnode_t **pp = &dir->child;
    while (*pp) {
        vnode_t *c = *pp;
        bool before;
        if ((n->type == VN_DIR) != (c->type == VN_DIR)) before = n->type == VN_DIR;
        else before = strcasecmp(n->name, c->name) < 0;
        if (before) break;
        pp = &c->sibling;
    }
    n->sibling = *pp;
    *pp = n;
    dir->mtime = now();
}

static void node_detach(vnode_t *n)
{
    if (!n->parent) return;
    vnode_t **pp = &n->parent->child;
    while (*pp && *pp != n) pp = &(*pp)->sibling;
    if (*pp) *pp = n->sibling;
    n->parent->mtime = now();
    n->parent = NULL;
    n->sibling = NULL;
}

static void node_free(vnode_t *n)
{
    while (n->child) {
        vnode_t *c = n->child;
        n->child = c->sibling;
        c->parent = NULL;
        c->unlinked = true;
        if (!c->refs) node_free(c);
    }
    if (n->data) {
        total_bytes -= n->size;
        kfree(n->data);
    }
    kfree(n);
}

static vnode_t *child_named(vnode_t *dir, const char *name, size_t len)
{
    for (vnode_t *c = dir->child; c; c = c->sibling)
        if (strlen(c->name) == len && !strncmp(c->name, name, len)) return c;
    return NULL;
}

/* ------------------------------------------------------------------------
 * paths
 * ---------------------------------------------------------------------- */

bool vfs_resolve(const char *cwd, const char *path, char *out, size_t outsz)
{
    char tmp[VFS_PATH_MAX * 2];
    if (!path || !*path) path = ".";
    if (path[0] == '/') strlcpy(tmp, path, sizeof(tmp));
    else {
        strlcpy(tmp, cwd && *cwd ? cwd : "/", sizeof(tmp));
        strlcat(tmp, "/", sizeof(tmp));
        strlcat(tmp, path, sizeof(tmp));
    }
    /* normalise: collapse //, handle . and .. */
    char *comp[64];
    int n = 0;
    char *p = tmp;
    while (*p) {
        while (*p == '/') *p++ = 0;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = 0;
        if (!strcmp(start, ".")) continue;
        if (!strcmp(start, "..")) { if (n) n--; continue; }
        if (n < 64) comp[n++] = start;
    }
    size_t len = 0;
    if (outsz < 2) return false;
    out[0] = 0;
    if (!n) { strlcpy(out, "/", outsz); return true; }
    for (int i = 0; i < n; i++) {
        size_t cl = strlen(comp[i]);
        if (len + cl + 2 > outsz) return false;
        out[len++] = '/';
        memcpy(out + len, comp[i], cl);
        len += cl;
    }
    out[len] = 0;
    return true;
}

static vnode_t *walk(const char *path, vnode_t **parent_out, const char **last_out)
{
    if (!path || path[0] != '/') return NULL;
    vnode_t *cur = root;
    vnode_t *parent = NULL;
    const char *p = path;
    const char *last = NULL;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - s);
        last = s;
        parent = cur;
        if (!cur || cur->type != VN_DIR) { cur = NULL; break; }
        cur = child_named(cur, s, len);
        if (!cur) {
            /* only the last component may be missing */
            while (*p == '/') p++;
            if (*p) parent = NULL;
            break;
        }
    }
    if (parent_out) *parent_out = parent;
    if (last_out) *last_out = last;
    return cur;
}

const char *vfs_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void vfs_dirname(const char *path, char *out, size_t sz)
{
    strlcpy(out, path, sz);
    char *s = strrchr(out, '/');
    if (!s) { strlcpy(out, "/", sz); return; }
    if (s == out) s[1] = 0;
    else *s = 0;
}

/* ------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

vnode_t *vfs_lookup(const char *path)
{
    mutex_lock(&vfs_mtx);
    vnode_t *n = walk(path, NULL, NULL);
    mutex_unlock(&vfs_mtx);
    return n;
}

static vnode_t *create_locked(const char *path, int type, int *err)
{
    vnode_t *parent;
    const char *last;
    vnode_t *n = walk(path, &parent, &last);
    if (n) { *err = E_EXIST; return n; }
    if (!parent || parent->type != VN_DIR || !last) { *err = E_NOENT; return NULL; }
    char name[64];
    size_t len = 0;
    while (last[len] && last[len] != '/' && len < 63) { name[len] = last[len]; len++; }
    name[len] = 0;
    n = node_new(name, type);
    if (!n) { *err = E_NOMEM; return NULL; }
    node_attach(parent, n);
    *err = 0;
    return n;
}

vnode_t *vfs_open(const char *path, bool create)
{
    mutex_lock(&vfs_mtx);
    vnode_t *n = walk(path, NULL, NULL);
    if (!n && create) {
        int err;
        n = create_locked(path, VN_FILE, &err);
    }
    if (n) n->refs++;
    mutex_unlock(&vfs_mtx);
    return n;
}

void vfs_close(vnode_t *vn)
{
    if (!vn) return;
    mutex_lock(&vfs_mtx);
    if (--vn->refs == 0 && vn->unlinked) node_free(vn);
    mutex_unlock(&vfs_mtx);
}

int vfs_mkdir(const char *path)
{
    mutex_lock(&vfs_mtx);
    int err;
    vnode_t *n = create_locked(path, VN_DIR, &err);
    if (err == E_EXIST && n && n->type == VN_DIR) err = 0;
    mutex_unlock(&vfs_mtx);
    return err;
}

int vfs_unlink(const char *path, bool recursive)
{
    mutex_lock(&vfs_mtx);
    vnode_t *n = walk(path, NULL, NULL);
    int r = 0;
    if (!n) r = E_NOENT;
    else if (n == root) r = E_INVAL;
    else if (n->type == VN_DIR && n->child && !recursive) r = E_NOTEMPTY;
    else {
        node_detach(n);
        n->unlinked = true;
        if (!n->refs) node_free(n);
    }
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_rename(const char *from, const char *to)
{
    mutex_lock(&vfs_mtx);
    int r = 0;
    vnode_t *n = walk(from, NULL, NULL);
    vnode_t *parent;
    const char *last;
    vnode_t *dst = walk(to, &parent, &last);
    if (!n) r = E_NOENT;
    else if (n == root) r = E_INVAL;
    else if (dst) r = E_EXIST;
    else if (!parent || parent->type != VN_DIR) r = E_NOENT;
    else {
        /* refuse to move a directory into itself */
        for (vnode_t *p = parent; p; p = p->parent)
            if (p == n) { r = E_INVAL; break; }
        if (!r) {
            node_detach(n);
            size_t len = 0;
            while (last[len] && last[len] != '/' && len < 63) { n->name[len] = last[len]; len++; }
            n->name[len] = 0;
            node_attach(parent, n);
        }
    }
    mutex_unlock(&vfs_mtx);
    return r;
}

static int ensure_cap(vnode_t *n, size_t need)
{
    if (need <= n->cap) return 0;
    size_t cap = n->cap ? n->cap : 256;
    while (cap < need) cap *= 2;
    uint8_t *d = krealloc(n->data, cap);
    if (!d) return E_NOMEM;
    n->data = d;
    n->cap = cap;
    return 0;
}

long vfs_read(vnode_t *n, uint64_t off, void *buf, size_t len)
{
    if (n->type == VN_DEV) return n->dev_read ? n->dev_read(n, buf, len, off) : E_IO;
    if (n->type == VN_DIR) return E_ISDIR;
    mutex_lock(&vfs_mtx);
    long r = 0;
    if (off < n->size) {
        size_t c = MIN(len, n->size - off);
        memcpy(buf, n->data + off, c);
        r = (long)c;
    }
    mutex_unlock(&vfs_mtx);
    return r;
}

long vfs_write(vnode_t *n, uint64_t off, const void *buf, size_t len)
{
    if (n->type == VN_DEV) return n->dev_write ? n->dev_write(n, buf, len, off) : E_IO;
    if (n->type == VN_DIR) return E_ISDIR;
    mutex_lock(&vfs_mtx);
    long r = ensure_cap(n, off + len);
    if (!r) {
        if (off > n->size) memset(n->data + n->size, 0, off - n->size);
        memcpy(n->data + off, buf, len);
        if (off + len > n->size) {
            total_bytes += off + len - n->size;
            n->size = off + len;
        }
        n->mtime = now();
        r = (long)len;
    }
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_truncate(vnode_t *n, size_t size)
{
    if (n->type != VN_FILE) return E_INVAL;
    mutex_lock(&vfs_mtx);
    int r = ensure_cap(n, size);
    if (!r) {
        if (size > n->size) memset(n->data + n->size, 0, size - n->size);
        total_bytes = total_bytes - n->size + size;
        n->size = size;
        n->mtime = now();
    }
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_stat(const char *path, struct vfs_stat *st)
{
    mutex_lock(&vfs_mtx);
    vnode_t *n = walk(path, NULL, NULL);
    if (n) {
        st->type = n->type;
        st->size = n->size;
        st->mtime = n->mtime;
    }
    mutex_unlock(&vfs_mtx);
    return n ? 0 : E_NOENT;
}

int vfs_readdir(const char *path, int index, struct vfs_dirent *out)
{
    mutex_lock(&vfs_mtx);
    vnode_t *d = walk(path, NULL, NULL);
    int r = E_NOENT;
    if (d && d->type == VN_DIR) {
        vnode_t *c = d->child;
        for (int i = 0; c && i < index; i++) c = c->sibling;
        if (c) {
            strlcpy(out->name, c->name, sizeof(out->name));
            out->type = c->type;
            out->size = c->size;
            out->mtime = c->mtime;
            r = 0;
        } else {
            r = 1;   /* end */
        }
    } else if (d) {
        r = E_NOTDIR;
    }
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_list(const char *path, struct vfs_dirent *out, int max)
{
    mutex_lock(&vfs_mtx);
    vnode_t *d = walk(path, NULL, NULL);
    int n = 0;
    if (!d || d->type != VN_DIR) {
        mutex_unlock(&vfs_mtx);
        return d ? E_NOTDIR : E_NOENT;
    }
    for (vnode_t *c = d->child; c && n < max; c = c->sibling, n++) {
        strlcpy(out[n].name, c->name, sizeof(out[n].name));
        out[n].type = c->type;
        out[n].size = c->size;
        out[n].mtime = c->mtime;
    }
    mutex_unlock(&vfs_mtx);
    return n;
}

char *vfs_read_file(const char *path, size_t *size)
{
    vnode_t *n = vfs_open(path, false);
    if (!n) return NULL;
    if (n->type != VN_FILE) { vfs_close(n); return NULL; }
    size_t sz = n->size;
    char *buf = kmalloc(sz + 1);
    if (buf) {
        long r = vfs_read(n, 0, buf, sz);
        if (r < 0) r = 0;
        buf[r] = 0;
        if (size) *size = (size_t)r;
    }
    vfs_close(n);
    return buf;
}

int vfs_write_file(const char *path, const void *data, size_t size)
{
    vnode_t *n = vfs_open(path, true);
    if (!n) return E_NOENT;
    if (n->type != VN_FILE) { vfs_close(n); return E_ISDIR; }
    vfs_truncate(n, 0);
    long r = vfs_write(n, 0, data, size);
    vfs_close(n);
    return r < 0 ? (int)r : 0;
}

int vfs_copy(const char *from, const char *to)
{
    struct vfs_stat st;
    if (vfs_stat(from, &st)) return E_NOENT;
    if (st.type == VN_DIR) {
        int r = vfs_mkdir(to);
        if (r) return r;
        struct vfs_dirent de;
        for (int i = 0; vfs_readdir(from, i, &de) == 0; i++) {
            char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
            snprintf(a, sizeof(a), "%s/%s", from, de.name);
            snprintf(b, sizeof(b), "%s/%s", to, de.name);
            vfs_copy(a, b);
        }
        return 0;
    }
    size_t sz;
    char *d = vfs_read_file(from, &sz);
    if (!d) return E_IO;
    int r = vfs_write_file(to, d, sz);
    kfree(d);
    return r;
}

vnode_t *vfs_register_dev(const char *path, dev_read_fn r, dev_write_fn w, void *ctx)
{
    mutex_lock(&vfs_mtx);
    int err;
    vnode_t *n = create_locked(path, VN_DEV, &err);
    if (n) {
        n->type = VN_DEV;
        n->dev_read = r;
        n->dev_write = w;
        n->dev_ctx = ctx;
    }
    mutex_unlock(&vfs_mtx);
    return n;
}

size_t vfs_total_bytes(void) { return total_bytes; }

/* ------------------------------------------------------------------------
 * initrd (ustar)
 * ---------------------------------------------------------------------- */

static uint64_t octal(const char *s, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (uint64_t)(s[i] - '0');
    return v;
}

void vfs_load_tar(const void *data, size_t size)
{
    const uint8_t *p = data;
    int files = 0, dirs = 0;
    size_t off = 0;
    while (off + 512 <= size) {
        const char *h = (const char *)p + off;
        if (!h[0]) break;
        char name[VFS_PATH_MAX];
        char full[VFS_PATH_MAX + 2];
        if (!memcmp(h + 257, "ustar", 5) && h[345]) {
            snprintf(name, sizeof(name), "%.155s/%.100s", h + 345, h);
        } else {
            snprintf(name, sizeof(name), "%.100s", h);
        }
        uint64_t fsize = octal(h + 124, 12);
        int64_t mtime = (int64_t)octal(h + 136, 12);
        char type = h[156];
        char abs[VFS_PATH_MAX];
        snprintf(full, sizeof(full), "/%s", name);
        vfs_resolve("/", full, abs, sizeof(abs));
        if (type == '5') {
            if (strcmp(abs, "/")) { vfs_mkdir(abs); dirs++; }
        } else if (type == '0' || type == 0) {
            vfs_write_file(abs, p + off + 512, fsize);
            vnode_t *n = vfs_lookup(abs);
            if (n && mtime) n->mtime = mtime;
            files++;
        }
        off += 512 + ALIGN_UP(fsize, 512);
    }
    klog("vfs: initrd unpacked, %d files, %d directories, %lu KiB", files, dirs, total_bytes >> 10);
}

void vfs_init(void)
{
    root = node_new("", VN_DIR);
    const char *dirs[] = { "/bin", "/dev", "/etc", "/home", "/home/user", "/home/user/Desktop",
                           "/home/user/Documents", "/home/user/Pictures", "/tmp" };
    for (unsigned i = 0; i < ARRAY_SIZE(dirs); i++) vfs_mkdir(dirs[i]);
}
