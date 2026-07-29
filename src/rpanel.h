/* =============================================================================
 * rpanel.h - FTP remote panel (right side of the screen)
 * -----------------------------------------------------------------------------
 * Fetches the directory listing via FtpClient::list() and converts the raw
 * LIST text lines into PanelEntry records. Supports the Unix "ls -l" format
 * and (as a bonus) the MS-DOS/IIS format. Enter changes directories via CWD,
 * Backspace goes up via CDUP.
 *
 * The panel only holds a pointer to the (externally managed) FtpClient.
 * Without a connection it shows an empty list with a note in the header.
 * ===========================================================================*/
#ifndef RPANEL_H
#define RPANEL_H

#include "panel.h"
#include "ftpcli.h"

/* Parse one raw listing line into a PanelEntry.
 * mlsd != 0: the line came from MLSD (RFC 3659) and is parsed as a fact list;
 * mlsd == 0: Unix "ls -l" or MS-DOS/IIS LIST output.
 * curYear supplies the year for LIST lines that only carry a time (no year).
 * 'full'/'fullcap' (optional) receive the untruncated name; pass 0 to ignore.
 * Returns 1 = recognized (e filled in: name, size, date, is_dir, marked=0,
 *         nameref=NAME_NONE), 0 = line not recognizable as an entry. Also used by the
 * recursive directory download (dircopy.cpp). */
int ftp_parse_list_line(const char *line, int curYear, PanelEntry *e,
                        char *full = 0, int fullcap = 0, int mlsd = 0);

class RemotePanel : public Panel {
public:
    RemotePanel();

    /* Attach the (externally created) FTP client. */
    void attach(FtpClient *client) { ftp = client; }

    int  refresh();             /* re-list the current remote directory       */
    int  enter_selected();      /* override: 1 = changed directory, 0 = file  */
    void go_parent();           /* override: CDUP                             */

    const char *path() const { return cwd; }

    /* Did the last navigation/listing action fail? + error text. */
    int         nav_failed() const { return navFailed; }
    const char *last_error() const { return ftp ? ftp->last_error() : ""; }

    /* How many entries of the last listing had a name too long for the name
     * pool. Those carry only a cut prefix (PanelEntry::name_cut) and cannot
     * be acted on; the caller warns the user once per listing. */
    int names_cut() const { return namesCut; }

    /* Whether the name arena may live in extended/expanded memory, and which
     * kind to prefer (0 auto, 1 XMS, 2 EMS). Mirrors the entry store's own
     * /EXMEM and /NOEXMEM handling; call before the first refresh(). */
    void set_name_memory(int allowExt, int pref)
    { nameExtAllow = allowExt; nameExtPref = pref; }

    /* Where the names of the last listing ended up ("XMS"/"EMS"/
     * "conventional"), and how much of the arena they used. Diagnostics only. */
    const char *names_kind() const { return nameStore.kind(); }

private:
    /* FTP servers are case-sensitive, so a name that carries case must be
     * shown exactly as the server spelled it. An ALL-CAPS 8.3 name carries
     * none - it is the classic DOS spelling, and leaving it uppercase next to
     * the local pane's lowercase made the two sides of the same directory
     * look different. So the Norton convention applies to those, and only
     * those: anything with a lowercase letter, or any long name, stays
     * verbatim. Defined in rpanel.cpp. */
    int nc_case(const char *name) const;

    FtpClient *ftp;
    /* Full remote path (via PWD), NOT truncated to the header display width -
     * deeply nested trees with long directory names easily exceed
     * PANEL_HEADER_MAX (80). Sized like FTP_LINE_MAX since that is the largest
     * a PWD reply (and thus the quoted path) can be. */
    char cwd[FTP_LINE_MAX];
    int  navFailed;             /* 1 = the last action reported an error       */
    int  curYear;               /* current year (for date lines with a time)  */

    /* Arena of full (untruncated) names for the current listing; entries refer
     * to it by handle (PanelEntry::nameref), which is why it can live in
     * XMS/EMS. Allocated lazily on the first refresh and reused (reset) after
     * that; handles stay valid across sort_entries(), which only moves
     * records. Panel::names points here. */
    NameStore nameStore;
    int       nameExtAllow;     /* 0 = conventional memory only (/NOEXMEM)    */
    int       nameExtPref;      /* 0 auto, 1 XMS, 2 EMS                       */
    int       namesCut;         /* entries whose name did not fit the arena   */

    /* LIST callback (ftpcli calls this for every raw line). */
    static void on_line(void *ctx, const char *line);
    void add_line(const char *line);
};

#endif /* RPANEL_H */
