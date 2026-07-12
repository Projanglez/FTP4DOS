/* =============================================================================
 * dialog.h - Modal dialogs for NCFTP386
 * -----------------------------------------------------------------------------
 * Every dialog draws itself centered over the current screen, runs its own
 * keyboard loop, and restores the screen when it closes - the caller does
 * NOT need to redraw.
 *
 * Multi-line messages: separate lines with '\n' (max. DLG_MAX_LINES).
 * ===========================================================================*/
#ifndef DIALOG_H
#define DIALOG_H

#define DLG_MAX_LINES 14    /* max. text lines in message/confirm dialogs */

/* Notice/error box with one [ OK ] button. is_error!=0 -> red styling.
 * Closes on Enter, Esc, or Space. */
void dlg_message(const char *title, const char *msg, int is_error);
void dlg_error(const char *title, const char *msg);   /* = dlg_message(...,1)  */

/* Yes/No prompt. Returns: 1 = Yes, 0 = No.
 * Y/N as direct keys, Left/Right/Tab switches focus, Enter selects, Esc = No.
 * Default focus is on "Yes". */
int dlg_confirm(const char *title, const char *msg);

/* Single-line input field (basis for the connect dialog).
 * buf is shown pre-filled and edited; maxlen = max. characters (buf must be
 * maxlen+1 in size). is_password!=0 shows '*'. Returns: 1 = OK, 0 = Cancel. */
int dlg_input(const char *title, const char *prompt,
              char *buf, int maxlen, int is_password);

/* Snapshot of a transfer's state, passed to dlg_progress_update().
 * Speeds are bytes/sec (0 = unknown); ETA fields are seconds (-1 = unknown).
 * When batch != 0, the batch (all-files) lines are drawn as well. */
struct ProgressInfo {
    const char   *filename;
    unsigned long file_sofar, file_total;   /* file_total 0 => size unknown    */
    unsigned long cur_speed, avg_speed;     /* bytes/sec, 0 = unknown          */
    long          file_eta_sec;             /* -1 = unknown                    */
    int           batch;                    /* 1 => draw the batch total lines */
    int           files_done, files_total;
    unsigned long batch_sofar, batch_total; /* batch_total 0 => unknown total  */
    long          batch_eta_sec;            /* -1 = unknown                    */
    int           paused;                   /* 1 => show *** PAUSED ***        */
};

/* Non-modal progress dialog for data transfers (F5).
 * Flow: begin() opens the box (saves the screen), update() is called from
 * the FtpProgressCb during the blocking transfer, end() closes the box and
 * restores the screen. begin(batch!=0) reserves space for the all-files lines.
 * update() only redraws on a visible change, for efficiency. */
void dlg_progress_begin(const char *title, int batch);
void dlg_progress_update(const ProgressInfo *pi);
void dlg_progress_end(void);

/* Change the displayed file name (and the n/N counter) while a progress
 * dialog is running, for batch/recursive copy operations. Resets the per-file
 * bar to 0%. No effect if no progress dialog is open. */
void dlg_progress_setfile(const char *name, int idx, int count);

/* Vertical selection menu (modal). Shows 'count' entries, 'initial' is
 * highlighted at first. Arrows/Home/End move, Enter selects, Esc cancels;
 * a letter key jumps straight to the first entry starting with that
 * character and selects it. Returns: selected index 0..count-1, or -1. */
int dlg_menu(const char *title, const char *const *items, int count, int initial);

/* Choice dialog: multi-line message 'msg' on top, below it a vertical list
 * of 'count' options. Arrows move, Enter selects, Esc cancels.
 * Returns: selected option index 0..count-1, or -1 on Esc. */
int dlg_choice(const char *title, const char *msg,
               const char *const *items, int count);

/* FTP connect form in a single dialog:
 * Host/Port/User/Pass fields + two "save" checkboxes.
 * Tab/Down/Up navigate between fields; Space/Enter toggles checkboxes.
 * *save_conn and *save_pass are used in/out (pre-filled from NCFTP.SAV).
 * Returns: 1 = Connect, 0 = Cancel. On 0, *save_conn/*save_pass are left
 * unchanged. */
int dlg_connect(const char *title,
                char *host, int host_max,
                char *port, int port_max,
                char *user, int user_max,
                char *pass, int pass_max,
                char *startdir, int startdir_max,
                int *save_conn,
                int *save_pass);

/* Show CRC32 + MD5 of a file and offer to save them to a file.
 * crc / md5 are the formatted hex strings (display only). fnamebuf (size
 * maxlen+1) is the editable target filename, pre-filled with a default; on
 * return 1 it holds the name to save to. Esc -> 0 (don't save). */
int dlg_checksum(const char *title, const char *crc, const char *md5,
                 char *fnamebuf, int maxlen);

/* Splash screen at program start: centered box with version info.
 * Disappears after 10 seconds or on a key press. */
void dlg_splash(const char *version);

#endif /* DIALOG_H */
