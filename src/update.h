/* =============================================================================
 * update.h - signed auto-update for FTP4DOS
 * -----------------------------------------------------------------------------
 * Fetches a manifest over plain HTTP, verifies its RSA-2048 signature, and only
 * then believes a word of it. The executable named by the (verified) manifest is
 * downloaded, checked against the SHA-256 the manifest states, and staged next
 * to the running program as FTP4DOS.NEW. The swap itself happens at exit, after
 * the mTCP stack and the TUI are gone - see upd_commit().
 *
 * Why the transport is unencrypted: mTCP has no TLS, and GitHub cannot be
 * reached over plain HTTP (Pages enforces HTTPS on its default domain, release
 * assets need signed URLs). A Cloudflare Worker bridges that gap - see
 * tools/worker/. Nothing in that path is trusted: authenticity comes from the
 * signature alone, so a hostile Worker, a hostile CDN or a hostile network can
 * deny service but cannot install anything.
 *
 * Order matters and is enforced in upd_check(): fetch, verify, THEN parse. A
 * manifest whose signature does not check out is never interpreted.
 * ===========================================================================*/
#ifndef UPDATE_H
#define UPDATE_H

#define UPD_OK             0
#define UPD_UPTODATE       1   /* reachable, but nothing newer available     */
#define UPD_ERR_NET       -1   /* DNS/connect/transfer failure               */
#define UPD_ERR_SIG       -2   /* signature did not verify - hard stop       */
#define UPD_ERR_PARSE     -3   /* manifest malformed or missing fields       */
#define UPD_ERR_HASH      -4   /* download did not match the signed hash     */
#define UPD_ERR_SPACE     -5   /* not enough free disk space                 */
#define UPD_ERR_IO        -6   /* local file error                           */
#define UPD_ERR_ABORT     -7   /* user cancelled                             */

#define UPD_VER_MAX     16
#define UPD_DATE_MAX    12
#define UPD_NOTES_MAX   96
#define UPD_FILE_MAX   128

struct UpdInfo {
    char          version[UPD_VER_MAX];
    char          date[UPD_DATE_MAX];
    char          notes[UPD_NOTES_MAX];
    char          file[UPD_FILE_MAX];    /* path on the update host      */
    unsigned long size;
    unsigned char sha256[32];
    int           newer;                 /* 1 = newer than the running build */
};

/* Same shape as FtpProgressCb, so ncftp.cpp's copy_progress() fits directly. */
typedef int (*UpdProgressCb)(void *ctx, unsigned long sofar, unsigned long total);

/* Derive FTP4DOS.EXE/.NEW/.BAK next to the running executable. Call once from
 * main() with argv[0], like connsave_init()/sites_init(). */
void upd_init(const char *argv0);

/* Fetch manifest + signature, verify, parse, compare against 'curVersion'.
 * Returns UPD_OK (out->newer says whether it is worth offering), UPD_UPTODATE,
 * or an error. Never returns UPD_OK on an unverified manifest. */
int upd_check(const char *curVersion, struct UpdInfo *out,
              UpdProgressCb cb, void *ctx);

/* Download the executable to FTP4DOS.NEW. Leaves nothing behind on failure.
 *
 * Split from upd_verify() only so the caller can label the two phases: hashing
 * 270 KB takes seconds on a 386, and a progress bar that silently switches
 * from "downloading" to "verifying" reads as a stall. */
int upd_download(const struct UpdInfo *ui, UpdProgressCb cb, void *ctx);

/* Check the staged file's size and SHA-256 against the (already signature-
 * verified) manifest. Deletes it and fails if either differs. Only after this
 * returns UPD_OK is upd_pending() true. */
int upd_verify(const struct UpdInfo *ui, UpdProgressCb cb, void *ctx);

/* Is a verified FTP4DOS.NEW staged and ready to be swapped in? */
int upd_pending(void);

/* Throw the staged file away again (user chose not to install after all). */
void upd_discard(void);

/* Cancel the pending swap but LEAVE the downloaded file in place, so the user
 * can install it by hand later. Returns its full path for the message. */
const char *upd_keep(void);

/* Perform the swap. MUST be called only after the mTCP stack is down and the
 * TUI is restored: it renames the running executable. Writes a human-readable
 * result into msgbuf. Returns UPD_OK or an error. */
int upd_commit(char *msgbuf, int msglen);

/* Full path of the executable, for execv() on restart. */
const char *upd_exe_path(void);

/* Detail for the last failure (already localised). */
const char *upd_last_error(void);

/* Compare two "M.m.p" version strings, with an optional trailing letter as
 * used by the v0.9.4a / v0.9.5a tags. <0, 0, >0 like strcmp. */
int upd_version_cmp(const char *a, const char *b);

#endif /* UPDATE_H */
