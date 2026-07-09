/* =============================================================================
 * lpanel.h - Local filesystem panel (left side of the screen)
 * -----------------------------------------------------------------------------
 * Reads the current DOS directory. On plain DOS (no LFN) it uses the
 * _dos_findfirst/_dos_findnext API (8.3 names only). When LFN is detected
 * (lfn_available() != 0), it switches to the Int 21h AX=714Eh/714Fh API
 * which returns the full long filename. Long names that don't fit in
 * PanelEntry::name are stored in a name pool and reached via fullname, the
 * same mechanism used by RemotePanel for long FTP names.
 *
 * Sorting: ".." first, then directories, then files — all alphabetically.
 * ===========================================================================*/
#ifndef LPANEL_H
#define LPANEL_H

#include "panel.h"
#include "lfn.h"

/* Name pool for LFN entries whose name exceeds PANEL_NAME_MAX-1 characters.
 * Sized for ~100 long names (typical DOS directory). */
#define LOCAL_NAME_POOL 8192U

class LocalPanel : public Panel {
public:
    LocalPanel();
    ~LocalPanel();

    int  refresh();             /* re-read the current directory             */
    int  enter_selected();      /* override: 1 = changed directory, 0 = file */
    void go_parent();           /* override: switch to the parent directory  */

    const char *path() const { return cwd; }

private:
    char cwd[PANEL_HEADER_MAX]; /* current working directory (with drive)    */

    /* Pool for LFN entries whose name exceeds PANEL_NAME_MAX-1 bytes.
     * Allocated lazily on first refresh(); reset (poolUsed=0) each refresh.
     * Entries point into this pool via PanelEntry::fullname. */
    char    *namePool;
    unsigned poolUsed;
    unsigned poolSize;

    void read_cwd();            /* fetch cwd from DOS (LFN-aware)            */
    char *pool_store(const char *s); /* intern a long name; 0 if pool full   */
};

#endif /* LPANEL_H */
