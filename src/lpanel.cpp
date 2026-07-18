/* =============================================================================
 * lpanel.cpp - Local filesystem panel
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 *
 * When LFN is available (lfn_available() != 0) refresh() uses the
 * Int 21h AX=714Eh/714Fh API to enumerate the directory so that long file
 * names are returned. Names that exceed PANEL_NAME_MAX-1 characters are
 * interned in the namePool and accessed via PanelEntry::fullname (same seam
 * used by RemotePanel for long FTP names). The display name in entry::name
 * is truncated to PANEL_NAME_MAX-1 chars with a trailing '>' marker.
 *
 * On plain DOS (no LFN) the code falls back to _dos_findfirst/_dos_findnext,
 * which is identical to the original behaviour.
 * ===========================================================================*/
#include <dos.h>      /* _dos_findfirst/_dos_findnext, _A_*, struct find_t   */
#include <direct.h>   /* getcwd, chdir, _mkdir                               */
#include <string.h>   /* strncpy, strcpy, stricmp, strncat, strlen           */
#include <stdio.h>    /* sprintf                                              */
#include <stdlib.h>   /* malloc, free                                        */

#include "lpanel.h"

/* Determine the last path component ("leaf name"). "C:\FOO\BAR" -> "BAR",
 * "C:\" -> "" (the root has no leaf). */
static void path_leaf(const char *path, char *out, int outsz)
{
    const char *p, *leaf = path;
    int n = 0;
    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') leaf = p + 1;
    while (leaf[n] && n < outsz - 1) { out[n] = leaf[n]; n++; }
    out[n] = '\0';
}

LocalPanel::LocalPanel()
{
    cwd[0]   = '\0';
    namePool = 0;
    poolUsed = 0;
    poolSize = 0;
}

LocalPanel::~LocalPanel()
{
    free(namePool);
    namePool = 0;
}

/* Intern a long name into the pool. Returns a stable pointer or 0 if full. */
char *LocalPanel::pool_store(const char *s)
{
    unsigned len = (unsigned)strlen(s) + 1;
    if (!namePool || poolUsed + len > poolSize) return 0;
    char *dst = namePool + poolUsed;
    memcpy(dst, s, len);
    poolUsed += len;
    return dst;
}

/* Determine the current working directory (including drive). */
void LocalPanel::read_cwd()
{
    if (lfn_available()) {
        if (lfn_getcwd(cwd, PANEL_HEADER_MAX) == 0) return;
    }
    /* Fallback: standard getcwd (8.3 path on non-LFN systems). */
    if (getcwd(cwd, PANEL_HEADER_MAX) == 0)
        strcpy(cwd, "C:\\");
}

/* Re-read the directory. Returns: number of entries. */
int LocalPanel::refresh()
{
    count     = 0;
    total     = 0;
    truncated = 0;
    store->reset();
    read_cwd();
    strncpy(header, cwd, PANEL_HEADER_MAX - 1);
    header[PANEL_HEADER_MAX - 1] = '\0';

    /* Lazy-allocate the name pool; reset it on every refresh. */
    if (namePool == 0) {
        namePool = (char *)malloc(LOCAL_NAME_POOL);
        poolSize = namePool ? LOCAL_NAME_POOL : 0;
    }
    poolUsed = 0;

    if (lfn_available()) {
        /* ---- LFN path: Int 21h AX=714Eh/714Fh ---- */
        LfnFindData fd;
        int handle = lfn_findfirst("*", &fd);
        while (handle >= 0) {
            /* Skip "." but keep ".." */
            if (!(fd.name[0] == '.' && fd.name[1] == '\0')) {
                PanelEntry e;
                total++;
                /* Store the full LFN in the pool when it exceeds the name
                 * column; truncate the display name with a '>' marker. */
                int namelen = (int)strlen(fd.name);
                if (namelen >= PANEL_NAME_MAX) {
                    strncpy(e.name, fd.name, PANEL_NAME_MAX - 2);
                    e.name[PANEL_NAME_MAX - 2] = '>';
                    e.name[PANEL_NAME_MAX - 1] = '\0';
                    e.fullname = pool_store(fd.name);
                } else {
                    strncpy(e.name, fd.name, PANEL_NAME_MAX - 1);
                    e.name[PANEL_NAME_MAX - 1] = '\0';
                    e.fullname = 0;
                }
                /* LFN wtime: HIWORD = DOS date, LOWORD = DOS time. */
                e.date     = (unsigned)(fd.wtime >> 16);
                e.time     = (unsigned)(fd.wtime & 0xFFFFU);
                e.size     = fd.size_lo;
                e.is_dir   = (fd.attr & LFN_A_SUBDIR) ? 1 : 0;
                e.is_parent = (fd.name[0] == '.' && fd.name[1] == '.'
                               && fd.name[2] == '\0') ? 1 : 0;
                e.marked   = 0;
                if (store->append(&e)) count++;
                else                   truncated = 1;
            }
            if (lfn_findnext(handle, &fd) != 0) {
                lfn_findclose(handle);
                break;
            }
        }
    } else {
        /* ---- SFN path: _dos_findfirst/_dos_findnext (original) ---- */
        struct find_t ff;
        unsigned amask = _A_SUBDIR | _A_HIDDEN | _A_SYSTEM | _A_RDONLY | _A_ARCH;
        unsigned rc = _dos_findfirst("*.*", amask, &ff);
        while (rc == 0) {
            if (!(ff.name[0] == '.' && ff.name[1] == '\0')) {
                PanelEntry e;
                total++;
                strncpy(e.name, ff.name, PANEL_NAME_MAX - 1);
                e.name[PANEL_NAME_MAX - 1] = '\0';
                e.fullname  = 0;
                e.size      = ff.size;
                e.date      = ff.wr_date;
                e.time      = ff.wr_time;
                e.is_dir    = (ff.attrib & _A_SUBDIR) ? 1 : 0;
                e.is_parent = (ff.name[0] == '.' && ff.name[1] == '.'
                               && ff.name[2] == '\0') ? 1 : 0;
                e.marked    = 0;
                if (store->append(&e)) count++;
                else                   truncated = 1;
            }
            rc = _dos_findnext(&ff);
        }
    }

    sort_entries();
    cursor   = 0;
    topentry = 0;
    return count;
}

/* Enter on the selected entry.
 * Returns: 1 = entered a directory (panel re-read),
 *          0 = file (caller handles viewing/launching it). */
int LocalPanel::enter_selected()
{
    PanelEntry *e = selected();
    if (e == 0)     return 0;
    if (!e->is_dir) return 0;

    if (e->is_parent) {
        char leaf[260];         /* full LFN size so long leaves reselect */
        path_leaf(cwd, leaf, sizeof(leaf));
        lfn_chdir("..");
        refresh();
        select_by_name(leaf);
    } else {
        /* lfn_chdir (713Bh): plain chdir (3Bh) stays 8.3-only even with an
         * LFN provider loaded; entry_name() is the long directory name. */
        lfn_chdir(entry_name(e));
        refresh();
    }
    return 1;
}

/* Backspace: go to the parent directory (no effect at the root).
 * Afterwards the cursor sits on the directory just left. */
void LocalPanel::go_parent()
{
    char leaf[260];             /* full LFN size so long leaves reselect */
    path_leaf(cwd, leaf, sizeof(leaf));
    lfn_chdir("..");
    refresh();
    select_by_name(leaf);
}
