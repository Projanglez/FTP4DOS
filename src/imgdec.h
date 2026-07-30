/* =============================================================================
 * imgdec.h - Streaming image decoder interface
 * -----------------------------------------------------------------------------
 * Every format decoder implements this. Rows are pulled one at a time and each
 * carries its own destination index: that is what lets bottom-up BMP,
 * interlaced GIF and Adam7 PNG work without ever holding a full-size image.
 * Nothing larger than a single source row is allocated by a decoder.
 *
 * Same abstract-class shape as ExtMem (extmem.h) and EntryStore (entrystore.h).
 *
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#ifndef IMGDEC_H
#define IMGDEC_H

#include <stdio.h>

/* Widest source image we will decode. A truecolour row of this width is the
 * largest single allocation a decoder makes (4096*3 = 12 KB). */
#define IMG_MAX_WIDTH   4096

/* Error codes, returned by open() and next_row(). */
#define IMG_OK           0
#define IMG_ERR_FORMAT  -1    /* not this format, or malformed              */
#define IMG_ERR_UNSUP   -2    /* valid but uses a feature we do not decode  */
#define IMG_ERR_MEM     -3    /* out of memory                              */
#define IMG_ERR_IO      -4    /* read error / truncated file                */
#define IMG_ERR_SIZE    -5    /* dimensions out of range                    */

struct ImgInfo {
    int width, height;
    int truecolor;              /* 0 = 8-bit palette indices, 1 = 24-bit RGB */
    unsigned char pal[768];     /* 256 RGB triples, 0..255 (palette images)  */
};

class ImgDecoder {
public:
    virtual ~ImgDecoder() {}

    /* Parse the header. The file is positioned at the start. Returns IMG_OK
     * and fills 'info', or a negative IMG_ERR_*. */
    virtual int open(FILE *f, ImgInfo *info) = 0;

    /* Produce one row. 'dst' holds width bytes (palette) or width*3 bytes
     * (truecolour, R,G,B order) and must be at least that large. '*row'
     * receives the destination row index, 0 = top.
     * Returns 1 when a row was produced, 0 when the image is complete,
     * or a negative IMG_ERR_*. */
    virtual int next_row(unsigned char far *dst, int *row) = 0;

    /* Release whatever the decoder allocated. Safe to call after a failure. */
    virtual void close() = 0;
};

/* Formats the viewer knows how to probe for. */
#define IMG_FMT_NONE  0
#define IMG_FMT_BMP   1
#define IMG_FMT_PCX   2
#define IMG_FMT_GIF   3
#define IMG_FMT_PNG   4

/* Each decoder module hands out its own instance; the viewer owns it and calls
 * close() plus delete. Implemented in the matching img_*.cpp. */
ImgDecoder *img_bmp_create(void);
ImgDecoder *img_pcx_create(void);
ImgDecoder *img_gif_create(void);
ImgDecoder *img_png_create(void);

#endif /* IMGDEC_H */
