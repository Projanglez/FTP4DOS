/* =============================================================================
 * lfn.h - Long Filename (LFN) wrappers for MS-DOS 7.x (Int 21h AX=71xxh)
 * -----------------------------------------------------------------------------
 * The LFN providers (Win9x IO.SYS, DOSLFN) implement ONLY the 71xx functions;
 * the classic Int 21h calls the C library maps to (3Bh chdir, 3Ch create,
 * 3Dh open, 39h mkdir, 41h delete, 56h rename, 4Eh findfirst) remain strictly
 * 8.3 and fail on long paths. Wrapped here:
 *   - Detection (lfn_detect / lfn_available)
 *   - FindFirst/FindNext/FindClose (714Eh/714Fh/71A1h)
 *   - GetCWD (7147h)
 *   - chdir/mkdir/remove/rename (713Bh/7139h/7141h/7156h)
 *   - fopen with create support (716Ch + fdopen)
 *   - lfn_normalize_path: convert a path to its 8.3 alias so the remaining
 *     plain C library calls keep working (used by join_local)
 * Every wrapper falls back to the C library on non-LFN systems.
 *
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#ifndef LFN_H
#define LFN_H

#include <stdio.h>   /* FILE (lfn_fopen) */

/* Call once at startup. Probes Int 21h/AX=71A0h (Get Volume Information). */
void lfn_detect(void);

/* Returns 1 if LFN is available on this system (set by lfn_detect). */
int lfn_available(void);

/* WIN32_FIND_DATA as returned by Int 21h AX=714Eh/714Fh. The three time
 * fields are 8-byte FILETIME slots even when the DOS date/time format is
 * requested (SI=1): the value then sits in the LOW DWORD (HIWORD=date,
 * LOWORD=time) and the high DWORD is zero. */
#pragma pack(1)
struct LfnFindData {
    unsigned long  attr;         /* file attributes                              */
    unsigned long  ctime;        /* creation date/time (HIWORD=date,LOWORD=time) */
    unsigned long  ctime_hi;     /* high DWORD of the FILETIME slot (0 if SI=1)  */
    unsigned long  atime;        /* last-access date (HIWORD=date, LOWORD=0)     */
    unsigned long  atime_hi;
    unsigned long  wtime;        /* last-write date/time (HIWORD=date,LOWORD=time)*/
    unsigned long  wtime_hi;
    unsigned long  size_hi;      /* file size high DWORD (always 0 on FAT16)     */
    unsigned long  size_lo;      /* file size low DWORD                          */
    unsigned long  reserved0;
    unsigned long  reserved1;
    char           name[260];    /* long filename (ASCIIZ)                       */
    char           short_name[14]; /* 8.3 alternative name (ASCIIZ)              */
};
#pragma pack()

/* DOS attribute bits (mirror of _A_* in dos.h). */
#define LFN_A_RDONLY  0x01
#define LFN_A_HIDDEN  0x02
#define LFN_A_SYSTEM  0x04
#define LFN_A_VOLID   0x08
#define LFN_A_SUBDIR  0x10
#define LFN_A_ARCH    0x20

/* FindFirst: returns a search handle (>= 0) on success, -1 on error / empty.
 * Matches all entries (dirs, hidden, system, read-only, archive) except
 * volume labels — equivalent to _dos_findfirst with _A_SUBDIR|_A_HIDDEN|...). */
int  lfn_findfirst(const char *path, LfnFindData *fd);

/* FindNext: 0 = ok (fd filled), -1 = no more entries or error. */
int  lfn_findnext(int handle, LfnFindData *fd);

/* FindClose: release the search handle returned by lfn_findfirst. */
void lfn_findclose(int handle);

/* Get the long-filename current working directory (including drive prefix).
 * Writes "X:\path" into buf (bufsz bytes). Returns 0 on success, -1 on error. */
int lfn_getcwd(char *buf, int bufsz);

/* Rewrite 'path' in place with its short (8.3) alias so the SFN-only C
 * library calls can operate on it. An existing path converts fully; for a
 * path yet to be created (or a wildcard pattern) only the directory part is
 * converted and the leaf is kept - creation then goes through lfn_fopen /
 * lfn_mkdir / lfn_rename. No-op on non-LFN systems. */
void lfn_normalize_path(char *path, int pathsz);

/* fopen replacement that can also open/create long-named files (AX=716Ch +
 * fdopen). mode "w..." creates/truncates, anything else opens for reading.
 * Falls back to fopen() on non-LFN systems. */
FILE *lfn_fopen(const char *path, const char *mode);

/* chdir/mkdir/remove/rename accepting long paths (713Bh/7139h/7141h/7156h).
 * Return 0 on success, -1 on error; fall back to the C library on non-LFN
 * systems. */
int lfn_chdir(const char *path);
int lfn_mkdir(const char *path);
int lfn_remove(const char *path);
int lfn_rename(const char *oldpath, const char *newpath);

#endif /* LFN_H */
