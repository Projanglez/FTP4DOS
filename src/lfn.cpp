/* =============================================================================
 * lfn.cpp - Long Filename (LFN) wrappers (Int 21h AX=71xxh)
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * In the Large Model, all data pointers are far; FP_SEG/FP_OFF extract the
 * segment:offset needed by int86x() to load DS/ES before the Int 21h call.
 * ===========================================================================*/
#include <i86.h>      /* int86, int86x, union REGS, struct SREGS, FP_SEG/FP_OFF */
#include <string.h>   /* memset, strncat, strlen */
#include <stdio.h>    /* fopen, fdopen (lfn_fopen fallback/wrap) */
#include <direct.h>   /* chdir, _mkdir (non-LFN fallbacks) */

#include "lfn.h"

static int g_lfn = 0;   /* 1 if LFN is available on this system */

/* Probe Int 21h/AX=71A0h (Get Volume Information) on the current drive's
 * root. The previously used AX=7100h "NULL function" is undocumented and
 * DOSLFN (unlike Win9x IO.SYS) does not answer it, so LFN went undetected
 * on MS-DOS 7.x + DOSLFN. 71A0h is a documented LFN function every provider
 * implements. Discrimination against the DOS 6.22 stub (which returns CF=0
 * with all registers unchanged for any AH=71h call): CX is the buffer size
 * on input and the maximum filename length on output. We pass CX=12; a real
 * LFN provider overwrites it with 255, the stub leaves it at 12. The FAT/
 * VFAT/CDFS filesystem name always fits the 12-byte buffer. */
void lfn_detect(void)
{
    union  REGS  r;
    struct SREGS sr;
    char root[4];
    char fsname[32];

    /* Current drive (AH=19h: 0=A:, 1=B:, ...) -> "X:\" root path. */
    memset(&r, 0, sizeof(r));
    r.h.ah = 0x19;
    int86(0x21, &r, &r);
    root[0] = (char)('A' + r.h.al);
    root[1] = ':';
    root[2] = '\\';
    root[3] = '\0';

    memset(fsname, 0, sizeof(fsname));
    memset(&r,  0, sizeof(r));
    memset(&sr, 0, sizeof(sr));
    r.w.ax = 0x71A0;
    r.w.cx = 12;            /* buffer size in; max name length out (255)   */
    sr.ds  = FP_SEG(root);
    r.w.dx = FP_OFF(root);
    sr.es  = FP_SEG(fsname);
    r.w.di = FP_OFF(fsname);
    int86x(0x21, &r, &r, &sr);
    g_lfn = (r.w.cflag == 0 && r.w.cx > 12) ? 1 : 0;
}

int lfn_available(void) { return g_lfn; }

/* FindFirst via AX=714Eh with SI=1 (DOS date/time; SI=0 would request
 * 64-bit Win32 FILETIMEs).
 * CL = allowable attributes: everything except the volume-label bit, so
 * directories and hidden/system files are included like _dos_findfirst
 * with _A_SUBDIR|_A_HIDDEN|... CH = required attributes: none. */
int lfn_findfirst(const char *path, LfnFindData *fd)
{
    union  REGS  r;
    struct SREGS sr;
    memset(fd, 0, sizeof(*fd));
    memset(&r,  0, sizeof(r));
    memset(&sr, 0, sizeof(sr));
    r.w.ax = 0x714E;
    r.w.cx = LFN_A_RDONLY | LFN_A_HIDDEN | LFN_A_SYSTEM |
             LFN_A_SUBDIR | LFN_A_ARCH;   /* CL = allowable, CH = 0 required */
    r.w.si = 1;             /* 1 = return times in DOS date/time format */
    sr.ds  = FP_SEG(path);
    r.w.dx = FP_OFF(path);
    sr.es  = FP_SEG(fd);
    r.w.di = FP_OFF(fd);
    int86x(0x21, &r, &r, &sr);
    if (r.w.cflag) return -1;
    return (int)(unsigned)r.w.ax;   /* search handle */
}

/* FindNext via AX=714Fh. */
int lfn_findnext(int handle, LfnFindData *fd)
{
    union  REGS  r;
    struct SREGS sr;
    memset(fd, 0, sizeof(*fd));
    memset(&r,  0, sizeof(r));
    memset(&sr, 0, sizeof(sr));
    r.w.ax = 0x714F;
    r.w.bx = (unsigned)handle;
    r.w.si = 1;   /* DOS date/time format */
    sr.es  = FP_SEG(fd);
    r.w.di = FP_OFF(fd);
    int86x(0x21, &r, &r, &sr);
    return r.w.cflag ? -1 : 0;
}

/* FindClose via AX=71A1h. */
void lfn_findclose(int handle)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x71A1;
    r.w.bx = (unsigned)handle;
    int86(0x21, &r, &r);
}

/* Get the LFN current working directory via AX=7147h.
 * Returns the path without drive prefix and without leading backslash, so we
 * prepend "X:\" using the current drive from AH=19h. */
int lfn_getcwd(char *buf, int bufsz)
{
    union  REGS  r;
    struct SREGS sr;
    char tmp[260];
    int  drvnum;

    /* Get current drive (0=A:, 1=B:, 2=C:, ...). */
    memset(&r, 0, sizeof(r));
    r.h.ah = 0x19;
    int86(0x21, &r, &r);
    drvnum = (int)(unsigned char)r.h.al;

    /* AX=7147h: DL=0 (current drive), DS:SI -> 260-byte output buffer. */
    memset(tmp, 0, sizeof(tmp));
    memset(&r,  0, sizeof(r));
    memset(&sr, 0, sizeof(sr));
    r.w.ax  = 0x7147;
    r.h.dl  = 0;            /* 0 = current drive */
    sr.ds   = FP_SEG(tmp);
    r.w.si  = FP_OFF(tmp);
    int86x(0x21, &r, &r, &sr);
    if (r.w.cflag) return -1;

    /* Build "X:\path": the drive letter + separator, then append tmp if non-empty. */
    if (bufsz < 4) return -1;
    buf[0] = (char)('A' + drvnum);
    buf[1] = ':';
    buf[2] = '\\';
    buf[3] = '\0';
    if (tmp[0] != '\0') {
        int n = 3;
        strncat(buf, tmp, bufsz - n - 1);
        buf[bufsz - 1] = '\0';
    }
    return 0;
}

/* Truename via AX=7160h CL=1: canonical SHORT (8.3) form of an EXISTING
 * path. Returns 0 on success, -1 on error (also when the path does not
 * exist yet). */
static int lfn_true_short(const char *path, char *out, int outsz)
{
    union  REGS  r;
    struct SREGS sr;
    char tmp[260];
    memset(tmp, 0, sizeof(tmp));
    memset(&r,  0, sizeof(r));
    memset(&sr, 0, sizeof(sr));
    r.w.ax = 0x7160;
    r.h.cl = 1;             /* subfunction 1: get short path name */
    r.h.ch = 0;
    sr.ds  = FP_SEG(path);
    r.w.si = FP_OFF(path);
    sr.es  = FP_SEG(tmp);
    r.w.di = FP_OFF(tmp);
    int86x(0x21, &r, &r, &sr);
    if (r.w.cflag) return -1;
    if ((int)strlen(tmp) >= outsz) return -1;
    strcpy(out, tmp);
    return 0;
}

void lfn_normalize_path(char *path, int pathsz)
{
    char buf[260];
    char *sep;
    if (!g_lfn) return;

    /* Whole path first - succeeds whenever the path exists. */
    if (!strchr(path, '*') && !strchr(path, '?')) {
        if (lfn_true_short(path, buf, sizeof(buf)) == 0) {
            if ((int)strlen(buf) < pathsz) strcpy(path, buf);
            return;
        }
    }

    /* Leaf doesn't exist yet or is a wildcard pattern: convert only the
     * directory part (which does exist) and keep the leaf. */
    sep = strrchr(path, '\\');
    if (sep == 0 || sep == path) return;
    *sep = '\0';
    if (lfn_true_short(path, buf, sizeof(buf)) == 0 &&
        (int)(strlen(buf) + 1 + strlen(sep + 1)) < pathsz) {
        char leaf[260];
        strncpy(leaf, sep + 1, sizeof(leaf) - 1);
        leaf[sizeof(leaf) - 1] = '\0';
        strcpy(path, buf);
        if (path[strlen(path) - 1] != '\\') strcat(path, "\\");
        strcat(path, leaf);
    } else {
        *sep = '\\';        /* conversion failed: restore the original */
    }
}

FILE *lfn_fopen(const char *path, const char *mode)
{
    union  REGS  r;
    struct SREGS sr;
    int wr = (mode[0] == 'w');
    if (!g_lfn) return fopen(path, mode);
    memset(&r,  0, sizeof(r));
    memset(&sr, 0, sizeof(sr));
    r.w.ax = 0x716C;
    r.w.bx = wr ? 0x0002 : 0x0000;   /* access: read/write : read-only     */
    r.w.cx = wr ? 0x0020 : 0x0000;   /* attributes when creating: archive  */
    r.w.dx = wr ? 0x0012 : 0x0001;   /* create+truncate : open-if-exists   */
    r.w.di = 0;                      /* numeric-tail alias hint            */
    sr.ds  = FP_SEG(path);
    r.w.si = FP_OFF(path);
    int86x(0x21, &r, &r, &sr);
    if (r.w.cflag) return 0;
    return fdopen((int)r.w.ax, mode);   /* AX = DOS file handle */
}

int lfn_chdir(const char *path)
{
    union  REGS  r;
    struct SREGS sr;
    if (!g_lfn) return chdir(path);
    memset(&r,  0, sizeof(r));
    memset(&sr, 0, sizeof(sr));
    r.w.ax = 0x713B;
    sr.ds  = FP_SEG(path);
    r.w.dx = FP_OFF(path);
    int86x(0x21, &r, &r, &sr);
    return r.w.cflag ? -1 : 0;
}

int lfn_mkdir(const char *path)
{
    union  REGS  r;
    struct SREGS sr;
    if (!g_lfn) return _mkdir(path);
    memset(&r,  0, sizeof(r));
    memset(&sr, 0, sizeof(sr));
    r.w.ax = 0x7139;
    sr.ds  = FP_SEG(path);
    r.w.dx = FP_OFF(path);
    int86x(0x21, &r, &r, &sr);
    return r.w.cflag ? -1 : 0;
}

int lfn_remove(const char *path)
{
    union  REGS  r;
    struct SREGS sr;
    if (!g_lfn) return remove(path);
    memset(&r,  0, sizeof(r));
    memset(&sr, 0, sizeof(sr));
    r.w.ax = 0x7141;
    r.w.si = 0;             /* no wildcards */
    sr.ds  = FP_SEG(path);
    r.w.dx = FP_OFF(path);
    int86x(0x21, &r, &r, &sr);
    return r.w.cflag ? -1 : 0;
}

int lfn_rename(const char *oldpath, const char *newpath)
{
    union  REGS  r;
    struct SREGS sr;
    if (!g_lfn) return rename(oldpath, newpath);
    memset(&r,  0, sizeof(r));
    memset(&sr, 0, sizeof(sr));
    r.w.ax = 0x7156;
    sr.ds  = FP_SEG(oldpath);
    r.w.dx = FP_OFF(oldpath);
    sr.es  = FP_SEG(newpath);
    r.w.di = FP_OFF(newpath);
    int86x(0x21, &r, &r, &sr);
    return r.w.cflag ? -1 : 0;
}
