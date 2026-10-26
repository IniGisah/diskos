/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "custom_saver_picker.h"
#include "sdio.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define CSP_PATH_MAX 512
struct csp_scan {
    char dir[CSP_PATH_MAX];
    csp_state state;
    int n, have_pos, have_id;
    long pos;
    dev_t dev; struct timespec mt;
    csp_stats st;
    csp_entry e[CSP_MAX_ENTRIES];
};

csp_scan *csp_open(const char *dir) {
    if(!dir) dir = CSP_DIR;
    if(strlen(dir) >= CSP_PATH_MAX) return NULL;
    csp_scan *s = calloc(1, sizeof *s);
    if(s) strcpy(s->dir, dir);
    return s;
}
void csp_close(csp_scan *s) { free(s); }
static void csp_reset(csp_scan *s) {
    s->n = s->have_pos = s->have_id = 0;
    s->pos = 0;
    memset(&s->st, 0, sizeof s->st);
    s->state = CSP_SCANNING;
}
void csp_restart(csp_scan *s) { if(s) csp_reset(s); }
csp_state csp_state_of(const csp_scan *s) { return s ? s->state : CSP_ERROR; }
int csp_count(const csp_scan *s) { return s ? s->n : 0; }
const csp_entry *csp_entry_at(const csp_scan *s, int i) { return s && i >= 0 && i < s->n ? &s->e[i] : NULL; }
const csp_stats *csp_stats_of(const csp_scan *s) { return s ? &s->st : NULL; }
int csp_path(const csp_scan *s, int i, char *out, size_t len) {
    const csp_entry *e = csp_entry_at(s, i);
    int n = e ? snprintf(out, len, "%s/%s", s->dir, e->name) : -1;
    return n > 0 && (size_t)n < len ? 0 : -1;
}

static int csp_cmp(const char *a, const char *b) {
    int c = strcasecmp(a, b);
    return c ? c : strcmp(a, b);
}
/* Sorted insert keeping the CSP_MAX_ENTRIES smallest, so the result does not depend on readdir order. */
static void csp_insert(csp_scan *s, const char *name, int kind, long long size) {
    int lo = 0, hi = s->n;
    while(lo < hi) {
        int mid = (lo + hi) / 2, c = csp_cmp(s->e[mid].name, name);
        if(!c) return;   /* seen twice after a directory change */
        if(c < 0) lo = mid + 1; else hi = mid;
    }
    if(lo >= CSP_MAX_ENTRIES) { s->st.hidden++; return; }
    if(s->n == CSP_MAX_ENTRIES) { s->n--; s->st.hidden++; }
    memmove(&s->e[lo + 1], &s->e[lo], (size_t)(s->n - lo) * sizeof s->e[0]);
    size_t n = strnlen(name, CSP_NAME_MAX - 1);   /* source_kind already refused names over 231 bytes */
    memcpy(s->e[lo].name, name, n);
    s->e[lo].name[n] = 0;
    s->e[lo].kind = kind;
    s->e[lo].size = size;
    s->n++;
}

/* The shared limit check; the descriptor is the caller's. */
static custom_gif_status csp_check(int fd, int kind, off_t size) {
    return kind == CUSTOM_SOURCE_GIF ? custom_gif_check_fd(fd, size) : custom_still_check_fd(fd, size);
}
static void csp_examine(csp_scan *s, int dfd, const struct dirent *de) {
    const char *name = de->d_name;
    s->st.examined++;
    if(name[0] == '.') { if(strcmp(name, ".") && strcmp(name, "..")) s->st.dotfiles++; return; }
    int kind = custom_saver_source_kind(name);
    if(!kind) return;
    if(kind == CUSTOM_SOURCE_TOO_LONG) { s->st.name_long++; return; }
    if(kind == CUSTOM_SOURCE_BAD_NAME) { s->st.name_bad++; return; }
    struct stat sb;
    if(de->d_type != DT_REG && de->d_type != DT_UNKNOWN) { s->st.not_regular++; return; }
    if(de->d_type == DT_UNKNOWN) {
        int rc = fstatat(dfd, name, &sb, AT_SYMLINK_NOFOLLOW);
        if(rc) {
            if(errno != ENOENT) s->st.unreadable++;
            return;
        }
        if(!S_ISREG(sb.st_mode)) { s->st.not_regular++; return; }
    }
    int fd = openat(dfd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) {
        if(errno == ELOOP) s->st.not_regular++;
        else if(errno != ENOENT) s->st.unreadable++;   /* ENOENT: removed since it was listed */
        return;
    }
    if(fstat(fd, &sb)) s->st.unreadable++;
    else if(!S_ISREG(sb.st_mode)) s->st.not_regular++;
    else switch(csp_check(fd, kind, sb.st_size)) {
        case CUSTOM_GIF_OK: csp_insert(s, name, kind, sb.st_size); break;
        case CUSTOM_GIF_TOO_LARGE: s->st.too_large++; break;
        case CUSTOM_GIF_DIMENSIONS: s->st.dimensions++; break;
        case CUSTOM_GIF_SD: s->st.unreadable++; break;
        default: s->st.invalid++; break;
    }
    close(fd);
}

csp_state csp_step(csp_scan *s, unsigned budget) {
    if(!s) return CSP_ERROR;
    if(s->state == CSP_NO_CARD) csp_reset(s);
    if(s->state != CSP_SCANNING) return s->state;
    if(!sd_io_begin()) { csp_reset(s); return s->state = CSP_NO_CARD; }
    int dfd = open(s->dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat sb;
    DIR *d = NULL;
    if(dfd < 0) {
        int missing = errno == ENOENT;
        csp_reset(s);   /* nothing listed earlier can still be trusted */
        if(missing) { s->st.folder_missing = 1; s->state = CSP_EMPTY; }
        else s->state = CSP_ERROR;
    } else if(fstat(dfd, &sb) || !(d = fdopendir(dfd))) {
        if(!d) close(dfd);
        s->state = CSP_ERROR;
    }
    if(d) {
        if(s->have_id && (sb.st_dev != s->dev || sb.st_mtim.tv_sec != s->mt.tv_sec || sb.st_mtim.tv_nsec != s->mt.tv_nsec))
            csp_reset(s);   /* other card, or the entry set changed since the last step */
        s->dev = sb.st_dev; s->mt = sb.st_mtim; s->have_id = 1;
        if(s->have_pos) seekdir(d, s->pos);
        int done = 0, read = 0;
        for(unsigned i = 0; i < (budget ? budget : 1); i++) {
            errno = 0;
            const struct dirent *de = readdir(d);
            if(!de) { if(errno) s->state = CSP_ERROR; else done = 1; break; }
            if(s->st.examined >= CSP_EXAMINE_MAX) { s->st.truncated = 1; done = 1; break; }   /* only when more really exists */
            csp_examine(s, dirfd(d), de);
            read++;
        }
        if(s->state == CSP_SCANNING) {
            if(done) s->state = s->n ? CSP_READY : CSP_EMPTY;
            else {
                long p = telldir(d);
                if(read && (p <= 0 || (s->have_pos && p == s->pos))) s->state = CSP_ERROR;   /* offset does not advance: cannot resume safely */
                else { s->pos = p; s->have_pos = 1; }
            }
        }
        closedir(d);
    }
    sd_io_end();
    return s->state;
}

csp_reval csp_revalidate(csp_scan *s, int i) {
    const csp_entry *e = csp_entry_at(s, i);
    if(!e) return CSP_REV_INDEX;
    if(!sd_io_begin()) return CSP_REV_NO_CARD;
    csp_reval r = CSP_REV_OK;
    struct stat sb;
    int fd = -1, dfd = open(s->dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if(dfd < 0) r = errno == ENOENT ? CSP_REV_GONE : CSP_REV_UNREADABLE;
    else if(fstatat(dfd, e->name, &sb, AT_SYMLINK_NOFOLLOW)) r = errno == ENOENT ? CSP_REV_GONE : CSP_REV_UNREADABLE;
    else if(!S_ISREG(sb.st_mode)) r = CSP_REV_CHANGED;
    else if((fd = openat(dfd, e->name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)) < 0)
        r = errno == ENOENT ? CSP_REV_GONE : errno == ELOOP ? CSP_REV_CHANGED : CSP_REV_UNREADABLE;
    else if(fstat(fd, &sb)) r = CSP_REV_UNREADABLE;
    else if(!S_ISREG(sb.st_mode) || sb.st_size != e->size) r = CSP_REV_CHANGED;
    else r = csp_check(fd, e->kind, sb.st_size) == CUSTOM_GIF_OK ? CSP_REV_OK : CSP_REV_REFUSED;
    if(fd >= 0) close(fd);
    if(dfd >= 0) close(dfd);
    sd_io_end();
    return r;
}
