// lsrecent [options] [DIR] - list DIR's entries ls -l style, most recently modified first.
// A directory's time is the newest mtime of itself and everything under it (symlinks never
// followed). Sizes are disk usage like du (st_blocks*512, directories' own blocks included,
// a hard-linked file counted once per top-level entry): roughly what deleting the entry frees.
// On a tty it shows a live, in-place table while scanning; piped, only the final table.
// Names, -w paths and link targets are printed with control and invalid bytes escaped.
// With -x a directory on another filesystem below DIR adds neither size nor time (like du -x).
// Options: -r oldest first, -x stay on one filesystem, -j N threads, -w show the newest item
// inside each directory, -q no live view, --bytes exact sizes.
// Build (portable, no arch-specific code): gcc -O2 -pthread -Wall -Wextra -s -o lsrecent-aarch64 lsrecent.c
// (lsrecent-amd64: the same command on x86-64, -o lsrecent-amd64)
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <wchar.h>
#include <unistd.h>

typedef struct { dev_t dev; ino_t ino; int used; } Key;

typedef struct {
    char *name, *target;
    mode_t mode;
    dev_t dev;
    pthread_mutex_t mu;
    int64_t msec;       // newest mtime (sec, nsec)
    long mnsec;
    char *newest;       // path of the newest item below the entry (-w), NULL: the entry itself
    uint64_t size, count;
    Key *keys;          // hard-linked files seen (dev, ino), created when first needed
    size_t nkeys, kcap;
    unsigned lg;
    atomic_int active;  // directory tasks outstanding
} Top;

// An open directory kept until the last queued child has opened its own fd relative to it.
typedef struct { DIR *d; int fd; atomic_int refs; } Node;

typedef struct Task {
    struct Task *next;
    Top *top;
    Node *parent;
    dev_t dev;
    char *name, *rel;   // rel: path below the top-level entry (-w only)
} Task;

static Top *tops;
static size_t ntops;
static int opt_x, opt_w, opt_r, opt_bytes, opt_q, jobs;
static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcv = PTHREAD_COND_INITIALIZER, donecv;
static Task *stack;
static long pending;
static atomic_ulong n_dirs, n_files, n_bytes, n_bad, n_gone, n_rderr, n_badent;
static volatile sig_atomic_t cursor_hidden;
static atomic_int open_nodes;
static int node_cap;

static void show_cursor(void) {
    static const char s[] = "\033[?25h";
    if (cursor_hidden && write(1, s, sizeof s - 1)) {}
}
static void die(const char *msg) {
    show_cursor();
    fprintf(stderr, "lsrecent: %s\n", msg);
    _exit(1);
}
static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) die("out of memory"); return p; }
static char *xstrdup(const char *s) { char *p = strdup(s); if (!p) die("out of memory"); return p; }
static void onsig(int sig) {
    show_cursor();
    if (cursor_hidden && write(1, "\n", 1)) {}
    _exit(sig == SIGINT ? 130 : sig == SIGQUIT ? 131 : sig == SIGTERM ? 143 : 129);
}
static void oncont(int sig) {
    (void)sig;
    static const char hide[] = "\033[?25l";
    if (cursor_hidden && write(1, hide, sizeof hide - 1)) {}
}
static void ontstp(int sig) {
    show_cursor();
    signal(SIGTSTP, SIG_DFL);
    sigset_t m;
    sigemptyset(&m); sigaddset(&m, sig);
    sigprocmask(SIG_UNBLOCK, &m, NULL);
    raise(SIGTSTP);
    // continued
    signal(SIGTSTP, ontstp);  // SIGCONT's handler hides the cursor again
}

static char *join(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *p = xmalloc(la + lb + 2);
    if (la) { memcpy(p, a, la); p[la] = '/'; la++; }
    memcpy(p + la, b, lb + 1);
    return p;
}

static void node_unref(Node *n) {
    if (atomic_fetch_sub(&n->refs, 1) == 1) { closedir(n->d); free(n); atomic_fetch_sub(&open_nodes, 1); }
}

static void submit_task(Task *k) {
    pthread_mutex_lock(&qmu);
    k->next = stack; stack = k;
    pthread_cond_signal(&qcv);
    pthread_mutex_unlock(&qmu);
}

// rel: the child's path below the top-level entry, malloc'd, taken over (NULL without -w)
static void submit(Top *t, Node *parent, dev_t dev, const char *name, char *rel) {
    Task *k = xmalloc(sizeof *k);
    k->top = t; k->parent = parent; k->dev = dev;
    k->name = xstrdup(name);
    k->rel = rel;
    atomic_fetch_add(&parent->refs, 1);
    atomic_fetch_add(&t->active, 1);
    pthread_mutex_lock(&qmu);
    pending++;
    pthread_mutex_unlock(&qmu);
    submit_task(k);
}

static int tcmp(int64_t s1, long n1, int64_t s2, long n2) {
    return s1 != s2 ? (s1 < s2 ? -1 : 1) : n1 != n2 ? (n1 < n2 ? -1 : 1) : 0;
}

static size_t khash(const Key *k, unsigned lg) {
    uint64_t x = ((uint64_t)k->ino ^ ((uint64_t)k->dev * 0xC2B2AE3D27D4EB4Full)) * 0x9E3779B97F4A7C15ull;
    return (size_t)(x >> (64 - lg));
}

// 1 if this (dev, ino) is new for the top-level entry
static int link_first(Top *t, const struct stat *st) {
    int fresh = 1;
    Key key = {st->st_dev, st->st_ino, 1};
    pthread_mutex_lock(&t->mu);
    if (t->nkeys * 2 >= t->kcap) {
        unsigned lg = t->kcap ? t->lg + 1 : 6;
        size_t nc = (size_t)1 << lg;
        Key *nk = calloc(nc, sizeof *nk);
        if (!nk) die("out of memory");
        for (size_t i = 0; i < t->kcap; i++)
            if (t->keys[i].used) {
                size_t h = khash(&t->keys[i], lg);
                while (nk[h].used) h = (h + 1) & (nc - 1);
                nk[h] = t->keys[i];
            }
        free(t->keys); t->keys = nk; t->kcap = nc; t->lg = lg;
    }
    size_t h = khash(&key, t->lg);
    while (t->keys[h].used) {
        if (t->keys[h].ino == key.ino && t->keys[h].dev == key.dev) { fresh = 0; break; }
        h = (h + 1) & (t->kcap - 1);
    }
    if (fresh) { t->keys[h] = key; t->nkeys++; }
    pthread_mutex_unlock(&t->mu);
    return fresh;
}

typedef struct {
    int has;
    int64_t sec; long nsec;
    uint64_t size, files;
    char *bestpath;     // -w: path of the newest item, below the top-level entry
} Acc;
typedef struct { Top *top; dev_t dev; } Ctx;
typedef struct { char **v; size_t n, cap; } Names;

// Read one open directory into acc. Subdirectories become queued tasks (children of me) while
// few nodes are open, else go to inl to be scanned inline by the caller.
static void walk(DIR *d, const Ctx *c, const char *wrel, Node *me, Acc *a, Names *inl) {
    int fd = dirfd(d);
    int has = 0;
    int64_t bs = 0; long bn = 0;
    char bestname[256] = "";
    uint64_t size = 0, files = 0;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) { if (errno) atomic_fetch_add(&n_rderr, 1); break; }
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2])))
            continue;
        struct stat st;
        if (fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            atomic_fetch_add(errno == ENOENT ? &n_gone : &n_badent, 1);
            continue;
        }
        int isdir = S_ISDIR(st.st_mode);
        if (isdir && opt_x && st.st_dev != c->dev) continue;
        if (!has || tcmp(st.st_mtim.tv_sec, st.st_mtim.tv_nsec, bs, bn) > 0) {
            has = 1; bs = st.st_mtim.tv_sec; bn = st.st_mtim.tv_nsec;
            if (opt_w) { strncpy(bestname, e->d_name, sizeof bestname - 1); bestname[sizeof bestname - 1] = 0; }
        }
        size += (uint64_t)st.st_blocks * 512;
        if (isdir) {
            if (me && atomic_load(&open_nodes) < node_cap)
                submit(c->top, me, st.st_dev, e->d_name, opt_w ? join(wrel, e->d_name) : NULL);
            else {
                if (inl->n == inl->cap) {
                    inl->cap = inl->cap ? inl->cap * 2 : 8;
                    inl->v = realloc(inl->v, inl->cap * sizeof *inl->v);
                    if (!inl->v) die("out of memory");
                }
                inl->v[inl->n++] = xstrdup(e->d_name);
            }
        } else {
            if (st.st_nlink > 1 && !link_first(c->top, &st)) size -= (uint64_t)st.st_blocks * 512;
            files++;
        }
    }
    atomic_fetch_add(&n_dirs, 1);
    atomic_fetch_add(&n_files, files);
    atomic_fetch_add(&n_bytes, size);
    a->size += size; a->files += files;
    if (has && (!a->has || tcmp(bs, bn, a->sec, a->nsec) > 0)) {
        a->has = 1; a->sec = bs; a->nsec = bn;
        if (opt_w) { free(a->bestpath); a->bestpath = join(wrel, bestname); }
    }
}

static void scan_names(Names *inl, int anchor, const char *arel, const char *wrel, const Ctx *c, Acc *a);

// Scan the directory rel (relative to the open fd anchor), holding no fd while its children run.
static void scan_inline(int anchor, const char *rel, char *wrel, const Ctx *c, Acc *a) {
    int fd = openat(anchor, rel, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *d = fd < 0 ? NULL : fdopendir(fd);
    if (fd >= 0 && !d) close(fd);
    if (!d) {
        atomic_fetch_add(errno == ENOENT ? &n_gone : &n_bad, 1);
        free(wrel);
        return;
    }
    Names inl = {0};
    walk(d, c, wrel, NULL, a, &inl);
    if (inl.n) {
        int reanchor = strlen(rel) > 2000;  // keep relative paths well under PATH_MAX
        if (reanchor) scan_names(&inl, fd, "", wrel, c, a);
        else { closedir(d); d = NULL; scan_names(&inl, anchor, rel, wrel, c, a); }
    }
    if (d) closedir(d);
    free(inl.v);
    free(wrel);
}

static void scan_names(Names *inl, int anchor, const char *arel, const char *wrel, const Ctx *c, Acc *a) {
    for (size_t i = 0; i < inl->n; i++) {
        char *rel = join(arel, inl->v[i]);
        scan_inline(anchor, rel, opt_w ? join(wrel, inl->v[i]) : NULL, c, a);
        free(rel); free(inl->v[i]);
    }
    inl->n = 0;
}

static void scan(Task *k) {
    int fd = openat(k->parent->fd, k->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int err = errno;
    node_unref(k->parent);
    DIR *d = fd < 0 ? NULL : fdopendir(fd);
    if (fd >= 0 && !d) { err = errno; close(fd); }
    if (!d) { atomic_fetch_add(err == ENOENT ? &n_gone : &n_bad, 1); free(k->name); free(k->rel); return; }
    Node *me = xmalloc(sizeof *me);
    me->d = d; me->fd = fd; atomic_init(&me->refs, 1);
    atomic_fetch_add(&open_nodes, 1);
    Ctx c = {k->top, k->dev};
    Acc a = {0};
    Names inl = {0};
    walk(d, &c, k->rel, me, &a, &inl);
    scan_names(&inl, fd, "", k->rel, &c, &a);
    free(inl.v);
    Top *t = k->top;
    pthread_mutex_lock(&t->mu);
    t->size += a.size; t->count += a.files;
    if (a.has && tcmp(a.sec, a.nsec, t->msec, t->mnsec) > 0) {
        t->msec = a.sec; t->mnsec = a.nsec;
        if (opt_w) { free(t->newest); t->newest = a.bestpath; a.bestpath = NULL; }
    }
    pthread_mutex_unlock(&t->mu);
    free(a.bestpath);
    node_unref(me);
    free(k->name); free(k->rel);
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&qmu);
        while (!stack && pending) pthread_cond_wait(&qcv, &qmu);
        Task *k = stack;
        if (!k) { pthread_mutex_unlock(&qmu); return NULL; }
        stack = k->next;
        pthread_mutex_unlock(&qmu);
        Top *t = k->top;
        scan(k);
        if (atomic_fetch_sub(&t->active, 1) == 1) {  // the entry is complete: drop its hard-link set
            pthread_mutex_lock(&t->mu);
            free(t->keys); t->keys = NULL; t->nkeys = t->kcap = 0;
            pthread_mutex_unlock(&t->mu);
        }
        free(k);
        pthread_mutex_lock(&qmu);
        if (--pending == 0) { pthread_cond_broadcast(&qcv); pthread_cond_broadcast(&donecv); }
        pthread_mutex_unlock(&qmu);
    }
}

// ---- output ----
typedef struct { char *p; size_t len, cap; } SB;
static void sb_put(SB *s, const char *b, size_t n) {
    if (s->len + n + 1 > s->cap) {
        s->cap = (s->len + n + 1) * 2;
        s->p = realloc(s->p, s->cap);
        if (!s->p) die("out of memory");
    }
    memcpy(s->p + s->len, b, n); s->len += n; s->p[s->len] = 0;
}
static void sb_str(SB *s, const char *b) { sb_put(s, b, strlen(b)); }
static void sb_fmt(SB *s, const char *f, ...) __attribute__((format(printf, 2, 3)));
static void sb_fmt(SB *s, const char *f, ...) {
    char b[128];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    sb_put(s, b, n > 0 ? (size_t)n : 0);
}

static int zerowidth_fmt(wchar_t c) {  // bidi controls and invisible characters: shown escaped
    return (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) || c == 0xFEFF;
}

// append str with control characters, bidi/zero-width characters and invalid bytes escaped
static void sb_esc(SB *o, const char *str) {
    size_t n = strlen(str);
    for (size_t i = 0; i < n;) {
        mbstate_t st;
        memset(&st, 0, sizeof st);
        wchar_t wc;
        size_t l = mbrtowc(&wc, str + i, n - i, &st);
        if (l != (size_t)-1 && l != (size_t)-2 && l != 0) {
            if (zerowidth_fmt(wc)) { sb_fmt(o, "\\u{%04X}", (unsigned)wc); i += l; continue; }
            if (wcwidth(wc) >= 0) { sb_put(o, str + i, l); i += l; continue; }
        }
        unsigned c = (unsigned char)str[i++];  // one byte, escaped
        switch (c) {
        case '\n': sb_str(o, "\\n"); break;
        case '\t': sb_str(o, "\\t"); break;
        case '\r': sb_str(o, "\\r"); break;
        case 033: sb_str(o, "\\e"); break;
        case '\a': sb_str(o, "\\a"); break;
        case '\b': sb_str(o, "\\b"); break;
        case '\f': sb_str(o, "\\f"); break;
        case '\v': sb_str(o, "\\v"); break;
        default: sb_fmt(o, "\\x%02X", c);
        }
    }
}

// bytes of the (already escaped) string that fit in maxw columns
static size_t fit(const char *s, size_t n, int maxw) {
    int w = 0;
    size_t i = 0;
    while (i < n) {
        mbstate_t st;
        memset(&st, 0, sizeof st);
        wchar_t wc;
        size_t l = mbrtowc(&wc, s + i, n - i, &st);
        int cw = 1;
        if (l == (size_t)-1 || l == (size_t)-2 || l == 0) l = 1;
        else if ((cw = wcwidth(wc)) < 0) cw = 1;
        if (w + cw > maxw) break;
        w += cw; i += l;
    }
    return i;
}

typedef struct {
    int64_t msec;
    long mnsec;
    uint64_t size, count;
    mode_t mode;
    Top *top;
    char *why;
    int busy;
} Row;

static void human(uint64_t n, char *o, size_t len) {
    if (opt_bytes) { snprintf(o, len, "%llu", (unsigned long long)n); return; }
    double f = n; const char *u = "BKMGTP"; int i = 0;
    while (f >= 1024 && u[i + 1]) { f /= 1024; i++; }
    if (i == 0) snprintf(o, len, "%llu", (unsigned long long)n);
    else snprintf(o, len, "%.1f%c", f, u[i]);
}

static void filemode(mode_t m, char *s) {
    const char *t = S_ISDIR(m) ? "d" : S_ISLNK(m) ? "l" : S_ISCHR(m) ? "c" : S_ISBLK(m) ? "b" :
                    S_ISFIFO(m) ? "p" : S_ISSOCK(m) ? "s" : "-";
    s[0] = t[0];
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) s[i + 1] = (m & (0400 >> i)) ? rwx[i] : '-';
    if (m & S_ISUID) s[3] = (m & 0100) ? 's' : 'S';
    if (m & S_ISGID) s[6] = (m & 0010) ? 's' : 'S';
    if (m & S_ISVTX) s[9] = (m & 0001) ? 't' : 'T';
    s[10] = 0;
}

static int cmp_row(const void *a, const void *b) {
    const Row *x = a, *y = b;
    int c = tcmp(x->msec, x->mnsec, y->msec, y->mnsec);
    if (c) return -c;
    return strcmp(x->top->name, y->top->name);
}
static int cmp_row_r(const void *a, const void *b) { return -cmp_row(a, b); }

static Row *snapshot(void) {
    Row *r = calloc(ntops ? ntops : 1, sizeof *r);
    if (!r) die("out of memory");
    for (size_t i = 0; i < ntops; i++) {
        Top *t = &tops[i];
        pthread_mutex_lock(&t->mu);
        r[i].msec = t->msec; r[i].mnsec = t->mnsec; r[i].size = t->size; r[i].count = t->count;
        if (opt_w && S_ISDIR(t->mode)) r[i].why = xstrdup(t->newest ? t->newest : ".");
        pthread_mutex_unlock(&t->mu);
        r[i].mode = t->mode; r[i].top = t; r[i].busy = atomic_load(&t->active) > 0;
    }
    qsort(r, ntops, sizeof *r, opt_r ? cmp_row_r : cmp_row);
    return r;
}

// Print rows [0,n), one terminal line each; maxw >= 2 truncates (after escaping).
static void print_rows(Row *r, size_t n, int maxw, int color, int live) {
    size_t w[3] = {10, 0, 0};
    char b[32];
    for (size_t i = 0; i < n; i++) {
        human(r[i].size, b, sizeof b);
        if (strlen(b) > w[1]) w[1] = strlen(b);
        snprintf(b, sizeof b, "%llu", (unsigned long long)r[i].count);
        if (S_ISDIR(r[i].mode) && strlen(b) > w[2]) w[2] = strlen(b);
    }
    SB line = {0};
    for (size_t i = 0; i < n; i++) {
        char mode[11], sz[32], ts[32], cnt[32] = "";
        filemode(r[i].mode, mode); human(r[i].size, sz, sizeof sz);
        time_t s = (time_t)r[i].msec;
        struct tm tm;
        if (!localtime_r(&s, &tm) || !strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", &tm)) strcpy(ts, "????" "-??" "-?? ??:??");
        int isdir = S_ISDIR(r[i].mode);
        if (isdir) snprintf(cnt, sizeof cnt, "%llu", (unsigned long long)r[i].count);
        Top *t = r[i].top;
        line.len = 0;
        sb_fmt(&line, "%s %*s %s %*s ", mode, (int)w[1], sz, ts, (int)w[2], cnt);
        if (live) sb_str(&line, r[i].busy ? "\xe2\x80\xa6 " : "  ");
        size_t namestart = line.len;
        sb_esc(&line, t->name);
        if (isdir) sb_str(&line, "/");
        if (t->target) { sb_str(&line, " -> "); sb_esc(&line, t->target); }
        if (opt_w && r[i].why && isdir) { sb_str(&line, "  ["); sb_esc(&line, r[i].why); sb_str(&line, "]"); }
        if (maxw >= 2) {
            size_t f = fit(line.p, line.len, maxw);
            line.len = f; line.p[f] = 0;
        }
        if (color && isdir && namestart < line.len) {
            fwrite(line.p, 1, namestart, stdout);
            fputs("\033[1;34m", stdout);
            fwrite(line.p + namestart, 1, line.len - namestart, stdout);
            fputs("\033[0m", stdout);
        } else fwrite(line.p, 1, line.len, stdout);
        fputs(live ? "\033[K\n" : "\n", stdout);
    }
    free(line.p);
}

static void usage(void) {
    puts("usage: lsrecent [-rxwq] [-j N] [--bytes] [DIR]\n"
         "  -r reverse (oldest first)   -x stay on one filesystem   -j N threads\n"
         "  -w show the newest item inside each directory   -q no live view on a tty\n"
         "  --bytes exact sizes");
}

static void draw(struct timespec *t0, size_t *drawn) {
    struct winsize ws;
    int rows = 24, cols = 80;
    if (!ioctl(1, TIOCGWINSZ, &ws)) {
        if (ws.ws_row > 0) rows = ws.ws_row;
        if (ws.ws_col > 0) cols = ws.ws_col;
    }
    int maxw = cols >= 2 ? cols - 1 : 0;
    Row *r = snapshot();
    size_t n = ntops, room = rows > 3 ? (size_t)rows - 2 : 1;
    if (n > room) n = room;
    if (*drawn) printf("\033[%zuA\r", *drawn);
    print_rows(r, n, maxw, 1, 1);
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    double el = (t1.tv_sec - t0->tv_sec) + (t1.tv_nsec - t0->tv_nsec) / 1e9;
    char sz[32]; human(atomic_load(&n_bytes), sz, sizeof sz);
    size_t busy = 0;
    for (size_t i = 0; i < ntops; i++) busy += atomic_load(&tops[i].active) > 0;
    char st[256];
    int sl = snprintf(st, sizeof st, "%lu dirs, %lu files, %s, %.1fs, %lu unreadable, %zu of %zu entries scanning%s",
                      atomic_load(&n_dirs), atomic_load(&n_files), sz, el, atomic_load(&n_bad), busy, ntops,
                      ntops > n ? " (list truncated)" : "");
    if (maxw && sl > maxw) st[maxw] = 0;
    printf("\033[2m%s\033[0m\033[K\n\033[J", st);
    *drawn = n + 1;
    fflush(stdout);
    for (size_t i = 0; i < ntops; i++) free(r[i].why);
    free(r);
}

int main(int argc, char **argv) {
    jobs = 8;
    static const struct option lo[] = {
        {"reverse", 0, 0, 'r'}, {"one-file-system", 0, 0, 'x'}, {"jobs", 1, 0, 'j'},
        {"why", 0, 0, 'w'}, {"bytes", 0, 0, 'b'}, {"quiet", 0, 0, 'q'}, {"help", 0, 0, 'h'}, {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "rxj:wqh", lo, NULL)) != -1) {
        switch (c) {
        case 'r': opt_r = 1; break;
        case 'x': opt_x = 1; break;
        case 'j': jobs = atoi(optarg); break;
        case 'w': opt_w = 1; break;
        case 'b': opt_bytes = 1; break;
        case 'q': opt_q = 1; break;
        case 'h': usage(); return 0;
        default: usage(); return 2;
        }
    }
    if (jobs < 1) jobs = 1;
    const char *root = optind < argc ? argv[optind] : ".";

    int live = !opt_q && isatty(1);
    struct sigaction sa = {0};
    sa.sa_handler = onsig;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sa.sa_handler = ontstp;
    sigaction(SIGTSTP, &sa, NULL);
    sa.sa_handler = oncont;
    sigaction(SIGCONT, &sa, NULL);
    if (!setlocale(LC_CTYPE, "") || strcmp(nl_langinfo(CODESET), "UTF-8")) setlocale(LC_CTYPE, "C.UTF-8");
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&donecv, &ca);

    struct rlimit rl;
    if (!getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur < rl.rlim_max) {
        rlim_t want = rl.rlim_max == RLIM_INFINITY || rl.rlim_max > 1048576 ? 1048576 : rl.rlim_max;
        rl.rlim_cur = want;
        if (setrlimit(RLIMIT_NOFILE, &rl)) { rl.rlim_cur = want / 2; setrlimit(RLIMIT_NOFILE, &rl); }
    }

    {
        struct rlimit cur;
        long lim = !getrlimit(RLIMIT_NOFILE, &cur) && cur.rlim_cur != RLIM_INFINITY ? (long)cur.rlim_cur : 1L << 20;
        node_cap = lim / 2 - 16 < 2 ? 2 : (int)(lim / 2 - 16);
    }
    int rfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *d = rfd < 0 ? NULL : fdopendir(rfd);
    struct stat rst;
    if (!d || fstat(rfd, &rst)) { fprintf(stderr, "lsrecent: %s: %s\n", root, strerror(errno)); return 1; }
    Node *rootn = xmalloc(sizeof *rootn);
    rootn->d = d; rootn->fd = rfd; atomic_init(&rootn->refs, 1);
    atomic_fetch_add(&open_nodes, 1);

    size_t cap = 64;
    tops = xmalloc(cap * sizeof *tops);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        struct stat st;
        if (fstatat(rfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            atomic_fetch_add(errno == ENOENT ? &n_gone : &n_badent, 1);
            continue;
        }
        if (ntops == cap) { cap *= 2; tops = realloc(tops, cap * sizeof *tops); if (!tops) die("out of memory"); }
        Top *t = &tops[ntops++];
        memset(t, 0, sizeof *t);
        pthread_mutex_init(&t->mu, NULL);
        t->name = xstrdup(e->d_name);
        t->mode = st.st_mode; t->dev = st.st_dev; t->msec = st.st_mtim.tv_sec; t->mnsec = st.st_mtim.tv_nsec;
        t->size = (uint64_t)st.st_blocks * 512;
        if (S_ISLNK(st.st_mode)) {
            char buf[PATH_MAX + 1];
            ssize_t n = readlinkat(rfd, e->d_name, buf, PATH_MAX);
            if (n >= 0) { buf[n] = 0; t->target = xstrdup(buf); }
        }
        if (!S_ISDIR(st.st_mode)) { t->count = 1; atomic_fetch_add(&n_files, 1); atomic_fetch_add(&n_bytes, t->size); }
    }

    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    if (live) { cursor_hidden = 1; fputs("\033[?25l", stdout); fflush(stdout); }

    pending = 1;  // main's reference while submitting
    pthread_t *th = xmalloc(jobs * sizeof *th);
    int started = 0;
    for (int i = 0; i < jobs; i++)
        if (pthread_create(&th[started], NULL, worker, NULL) == 0) started++;
    if (!started) die("cannot start threads");
    for (size_t i = 0; i < ntops; i++)
        if (S_ISDIR(tops[i].mode) && (!opt_x || tops[i].dev == rst.st_dev))
            submit(&tops[i], rootn, tops[i].dev, tops[i].name, opt_w ? xstrdup("") : NULL);
    node_unref(rootn);
    pthread_mutex_lock(&qmu);
    if (--pending == 0) { pthread_cond_broadcast(&qcv); pthread_cond_broadcast(&donecv); }
    pthread_mutex_unlock(&qmu);

    size_t drawn = 0;
    if (live) {
        for (;;) {
            draw(&t0, &drawn);
            struct timespec until;
            clock_gettime(CLOCK_MONOTONIC, &until);
            until.tv_nsec += 200000000;
            if (until.tv_nsec >= 1000000000) { until.tv_nsec -= 1000000000; until.tv_sec++; }
            pthread_mutex_lock(&qmu);
            while (pending && pthread_cond_timedwait(&donecv, &qmu, &until) == 0) {}
            long p = pending;
            pthread_mutex_unlock(&qmu);
            if (!p) break;
        }
    }
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
    if (live) { if (drawn) printf("\033[%zuA\r", drawn); fputs("\033[J\033[?25h", stdout); fflush(stdout); cursor_hidden = 0; }
    Row *r = snapshot();
    print_rows(r, ntops, 0, isatty(1), 0);
    fflush(stdout);
    for (size_t i = 0; i < ntops; i++) free(r[i].why);
    free(r);
    char msg[240] = "";
    size_t ml = 0;
    unsigned long v;
    if ((v = atomic_load(&n_bad))) ml += snprintf(msg + ml, sizeof msg - ml, "%lu director%s unreadable", v, v == 1 ? "y" : "ies");
    if ((v = atomic_load(&n_badent))) ml += snprintf(msg + ml, sizeof msg - ml, "%s%lu entr%s unreadable", ml ? ", " : "", v, v == 1 ? "y" : "ies");
    if ((v = atomic_load(&n_gone))) ml += snprintf(msg + ml, sizeof msg - ml, "%s%lu entr%s vanished", ml ? ", " : "", v, v == 1 ? "y" : "ies");
    if ((v = atomic_load(&n_rderr))) ml += snprintf(msg + ml, sizeof msg - ml, "%s%lu read error%s", ml ? ", " : "", v, v == 1 ? "" : "s");
    if (ml) fprintf(stderr, "lsrecent: %s\n", msg);
    return 0;
}
