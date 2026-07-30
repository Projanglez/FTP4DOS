/* =============================================================================
 * assoc.cpp - File associations (FTP4DOS.EXT)
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 *
 * Memory note: FTP4DOS is linked with MAXALLOC=0xFFFF, so DOS hands it every
 * free byte at load time; the Watcom runtime gives the unused part back at
 * startup, which is the only reason a child process can be started at all.
 * _fheapshrink() before the spawn returns free heap blocks to DOS on top of
 * that - measurably more than was free right after startup.
 * ===========================================================================*/
#include <stdio.h>
#include <stdlib.h>    /* _splitpath, _makepath, getenv, _MAX_*             */
#include <string.h>    /* strncpy, strchr, strpbrk, stricmp                 */
#include <ctype.h>     /* toupper, isspace                                  */
#include <direct.h>    /* getcwd                                            */
#include <dos.h>       /* _dos_allocmem, _dos_freemem                       */
#include <errno.h>     /* ENOENT, ENOMEM                                    */
#include <malloc.h>    /* _heapshrink, _fheapshrink                         */
#include <process.h>   /* spawnvp, P_WAIT                                   */

#include "assoc.h"
#include "tui.h"

#define ASSOC_MAX_ARGS 16    /* program name + arguments + NULL terminator */

static char g_path[160] = "";   /* full path of FTP4DOS.EXT */


/* ------------------------------------------------------------------ */
/* Determine the path (same scheme as sites_init / connsave_init)      */
/* ------------------------------------------------------------------ */
void assoc_init(const char *argv0)
{
    char drive[_MAX_DRIVE], dir[_MAX_DIR];

    if (argv0 && argv0[0]) _splitpath(argv0, drive, dir, 0, 0);
    else                   { drive[0] = 0; dir[0] = 0; }

    if (drive[0] || dir[0]) {
        _makepath(g_path, drive, dir, "FTP4DOS", "EXT");
        return;
    }

    {
        char cwd[128];
        int  n;
        if (getcwd(cwd, sizeof(cwd)) == 0) strcpy(cwd, ".");
        n = (int)strlen(cwd);
        if (n > 0 && (cwd[n - 1] == '\\' || cwd[n - 1] == '/'))
            sprintf(g_path, "%sFTP4DOS.EXT", cwd);
        else
            sprintf(g_path, "%s\\FTP4DOS.EXT", cwd);
    }
}


/* ------------------------------------------------------------------ */
/* Extension helpers                                                    */
/* ------------------------------------------------------------------ */
const char *assoc_ext_of(const char *name)
{
    const char *dot = 0;
    const char *p;

    if (name == 0) return "";
    for (p = name; *p; p++)
        if (*p == '.') dot = p;
    /* A leading dot is part of the name ("...", ".profile"), not an extension. */
    if (dot == 0 || dot == name || dot[1] == '\0') return "";
    return dot + 1;
}

void assoc_dos_ext3(const char *ext, char *out)
{
    int i;

    out[0] = '\0';
    if (ext == 0 || ext[0] == '\0') return;

    /* Four-letter forms whose 3-char truncation would be misleading. */
    if      (stricmp(ext, "jpeg") == 0) { strcpy(out, "JPG"); return; }
    else if (stricmp(ext, "tiff") == 0) { strcpy(out, "TIF"); return; }
    else if (stricmp(ext, "html") == 0) { strcpy(out, "HTM"); return; }

    for (i = 0; i < 3 && ext[i]; i++)
        out[i] = (char)toupper((unsigned char)ext[i]);
    out[i] = '\0';
}

int assoc_is_image_ext(const char *ext)
{
    static const char *img[] = {
        "bmp", "jpg", "jpeg", "gif", "pcx", "png",
        "tif", "tiff", "tga", "lbm", "iff", 0
    };
    int i;

    if (ext == 0 || ext[0] == '\0') return 0;
    for (i = 0; img[i]; i++)
        if (stricmp(ext, img[i]) == 0) return 1;
    return 0;
}


/* ------------------------------------------------------------------ */
/* Lookup - parses the file on demand, keeps nothing resident           */
/* ------------------------------------------------------------------ */
int assoc_lookup(const char *ext, char *cmd, int cmdsz)
{
    FILE *f;
    char  line[256];
    int   found = 0;

    if (g_path[0] == '\0' || ext == 0 || ext[0] == '\0' || cmdsz <= 0) return 0;
    f = fopen(g_path, "r");
    if (!f) return 0;

    while (!found && fgets(line, sizeof(line), f)) {
        char *nl, *eq, *key, *val;

        nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        key = line;
        val = eq + 1;

        /* Tolerate surrounding blanks and a written-out leading dot. */
        while (*key && isspace((unsigned char)*key)) key++;
        if (*key == '.') key++;
        {
            int n = (int)strlen(key);
            while (n > 0 && isspace((unsigned char)key[n - 1])) key[--n] = '\0';
        }
        while (*val && isspace((unsigned char)*val)) val++;

        if (*key == '\0' || *val == '\0') continue;
        if (stricmp(key, ext) != 0) continue;

        strncpy(cmd, val, cmdsz - 1);
        cmd[cmdsz - 1] = '\0';
        found = 1;
    }

    fclose(f);
    return found;
}


/* ------------------------------------------------------------------ */
/* Free conventional memory                                             */
/* ------------------------------------------------------------------ */
unsigned assoc_free_kb(void)
{
    unsigned seg = 0;

    /* Hand idle heap blocks back to DOS first. Watcom's far heap keeps every
     * block it has ever taken from DOS - free() only returns it to the heap's
     * own free list - so without this the figure drops after each viewed file
     * and never recovers, reporting 0 KB while the memory is in fact still
     * available. Shrinking here also makes the number mean the same thing as
     * assoc_run(), which shrinks before it spawns. */
    _heapshrink();
    _fheapshrink();

    /* The oversized request is meant to fail: DOS then reports the largest
     * available block, in paragraphs, in 'seg'. */
    if (_dos_allocmem(0xFFFFu, &seg) != 0)
        return (unsigned)(((unsigned long)seg * 16UL) / 1024UL);

    _dos_freemem(seg);           /* a full megabyte was actually free */
    return 1024u;
}


/* ------------------------------------------------------------------ */
/* Build the child's argv from the association command                  */
/* -----------------------------------------------------------------------------
 * Splits on blanks, replaces the first %1 with filepath (appending it when the
 * command has no %1). 'store' backs the argv strings. Returns the argument
 * count, or -1 if the command does not fit.
 * ---------------------------------------------------------------------- */
static int build_argv(const char *cmd, const char *filepath,
                      char *store, int storesz,
                      char *argv[], int maxargs)
{
    const char *p = cmd;
    int  argc = 0;
    int  used = 0;
    int  have_file = 0;

    while (*p && argc < maxargs - 1) {
        char *dst;
        int   len = 0;

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;

        dst = store + used;
        while (*p && !isspace((unsigned char)*p)) {
            if (p[0] == '%' && p[1] == '1') {
                int fl = (int)strlen(filepath);
                if (used + len + fl + 1 > storesz) return -1;
                memcpy(dst + len, filepath, fl);
                len += fl;
                have_file = 1;
                p += 2;
                continue;
            }
            if (used + len + 2 > storesz) return -1;
            dst[len++] = *p++;
        }
        dst[len] = '\0';
        used += len + 1;
        argv[argc++] = dst;
    }

    /* No %1 anywhere: the file becomes the last argument. */
    if (!have_file && argc < maxargs - 1) {
        int fl = (int)strlen(filepath);
        if (used + fl + 1 > storesz) return -1;
        memcpy(store + used, filepath, fl + 1);
        argv[argc++] = store + used;
        used += fl + 1;
    }

    argv[argc] = 0;
    return argc;
}


/* ------------------------------------------------------------------ */
/* Run the association                                                  */
/* ------------------------------------------------------------------ */
int assoc_run(const char *cmd, const char *filepath, int video_pref)
{
    char  store[ASSOC_CMD_MAX + 160];
    char *argv[ASSOC_MAX_ARGS];
    int   argc, rc, err;

    if (cmd == 0 || cmd[0] == '\0' || filepath == 0) return ASSOC_EFAIL;

    argc = build_argv(cmd, filepath, store, (int)sizeof(store),
                      argv, ASSOC_MAX_ARGS);
    if (argc <= 0) return ASSOC_EFAIL;

    /* A .BAT target needs the command interpreter; spawn cannot run one. */
    {
        const char *ext = assoc_ext_of(argv[0]);
        if (stricmp(ext, "bat") == 0) {
            static char cmdline[ASSOC_CMD_MAX + 200];
            static char *shargv[4];
            const char *comspec = getenv("COMSPEC");
            int   i, n = 0;

            if (comspec == 0 || comspec[0] == '\0') comspec = "COMMAND.COM";
            for (i = 0; i < argc; i++) {
                int l = (int)strlen(argv[i]);
                if (n + l + 2 > (int)sizeof(cmdline)) break;
                if (n) cmdline[n++] = ' ';
                memcpy(cmdline + n, argv[i], l);
                n += l;
            }
            cmdline[n] = '\0';
            shargv[0] = (char *)comspec;
            shargv[1] = (char *)"/C";
            shargv[2] = cmdline;
            shargv[3] = 0;
            memcpy(argv, shargv, sizeof(shargv));
            argc = 3;
        }
    }

    /* Hand back every free heap block first - this is worth real kilobytes. */
    fflush(0);
    _heapshrink();
    _fheapshrink();

    tui_shutdown();
    errno = 0;
    rc = spawnvp(P_WAIT, argv[0], (const char * const *)argv);
    err = errno;
    tui_init(video_pref);

    if (rc == -1) {
        if (err == ENOENT) return ASSOC_ENOENT;
        if (err == ENOMEM) return ASSOC_ENOMEM;
        return ASSOC_EFAIL;
    }
    return ASSOC_OK;
}
