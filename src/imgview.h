/* =============================================================================
 * imgview.h - Full-screen image viewer (F3 on an image file)
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#ifndef IMGVIEW_H
#define IMGVIEW_H

#include "imgdec.h"   /* IMG_FMT_* - the vocabulary img_probe_file speaks */

/* Sniff the first bytes of 'path' and report an IMG_FMT_* value, or
 * IMG_FMT_NONE when it is not an image we can decode. Content-based, so it is
 * right even when the extension lies. */
int img_probe_file(const char *path);

/* 1 when 'ext' names a format the built-in decoders handle. Extension-based
 * and therefore only a guess, but it lets the remote path decide whether a
 * download is worth starting before spending the transfer. */
int img_ext_known(const char *ext);

/* 1 when 'ext' names a binary image format at all, decodable or not. Used to
 * warn before dumping a JPEG into the text viewer. */
int img_is_image_ext(const char *ext);

/* Extension of a file name (after the last dot), "" when there is none.
 * Returns a pointer into 'name'. */
const char *img_ext_of(const char *name);

/* Largest free conventional memory block, in KB. A picture needs roughly
 * 100 KB of it, so this is what decides whether one can be shown at all -
 * which is why it is reported on the help screen and in the out-of-memory
 * dialog. Idle heap blocks are handed back to DOS before measuring, because
 * Watcom's far heap keeps everything free() gives it. */
unsigned img_free_kb(void);

/* Decode and display 'path'. The image is decoded in text mode (so failures
 * are ordinary dialogs and a slow decode can show progress), then the display
 * switches to graphics, shows it, and waits for a key.
 *
 * video_pref is the /MONO,/COLOR preference: the TUI is re-initialised with it
 * on the way out, so the caller repaints afterwards exactly as after a dialog.
 * Returns 0 when the image was shown, non-zero when it was not (a message has
 * already been given to the user). */
int img_view(const char *path, const char *title, int video_pref);

#endif /* IMGVIEW_H */
