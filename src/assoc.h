/* =============================================================================
 * assoc.h - File associations: run an external program on the selected file
 * -----------------------------------------------------------------------------
 * F3/Enter hands a file to an external DOS program when its extension is listed
 * in FTP4DOS.EXT (next to the EXE). This is how image formats are viewed: point
 * jpg/png/gif/bmp/pcx at a DOS image viewer. Anything without an association
 * keeps going to the built-in text viewer.
 *
 * File format (text, editable), one association per line:
 *     # FTP4DOS file associations
 *     jpg=C:\TOOLS\PICTVIEW\PICTVIEW.EXE %1
 *     zip=C:\UTIL\PKUNZIP.EXE -v %1
 *
 * %1 is replaced by the full path of the file; without it the path is appended.
 * The file is parsed on demand rather than held in memory: DGROUP has only a
 * few KB to spare, and it means FTP4DOS.EXT can be edited while we run.
 *
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#ifndef ASSOC_H
#define ASSOC_H

#define ASSOC_CMD_MAX  128   /* longest command line an association may hold */
#define ASSOC_EXT_MAX   16   /* longest extension we bother matching         */

/* Call once at startup: locates FTP4DOS.EXT next to the executable. */
void assoc_init(const char *argv0);

/* Look up the command for 'ext' (without the dot, case-insensitive).
 * Returns 1 and fills cmd on a match, 0 if there is no association. */
int assoc_lookup(const char *ext, char *cmd, int cmdsz);

/* Result of assoc_run(). */
#define ASSOC_OK        0
#define ASSOC_ENOMEM   -1    /* not enough free conventional memory          */
#define ASSOC_ENOENT   -2    /* the program could not be found               */
#define ASSOC_EFAIL    -3    /* spawn failed for some other reason           */

/* Run 'cmd' with 'filepath' substituted for %1. Tears the TUI down around the
 * child and brings it back up with video_pref (the /MONO,/COLOR preference);
 * the caller still has to repaint, exactly as after a dialog. */
int assoc_run(const char *cmd, const char *filepath, int video_pref);

/* Largest free conventional memory block, in KB. This is what an external
 * program has to fit into while FTP4DOS stays resident, so it is reported on
 * the help screen and whenever a spawn fails. */
unsigned assoc_free_kb(void);

/* 1 if 'ext' is a binary image format. Used to offer a helpful hint instead of
 * dumping a JPEG into the text viewer when nothing is associated with it. */
int assoc_is_image_ext(const char *ext);

/* Extension of a file name (after the last dot), "" when there is none.
 * Returns a pointer into 'name'. */
const char *assoc_ext_of(const char *name);

/* DOS 8.3 extension for a temporary file, uppercased and cut to three
 * characters, mapping the common four-letter cases (jpeg->JPG, tiff->TIF,
 * html->HTM). 'out' must hold at least 4 bytes. */
void assoc_dos_ext3(const char *ext, char *out);

#endif /* ASSOC_H */
