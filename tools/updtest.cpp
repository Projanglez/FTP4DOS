/* =============================================================================
 * updtest.cpp - standalone DOS harness for src/update.cpp
 * -----------------------------------------------------------------------------
 * Drives the verify-first chain without the TUI, so a signature bug cannot hide
 * behind a dialog.
 *
 *   updtest check <current-version>    fetch, verify, parse, compare
 *   updtest get   <current-version>    the above, then download and verify
 *
 * Point it at a test channel with FTP4DOS_UPDHOST / FTP4DOS_UPDPORT in
 * MTCP.CFG. There is deliberately no way to switch the signature check off -
 * a test build gets test keys compiled in instead.
 *
 * Run it from a WRITABLE drive: the staged file lands next to the executable.
 * ===========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/update.h"
#include "../src/sha256.h"
#include "../src/ftpcli.h"

static int progress(void *ctx, unsigned long sofar, unsigned long total)
{
    static unsigned long last = 0;
    (void)ctx;
    if (sofar < last) last = 0;           /* a new phase started */
    if (sofar - last >= 16384UL || sofar == total) {
        last = sofar;
        if (total) printf("\r  %lu / %lu (%lu%%)   ", sofar, total,
                          (sofar * 100UL) / total);
        else       printf("\r  %lu bytes   ", sofar);
        fflush(stdout);
    }
    return 0;
}

static const char *rcName(int rc)
{
    switch (rc) {
    case UPD_OK:         return "OK";
    case UPD_UPTODATE:   return "UPTODATE";
    case UPD_ERR_NET:    return "NET";
    case UPD_ERR_SIG:    return "SIG";
    case UPD_ERR_PARSE:  return "PARSE";
    case UPD_ERR_HASH:   return "HASH";
    case UPD_ERR_SPACE:  return "SPACE";
    case UPD_ERR_IO:     return "IO";
    case UPD_ERR_ABORT:  return "ABORT";
    default:             return "?";
    }
}

static void showVersionCmp(void)
{
    /* The letter-suffix tags (v0.9.4a) are the ones a naive comparison gets
     * wrong, so they are checked explicitly. */
    struct { const char *a, *b; int want; } t[] = {
        { "1.0.2", "1.0.1",  1 }, { "1.0.1", "1.0.2", -1 },
        { "1.0.1", "1.0.1",  0 }, { "1.1.0", "1.0.9",  1 },
        { "2.0.0", "1.9.9",  1 }, { "0.9.4a", "0.9.4", 1 },
        { "0.9.4", "0.9.4a", -1 }, { "0.9.5", "0.9.4a", 1 },
        { "1.0.10", "1.0.9", 1 }, { "v1.0.2", "1.0.1",  1 }
    };
    int i, bad = 0;
    printf("version_cmp:");
    for (i = 0; i < (int)(sizeof(t) / sizeof(t[0])); i++) {
        int got = upd_version_cmp(t[i].a, t[i].b);
        int norm = (got > 0) ? 1 : ((got < 0) ? -1 : 0);
        if (norm != t[i].want) {
            printf("\n  FAIL %s vs %s -> %d, want %d", t[i].a, t[i].b, norm, t[i].want);
            bad++;
        }
    }
    printf(bad ? "\n" : " all ok\n");
}

int main(int argc, char **argv)
{
    struct UpdInfo ui;
    char hex[SHA256_HEX_LEN];
    int rc;

    if (argc < 3) {
        fprintf(stderr, "usage: updtest check|get <current-version>\n");
        return 2;
    }

    showVersionCmp();

    upd_init(argv[0]);
    printf("exe path: %s\n", upd_exe_path());

    if (FtpClient::init_stack() != FTP_OK) {
        fprintf(stderr, "cannot start the mTCP stack (is MTCPCFG set?)\n");
        return 3;
    }

    rc = upd_check(argv[2], &ui);
    printf("check: rc=%s\n", rcName(rc));
    if (rc == UPD_ERR_SIG || rc == UPD_ERR_NET || rc == UPD_ERR_PARSE) {
        printf("  error: %s\n", upd_last_error());
        FtpClient::shutdown_stack();
        return 1;
    }

    sha256_hex(ui.sha256, hex);
    printf("  version=%s date=%s newer=%d\n", ui.version, ui.date, ui.newer);
    printf("  file=%s size=%lu\n", ui.file, ui.size);
    printf("  sha256=%s\n", hex);
    printf("  notes=%s\n", ui.notes);

    if (strcmp(argv[1], "get") == 0 && rc == UPD_OK) {
        rc = upd_download(&ui, progress, 0);
        printf("\ndownload: rc=%s\n", rcName(rc));
        if (rc != UPD_OK) {
            printf("  error: %s\n", upd_last_error());
        } else {
            rc = upd_verify(&ui, progress, 0);
            printf("\nverify: rc=%s\n", rcName(rc));
            if (rc != UPD_OK) printf("  error: %s\n", upd_last_error());
            else              printf("  staged, pending=%d\n", upd_pending());
        }
    }

    FtpClient::shutdown_stack();
    return (rc == UPD_OK || rc == UPD_UPTODATE) ? 0 : 1;
}
