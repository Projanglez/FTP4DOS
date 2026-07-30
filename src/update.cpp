/*
   FTP4DOS - update.cpp
   Signed auto-update: fetch, verify, stage, swap.

   The security-relevant part of this file is the ORDER of operations in
   upd_check(): the manifest is fetched, its signature is checked against the
   public keys compiled into this binary, and only then is a single byte of it
   parsed or believed. Everything downstream - the version comparison, the file
   name, the expected hash - comes from a document that has already been proven
   to be ours.

   If the signature fails, that is the end of it. There is deliberately no
   override switch and no "install anyway" path: on a plain-HTTP channel that
   escape hatch is exactly what an attacker would aim for.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dos.h>
#include <direct.h>

#include "update.h"
#include "httpget.h"
#include "sha256.h"
#include "rsaverify.h"
#include "rsakeys.h"
#include "lfn.h"
#include "i18n.h"

/* mTCP, for reading the optional host override out of MTCP.CFG. */
#include "types.h"
#include "utils.h"

/* After all system/mTCP headers: short umlaut macros for German text. */
#include "umlaut.h"


/* Default update channel. A Cloudflare Worker that reads the repository's
 * latest GitHub release and hands the assets out over plain HTTP - the one
 * route that does not redirect to a TLS endpoint the DOS client cannot follow.
 * See tools/worker/README.md for why every GitHub-hosted alternative failed. */
#define UPD_HOST_DEFAULT "ftp4dos-update.ftp4dos-update.workers.dev"
#define UPD_PORT_DEFAULT 80
#define UPD_PATH_INF     "/UPDATE.INF"
#define UPD_PATH_SIG     "/UPDATE.SIG"

#define MANIFEST_MAX      1024   /* a manifest is ~200 bytes                */
#define SIG_LEN            256   /* RSA-2048, exactly                       */
#define SPACE_SLACK      65536UL /* free space required beyond the download */

static char g_exePath[160] = "";
static char g_newPath[160] = "";
static char g_bakPath[160] = "";
static char g_err[160]     = "";
static int  g_pending      = 0;

static char g_host[HTTP_HOST_MAX] = UPD_HOST_DEFAULT;
static unsigned g_port            = UPD_PORT_DEFAULT;
static int  g_cfgRead             = 0;

const char *upd_last_error(void) { return g_err; }
const char *upd_exe_path(void)   { return g_exePath; }
int         upd_pending(void)    { return g_pending; }

static void setErr(const char *msg)
{
    strncpy(g_err, msg, sizeof(g_err) - 1);
    g_err[sizeof(g_err) - 1] = '\0';
}


/* --- Paths --------------------------------------------------------------- */

void upd_init(const char *argv0)
{
    char drive[_MAX_DRIVE], dir[_MAX_DIR];

    /* Same scheme as connsave_init()/sites_init(): everything lives beside the
     * executable, so a machine with several copies keeps them independent. */
    if (argv0 && argv0[0]) {
        _splitpath(argv0, drive, dir, 0, 0);
        if (drive[0] || dir[0]) {
            _makepath(g_exePath, drive, dir, "FTP4DOS", "EXE");
            _makepath(g_newPath, drive, dir, "FTP4DOS", "NEW");
            _makepath(g_bakPath, drive, dir, "FTP4DOS", "BAK");
            return;
        }
    }
    /* argv[0] carried no path (very old DOS): fall back to the current dir. */
    if (getcwd(g_exePath, (int)sizeof(g_exePath) - 16) == 0) {
        strcpy(g_exePath, ".");
    }
    strcpy(g_newPath, g_exePath);
    strcpy(g_bakPath, g_exePath);
    {
        int n = (int)strlen(g_exePath);
        const char *sep = (n > 0 && (g_exePath[n - 1] == '\\' ||
                                     g_exePath[n - 1] == '/')) ? "" : "\\";
        strcat(g_exePath, sep); strcat(g_exePath, "FTP4DOS.EXE");
        strcat(g_newPath, sep); strcat(g_newPath, "FTP4DOS.NEW");
        strcat(g_bakPath, sep); strcat(g_bakPath, "FTP4DOS.BAK");
    }
}

/* Optional per-machine override, so the channel can be pointed at a local test
 * server without rebuilding. Read lazily and once: init_stack() has long since
 * closed the config file by the time an update check runs.
 *
 * Note there is deliberately no key that disables the signature check. Test
 * builds get test keys compiled in instead - a runtime switch for that would be
 * a hole shipped to every user for the convenience of one. */
static void readCfgOnce(void)
{
    char tmp[16];

    if (g_cfgRead) return;
    g_cfgRead = 1;

    /* openCfgFile() returns a FILE*, not a status: NULL means it could not be
     * opened. (ftpcli.cpp ignores the result entirely, which is why the
     * inverted test here went unnoticed until the override silently did
     * nothing.) */
    if (Utils::openCfgFile() == 0) return;
    if (Utils::getAppValue((char *)"FTP4DOS_UPDHOST", g_host, sizeof(g_host)) != 0)
        strcpy(g_host, UPD_HOST_DEFAULT);
    if (Utils::getAppValue((char *)"FTP4DOS_UPDPORT", tmp, sizeof(tmp)) == 0) {
        int p = atoi(tmp);
        if (p > 0 && p < 65536) g_port = (unsigned)p;
    }
    Utils::closeCfgFile();
}


/* --- Version comparison -------------------------------------------------- */

/* "1.0.2" or "0.9.4a" -> three numbers plus a letter suffix. The letter matters:
 * the project has shipped v0.9.4a and v0.9.5a, and treating those as equal to
 * their base version would silently skip an update. */
static void parseVersion(const char *s, int *maj, int *min, int *pat, int *suf)
{
    *maj = *min = *pat = 0;
    *suf = 0;
    if (!s) return;

    while (*s == 'v' || *s == 'V') s++;
    *maj = atoi(s);
    while (isdigit((unsigned char)*s)) s++;
    if (*s == '.') { s++; *min = atoi(s); while (isdigit((unsigned char)*s)) s++; }
    if (*s == '.') { s++; *pat = atoi(s); while (isdigit((unsigned char)*s)) s++; }
    if (isalpha((unsigned char)*s)) *suf = tolower((unsigned char)*s) - 'a' + 1;
}

int upd_version_cmp(const char *a, const char *b)
{
    int am, an, ap, as, bm, bn, bp, bs;

    parseVersion(a, &am, &an, &ap, &as);
    parseVersion(b, &bm, &bn, &bp, &bs);

    if (am != bm) return (am < bm) ? -1 : 1;
    if (an != bn) return (an < bn) ? -1 : 1;
    if (ap != bp) return (ap < bp) ? -1 : 1;
    if (as != bs) return (as < bs) ? -1 : 1;
    return 0;
}


/* --- Manifest parsing ---------------------------------------------------- */

/* Only ever called on bytes whose signature has already verified. Unknown keys
 * are ignored so future fields do not break older clients. */
static int parseManifest(const char *text, int len, struct UpdInfo *out)
{
    const char *p = text;
    const char *end = text + len;
    int haveVer = 0, haveSize = 0, haveHash = 0, haveFile = 0;

    memset(out, 0, sizeof(*out));

    while (p < end) {
        const char *eol = p;
        const char *eq;
        char key[24];
        char val[UPD_FILE_MAX];
        int klen, vlen;

        while (eol < end && *eol != '\n' && *eol != '\r') eol++;

        if (p < eol && *p != '#') {
            eq = p;
            while (eq < eol && *eq != '=') eq++;
            if (eq < eol) {
                klen = (int)(eq - p);
                vlen = (int)(eol - eq - 1);
                if (klen > 0 && klen < (int)sizeof(key) &&
                    vlen >= 0 && vlen < (int)sizeof(val)) {
                    memcpy(key, p, klen);      key[klen] = '\0';
                    memcpy(val, eq + 1, vlen); val[vlen] = '\0';

                    if (stricmp(key, "version") == 0) {
                        strncpy(out->version, val, UPD_VER_MAX - 1);
                        haveVer = 1;
                    } else if (stricmp(key, "date") == 0) {
                        strncpy(out->date, val, UPD_DATE_MAX - 1);
                    } else if (stricmp(key, "notes") == 0) {
                        strncpy(out->notes, val, UPD_NOTES_MAX - 1);
                    } else if (stricmp(key, "file") == 0) {
                        strncpy(out->file, val, UPD_FILE_MAX - 1);
                        haveFile = 1;
                    } else if (stricmp(key, "size") == 0) {
                        out->size = strtoul(val, 0, 10);
                        haveSize = 1;
                    } else if (stricmp(key, "sha256") == 0) {
                        if (sha256_parse_hex(val, out->sha256) == 0) haveHash = 1;
                    }
                }
            }
        }

        p = eol;
        while (p < end && (*p == '\n' || *p == '\r')) p++;
    }

    if (!haveVer || !haveSize || !haveHash || !haveFile) return -1;
    if (out->size == 0) return -1;
    return 0;
}


/* --- The check ----------------------------------------------------------- */

int upd_check(const char *curVersion, struct UpdInfo *out,
              UpdProgressCb cb, void *ctx)
{
    unsigned char inf[MANIFEST_MAX];
    unsigned char sig[SIG_LEN + 4];
    unsigned char hash[SHA256_DIGEST_LEN];
    int inflen = 0, siglen = 0;
    int rc;

    g_err[0] = '\0';
    readCfgOnce();

    /* 1. The manifest. */
    rc = http_get_mem(g_host, g_port, UPD_PATH_INF, inf, (int)sizeof(inf), &inflen);
    if (rc != HTTP_OK) {
        setErr(http_last_error());
        return UPD_ERR_NET;
    }

    /* 2. Its detached signature. Exactly 256 bytes or it is not one. */
    rc = http_get_mem(g_host, g_port, UPD_PATH_SIG, sig, (int)sizeof(sig), &siglen);
    if (rc != HTTP_OK) {
        setErr(http_last_error());
        return UPD_ERR_NET;
    }
    if (siglen != SIG_LEN) {
        setErr(L("Signature has the wrong size", "Signatur hat die falsche Gr" oe ss "e"));
        return UPD_ERR_SIG;
    }

    /* 3. Hash exactly the bytes that arrived - no reformatting, no line-ending
     *    normalisation. That is why the signature is a separate file. */
    sha256_buf(inf, (unsigned int)inflen, hash);

    /* 4. Verify against both keys. The reserve exists because a shipped binary
     *    can never be taught a new one.
     *
     *    This is the slow step - about 3.5 s per key on a 25 MHz 386 - so it
     *    reports progress and can be cancelled. A cancelled check returns -1
     *    and must not be mistaken for a bad signature. */
    rc = rsa_verify_sha256_cb(sig, SIG_LEN, hash, rsa_key_primary,
                              (RsaProgressCb)cb, ctx);
    if (rc == 0)
        rc = rsa_verify_sha256_cb(sig, SIG_LEN, hash, rsa_key_reserve,
                                  (RsaProgressCb)cb, ctx);
    if (rc < 0) {
        setErr(L("Update check cancelled", "Update-Pr" ue "fung abgebrochen"));
        return UPD_ERR_ABORT;
    }
    if (rc == 0) {
        setErr(L("Signature invalid - update refused",
                 "Signatur ung" ue "ltig - Update abgelehnt"));
        return UPD_ERR_SIG;
    }

    /* 5. Only now is the content trustworthy enough to look at. */
    if (parseManifest((const char *)inf, inflen, out) != 0) {
        setErr(L("Manifest is incomplete", "Manifest ist unvollst" ae "ndig"));
        return UPD_ERR_PARSE;
    }

    out->newer = (upd_version_cmp(out->version, curVersion) > 0) ? 1 : 0;
    return out->newer ? UPD_OK : UPD_UPTODATE;
}


/* --- The download -------------------------------------------------------- */

static int enoughSpace(unsigned long need)
{
    struct _diskfree_t df;
    unsigned drive = 0;   /* 0 = current */
    unsigned long freeBytes;

    if (g_exePath[0] && g_exePath[1] == ':')
        drive = (unsigned)(toupper((unsigned char)g_exePath[0]) - 'A' + 1);

    if (_dos_getdiskfree(drive, &df) != 0) return 1;   /* unknown: let it try */

    freeBytes = (unsigned long)df.avail_clusters *
                (unsigned long)df.sectors_per_cluster *
                (unsigned long)df.bytes_per_sector;
    return (freeBytes >= need) ? 1 : 0;
}

int upd_download(const struct UpdInfo *ui, UpdProgressCb cb, void *ctx)
{
    int rc;

    g_err[0] = '\0';
    g_pending = 0;
    readCfgOnce();

    if (!enoughSpace(ui->size + SPACE_SLACK)) {
        setErr(L("Not enough free disk space", "Zu wenig freier Speicherplatz"));
        return UPD_ERR_SPACE;
    }

    lfn_remove(g_newPath);   /* a leftover from an aborted attempt */

    rc = http_get_file(g_host, g_port, ui->file, g_newPath,
                       (HttpProgressCb)cb, ctx);
    if (rc != HTTP_OK) {
        lfn_remove(g_newPath);
        setErr(http_last_error());
        return (rc == HTTP_ERR_ABORT)   ? UPD_ERR_ABORT :
               (rc == HTTP_ERR_LOCALIO) ? UPD_ERR_IO    : UPD_ERR_NET;
    }
    return UPD_OK;
}

int upd_verify(const struct UpdInfo *ui, UpdProgressCb cb, void *ctx)
{
    unsigned char hash[SHA256_DIGEST_LEN];

    g_err[0] = '\0';
    g_pending = 0;

    /* The signed manifest said how big it is and what it hashes to. Both must
     * hold, or this is not the file that was signed for. */
    {
        FILE *f = lfn_fopen(g_newPath, "rb");
        unsigned long got = 0;
        if (f) {
            if (fseek(f, 0L, SEEK_END) == 0) {
                long sz = ftell(f);
                if (sz > 0L) got = (unsigned long)sz;
            }
            fclose(f);
        }
        if (got != ui->size) {
            lfn_remove(g_newPath);
            setErr(L("Downloaded file has the wrong size",
                     "Heruntergeladene Datei hat die falsche Gr" oe ss "e"));
            return UPD_ERR_HASH;
        }
    }

    if (sha256_file(g_newPath, hash, (Sha256ProgressCb)cb, ctx) != 0) {
        lfn_remove(g_newPath);
        setErr(L("Could not verify the download", "Download konnte nicht gepr" ue "ft werden"));
        return UPD_ERR_IO;
    }

    if (memcmp(hash, ui->sha256, SHA256_DIGEST_LEN) != 0) {
        lfn_remove(g_newPath);
        setErr(L("Checksum mismatch - update refused",
                 "Pr" ue "fsumme stimmt nicht - Update abgelehnt"));
        return UPD_ERR_HASH;
    }

    g_pending = 1;
    return UPD_OK;
}


void upd_discard(void)
{
    if (g_newPath[0]) lfn_remove(g_newPath);
    g_pending = 0;
}

const char *upd_keep(void)
{
    /* Only the pending swap is cancelled; the verified file stays where it is
     * so it can be renamed over FTP4DOS.EXE by hand. */
    g_pending = 0;
    return g_newPath;
}


/* --- The swap ------------------------------------------------------------ */

/* Called at exit, after FtpClient::shutdown_stack() and tui_shutdown().
 *
 * Renaming the running executable is safe on DOS: the image is fully loaded
 * into memory and no handle to the file remains open. Both steps are FAT
 * directory-entry updates.
 *
 * The order is chosen so that a failure always leaves a working executable
 * behind: if the second rename fails, the first is undone. */
int upd_commit(char *msgbuf, int msglen)
{
    if (!g_pending) {
        if (msgbuf && msglen > 0) msgbuf[0] = '\0';
        return UPD_OK;
    }

    lfn_remove(g_bakPath);   /* previous backup; failure is fine */

    if (lfn_rename(g_exePath, g_bakPath) != 0) {
        if (msgbuf)
            _snprintf(msgbuf, msglen,
                      L("Update NOT installed - could not rename %s.\nThe new version is waiting as FTP4DOS.NEW.",
                        "Update NICHT installiert - %s konnte nicht umbenannt werden.\nDie neue Version liegt als FTP4DOS.NEW bereit."),
                      g_exePath);
        return UPD_ERR_IO;
    }

    if (lfn_rename(g_newPath, g_exePath) != 0) {
        /* Put the old executable back rather than leaving none at all. */
        lfn_rename(g_bakPath, g_exePath);
        if (msgbuf)
            _snprintf(msgbuf, msglen,
                      L("Update NOT installed - the old version was restored.",
                        "Update NICHT installiert - die alte Version wurde wiederhergestellt."));
        return UPD_ERR_IO;
    }

    g_pending = 0;
    if (msgbuf)
        _snprintf(msgbuf, msglen,
                  L("Update installed. The previous version is FTP4DOS.BAK.",
                    "Update installiert. Die bisherige Version liegt als FTP4DOS.BAK."));
    return UPD_OK;
}
