/* =============================================================================
 * lfn.cpp - Long Filename (LFN) wrappers (Int 21h AX=71xxh)
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * In the Large Model, all data pointers are far; FP_SEG/FP_OFF extract the
 * segment:offset needed by int86x() to load DS/ES before the Int 21h call.
 * ===========================================================================*/
#include <i86.h>      /* int86, int86x, union REGS, struct SREGS, FP_SEG/FP_OFF */
#include <string.h>   /* memset, strncat, strlen */

#include "lfn.h"

static int g_lfn = 0;   /* 1 if LFN is available on this system */

/* Probe Int 21h/AX=7100h (LFN NULL function).
 * Real LFN (DOSLFN / Win9x IO.SYS) clears CF and modifies AX (typically to 0).
 * DOS 6.22 returns CF=0 but leaves AX=0x7100 unchanged — it silently accepts
 * AH=71h but doesn't implement any of the 71xx functions. Checking AX!=0x7100
 * distinguishes real LFN from this stub behaviour. */
void lfn_detect(void)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x7100;
    int86(0x21, &r, &r);
    g_lfn = (r.w.cflag == 0 && r.w.ax != 0x7100) ? 1 : 0;
}

int lfn_available(void) { return g_lfn; }

/* FindFirst via AX=714Eh with SI=0 (DOS date/time).
 * BX=LFN_A_VOLID, CX=0 -> return every entry where (attr & 0x08)==0,
 * i.e. everything except volume labels. */
int lfn_findfirst(const char *path, LfnFindData *fd)
{
    union  REGS  r;
    struct SREGS sr;
    memset(fd, 0, sizeof(*fd));
    memset(&r,  0, sizeof(r));
    memset(&sr, 0, sizeof(sr));
    r.w.ax = 0x714E;
    r.w.bx = LFN_A_VOLID;  /* mask: check only volume-label bit */
    r.w.cx = 0;             /* required: volume-label bit must be 0 */
    r.w.si = 0;             /* 0 = return time in DOS date/time format */
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
    r.w.si = 0;   /* DOS date/time format */
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
