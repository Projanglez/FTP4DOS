/* =============================================================================
 * lfn.h - Long Filename (LFN) wrappers for MS-DOS 7.x (Int 21h AX=71xxh)
 * -----------------------------------------------------------------------------
 * Only the parts that don't work via the standard C library are wrapped here:
 *   - Detection (lfn_detect / lfn_available)
 *   - FindFirst/FindNext/FindClose (8.3 API returns only short names)
 *   - GetCWD (Int 21h AX=47h returns only the short path)
 *
 * All other LFN-capable operations (mkdir, rmdir, remove, rename, chdir) are
 * already handled transparently by DOSLFN's Int 21h intercept, so the standard
 * C library functions (_mkdir, remove, rename, chdir, ...) work on LFN paths
 * without wrappers.
 *
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#ifndef LFN_H
#define LFN_H

/* Call once at startup. Probes Int 21h/AX=7100h (LFN NULL function). */
void lfn_detect(void);

/* Returns 1 if LFN is available on this system (set by lfn_detect). */
int lfn_available(void);

/* WIN32_FIND_DATA with SI=0 (DOS date/time: HIWORD=date, LOWORD=time). */
#pragma pack(1)
struct LfnFindData {
    unsigned long  attr;         /* file attributes                              */
    unsigned long  ctime;        /* creation date/time (HIWORD=date,LOWORD=time) */
    unsigned long  atime;        /* last-access date (HIWORD=date, LOWORD=0)     */
    unsigned long  wtime;        /* last-write date/time (HIWORD=date,LOWORD=time)*/
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

#endif /* LFN_H */
