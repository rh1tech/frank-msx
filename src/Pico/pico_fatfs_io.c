/*
 * pico_fatfs_io.c — newlib POSIX syscall retargeting for fMSX.
 *
 * fMSX uses fopen/fread/fwrite/... and getcwd/chdir. Under the Pico SDK
 * these go through newlib. We override _open/_read/_write/_close/_lseek/
 * _fstat/_isatty (weak in the SDK) so they talk to FatFS for any
 * non-stdio descriptor, while stdio (fd 0/1/2) keeps going through the
 * Pico's USB/UART stdio driver.
 *
 * We also provide our own chdir/getcwd so fMSX's ProgDir/WorkDir dance
 * works transparently on the SD card.
 */

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "pico/stdio.h"
#include "pico/stdlib.h"

#include "ff.h"

#define MAX_OPEN_FILES 8
#define FD_BASE        16   /* stay well clear of stdio (0,1,2) */

typedef struct {
    int  used;
    int  is_dir;
    FIL  fp;
    DIR  dir;
} fatfs_slot_t;

static fatfs_slot_t g_slots[MAX_OPEN_FILES];

/* Current working directory, relative to SD root ("" == root). */
static char g_cwd[256] = "";

/* ---- Path helpers ---- */

static void path_join(char *dst, size_t dst_sz, const char *cwd, const char *in) {
    if (!in || !*in) { dst[0] = 0; return; }
    if (in[0] == '/') {
        /* absolute-on-SD */
        size_t n = strnlen(in, dst_sz - 1);
        memcpy(dst, in + 1, n);
        dst[n] = 0;
    } else if (cwd[0]) {
        snprintf(dst, dst_sz, "%s/%s", cwd, in);
    } else {
        strncpy(dst, in, dst_sz - 1);
        dst[dst_sz - 1] = 0;
    }
}

static fatfs_slot_t *slot_alloc(int *fd_out) {
    for (int i = 0; i < MAX_OPEN_FILES; ++i) {
        if (!g_slots[i].used) {
            g_slots[i].used = 1;
            g_slots[i].is_dir = 0;
            *fd_out = FD_BASE + i;
            return &g_slots[i];
        }
    }
    return NULL;
}

static fatfs_slot_t *slot_get(int fd) {
    int idx = fd - FD_BASE;
    if (idx < 0 || idx >= MAX_OPEN_FILES) return NULL;
    if (!g_slots[idx].used) return NULL;
    return &g_slots[idx];
}

/* ---- newlib syscall overrides ----
 * These are __attribute__((weak)) in the Pico SDK, so our strong
 * definitions win at link time.
 */

int _open(const char *name, int flags, int mode) {
    (void)mode;
    if (!name) { errno = EINVAL; return -1; }
    char path[300];
    path_join(path, sizeof(path), g_cwd, name);

    BYTE fmode = 0;
    int rw = flags & (O_RDONLY | O_WRONLY | O_RDWR);
    if (rw == O_RDONLY) fmode = FA_READ;
    else if (rw == O_WRONLY) fmode = FA_WRITE;
    else fmode = FA_READ | FA_WRITE;
    if (flags & O_CREAT)  fmode |= FA_OPEN_ALWAYS;
    if (flags & O_TRUNC)  fmode |= FA_CREATE_ALWAYS;
    if (flags & O_APPEND) fmode |= FA_OPEN_APPEND;
    if ((flags & (O_CREAT | O_TRUNC)) == 0 && rw == O_RDONLY)
        fmode = FA_READ;

    int fd;
    fatfs_slot_t *s = slot_alloc(&fd);
    if (!s) { errno = EMFILE; return -1; }

    FRESULT r = f_open(&s->fp, path, fmode);
    if (r != FR_OK) {
        s->used = 0;
        switch (r) {
            case FR_NO_FILE:
            case FR_NO_PATH: errno = ENOENT; break;
            case FR_DENIED:  errno = EACCES; break;
            case FR_EXIST:   errno = EEXIST; break;
            case FR_DISK_ERR:
            case FR_INT_ERR: errno = EIO;    break;
            default:         errno = EIO;    break;
        }
        return -1;
    }
    return fd;
}

int _close(int fd) {
    if (fd < FD_BASE) return 0;          /* stdio descriptors */
    fatfs_slot_t *s = slot_get(fd);
    if (!s) { errno = EBADF; return -1; }
    if (s->is_dir) f_closedir(&s->dir);
    else           f_close(&s->fp);
    s->used = 0;
    return 0;
}

int _read(int fd, char *buf, int len) {
    if (fd < FD_BASE) return 0;          /* no stdin from SD */
    fatfs_slot_t *s = slot_get(fd);
    if (!s || s->is_dir) { errno = EBADF; return -1; }
    UINT br = 0;
    FRESULT r = f_read(&s->fp, buf, len, &br);
    if (r != FR_OK) { errno = EIO; return -1; }
    return (int)br;
}

int _write(int fd, const char *buf, int len) {
    if (fd < FD_BASE) {
        /* stdout / stderr: fall back to Pico's default stdio */
        extern int stdio_puts(const char *s);
        (void)stdio_puts;
        /* The Pico SDK's weak _write writes to the stdio driver; we can
         * reach it by falling through newlib's default. The simplest
         * approach is to use fwrite on stderr, but that recurses. Instead
         * we use stdio_putchar_raw via Pico SDK. */
        for (int i = 0; i < len; ++i) putchar_raw((unsigned char)buf[i]);
        return len;
    }
    fatfs_slot_t *s = slot_get(fd);
    if (!s || s->is_dir) { errno = EBADF; return -1; }
    UINT bw = 0;
    FRESULT r = f_write(&s->fp, buf, len, &bw);
    if (r != FR_OK) { errno = EIO; return -1; }
    return (int)bw;
}

off_t _lseek(int fd, off_t off, int whence) {
    if (fd < FD_BASE) { errno = ESPIPE; return -1; }
    fatfs_slot_t *s = slot_get(fd);
    if (!s || s->is_dir) { errno = EBADF; return -1; }
    FSIZE_t target;
    switch (whence) {
        case SEEK_SET: target = (FSIZE_t)off; break;
        case SEEK_CUR: target = (FSIZE_t)(s->fp.fptr + off); break;
        case SEEK_END: target = (FSIZE_t)(f_size(&s->fp) + off); break;
        default: errno = EINVAL; return -1;
    }
    FRESULT r = f_lseek(&s->fp, target);
    if (r != FR_OK) { errno = EIO; return -1; }
    return (off_t)s->fp.fptr;
}

int _fstat(int fd, struct stat *st) {
    memset(st, 0, sizeof(*st));
    if (fd < FD_BASE) { st->st_mode = S_IFCHR; return 0; }
    fatfs_slot_t *s = slot_get(fd);
    if (!s) { errno = EBADF; return -1; }
    if (s->is_dir) {
        st->st_mode = S_IFDIR;
    } else {
        st->st_mode = S_IFREG;
        st->st_size = (off_t)f_size(&s->fp);
    }
    return 0;
}

int _isatty(int fd) {
    return fd < FD_BASE;
}

/* Non-standard POSIX bits that fMSX touches (ProgDir chdir, getcwd). */

int chdir(const char *path) {
    if (!path) { errno = EINVAL; return -1; }
    if (path[0] == 0) return 0;
    /* Accept absolute ("/foo/bar") or relative paths. We don't actually
     * change FatFS's directory — we just update our own g_cwd so future
     * path_join() calls compose the right path. */
    if (path[0] == '/') {
        size_t n = strlen(path + 1);
        if (n >= sizeof(g_cwd)) { errno = ENAMETOOLONG; return -1; }
        memcpy(g_cwd, path + 1, n + 1);
    } else {
        size_t cur = strlen(g_cwd);
        size_t add = strlen(path);
        if (cur + 1 + add + 1 > sizeof(g_cwd)) { errno = ENAMETOOLONG; return -1; }
        if (cur) { g_cwd[cur] = '/'; memcpy(g_cwd + cur + 1, path, add + 1); }
        else     { memcpy(g_cwd, path, add + 1); }
    }
    /* Strip trailing slashes */
    size_t L = strlen(g_cwd);
    while (L > 0 && g_cwd[L - 1] == '/') g_cwd[--L] = 0;
    return 0;
}

char *getcwd(char *buf, size_t size) {
    if (!buf) {
        buf = malloc(size ? size : 512);
        if (!buf) { errno = ENOMEM; return NULL; }
        size = size ? size : 512;
    }
    size_t L = strlen(g_cwd);
    if (L + 2 > size) { errno = ERANGE; return NULL; }
    buf[0] = '/';
    memcpy(buf + 1, g_cwd, L + 1);
    return buf;
}
