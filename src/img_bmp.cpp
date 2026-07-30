/* =============================================================================
 * img_bmp.cpp - Windows BMP decoder
 * -----------------------------------------------------------------------------
 * Handles 1/4/8/24/32 bpp BI_RGB, plus BI_RLE8 and BI_RLE4, in both the normal
 * bottom-up layout and the rarer top-down one (negative height).
 *
 * Bottom-up files need no buffering: the row index reported by next_row() is
 * simply flipped, which is exactly what that index exists for.
 *
 * The RLE forms are decoded one row at a time rather than expanded whole. A
 * run-length stream is not randomly addressable, but it is sequential - rows
 * are separated by end-of-line codes and deltas only ever move forward - so a
 * single row buffer is enough. That matters: a 720x400 screenshot would need
 * 288000 bytes if expanded in full, which is more than the far heap can spare
 * next to a 64000-byte framebuffer.
 *
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgdec.h"

#define BI_RGB   0L
#define BI_RLE8  1L
#define BI_RLE4  2L

class BmpDecoder : public ImgDecoder {
public:
    BmpDecoder();
    virtual int  open(FILE *f, ImgInfo *info);
    virtual int  next_row(unsigned char far *dst, int *row);
    virtual void close();
private:
    int  rleRow(unsigned char far *dst);
    FILE *m_f;
    long  m_dataOfs;
    long  m_compress;
    int   m_w, m_h;
    int   m_bpp;
    int   m_topdown;
    int   m_srcPitch;         /* padded bytes per source row (non-RLE) */
    int   m_y;                /* next source row, 0 = first in file order */
    unsigned char far *m_raw; /* one padded source row (non-RLE) */

    /* RLE state, carried between rows */
    int   m_isRle;
    int   m_rleBits;          /* 4 or 8 */
    int   m_skipRows;         /* blank rows still owed by a delta */
    int   m_startX;           /* starting column for the next row (delta) */
    int   m_rleEof;           /* stream ended; remaining rows are background */
};

BmpDecoder::BmpDecoder()
    : m_f(0), m_dataOfs(0), m_compress(0), m_w(0), m_h(0), m_bpp(0),
      m_topdown(0), m_srcPitch(0), m_y(0), m_raw(0),
      m_isRle(0), m_rleBits(0), m_skipRows(0), m_startX(0), m_rleEof(0) {}

/* --- little-endian readers ------------------------------------------------ */
static int rd16(FILE *f, unsigned *v)
{
    int a = fgetc(f), b = fgetc(f);
    if (b == EOF) return -1;
    *v = (unsigned)a | ((unsigned)b << 8);
    return 0;
}
static int rd32(FILE *f, unsigned long *v)
{
    int a = fgetc(f), b = fgetc(f), c = fgetc(f), d = fgetc(f);
    if (d == EOF) return -1;
    *v = (unsigned long)a | ((unsigned long)b << 8) |
         ((unsigned long)c << 16) | ((unsigned long)d << 24);
    return 0;
}

int BmpDecoder::open(FILE *f, ImgInfo *info)
{
    unsigned      u16;
    unsigned long u32, hdrSize, ncolors;
    long          w, h;
    int           i;

    m_f = f;

    if (rd16(f, &u16) != 0 || u16 != 0x4D42) return IMG_ERR_FORMAT;  /* "BM" */
    if (rd32(f, &u32) != 0) return IMG_ERR_IO;      /* file size, ignored    */
    if (rd32(f, &u32) != 0) return IMG_ERR_IO;      /* reserved              */
    if (rd32(f, &u32) != 0) return IMG_ERR_IO;
    m_dataOfs = (long)u32;

    if (rd32(f, &hdrSize) != 0) return IMG_ERR_IO;

    if (hdrSize == 12) {
        /* BITMAPCOREHEADER: 16-bit dimensions, no compression field. */
        if (rd16(f, &u16) != 0) return IMG_ERR_IO;
        w = (long)u16;
        if (rd16(f, &u16) != 0) return IMG_ERR_IO;
        h = (long)u16;
        if (rd16(f, &u16) != 0) return IMG_ERR_IO;   /* planes */
        if (rd16(f, &u16) != 0) return IMG_ERR_IO;
        m_bpp = (int)u16;
        m_compress = BI_RGB;
        ncolors = (m_bpp <= 8) ? (1UL << m_bpp) : 0;
    } else if (hdrSize >= 40) {
        if (rd32(f, &u32) != 0) return IMG_ERR_IO;
        w = (long)u32;
        if (rd32(f, &u32) != 0) return IMG_ERR_IO;
        h = (long)u32;
        if (rd16(f, &u16) != 0) return IMG_ERR_IO;   /* planes */
        if (rd16(f, &u16) != 0) return IMG_ERR_IO;
        m_bpp = (int)u16;
        if (rd32(f, &u32) != 0) return IMG_ERR_IO;
        m_compress = (long)u32;
        if (rd32(f, &u32) != 0) return IMG_ERR_IO;   /* image size   */
        if (rd32(f, &u32) != 0) return IMG_ERR_IO;   /* x ppm        */
        if (rd32(f, &u32) != 0) return IMG_ERR_IO;   /* y ppm        */
        if (rd32(f, &u32) != 0) return IMG_ERR_IO;
        ncolors = u32;
        if (rd32(f, &u32) != 0) return IMG_ERR_IO;   /* important    */
        if (ncolors == 0 && m_bpp <= 8) ncolors = 1UL << m_bpp;
        /* Skip any extra header bytes (V4/V5 headers). */
        if (hdrSize > 40) fseek(f, (long)hdrSize - 40, SEEK_CUR);
    } else {
        return IMG_ERR_FORMAT;
    }

    /* Height < 0 means the rows are stored top-down. */
    if (h < 0) { m_topdown = 1; h = -h; }
    if (w <= 0 || h <= 0 || w > IMG_MAX_WIDTH || h > 20000) return IMG_ERR_SIZE;
    m_w = (int)w;
    m_h = (int)h;

    if (m_bpp != 1 && m_bpp != 4 && m_bpp != 8 && m_bpp != 24 && m_bpp != 32)
        return IMG_ERR_UNSUP;

    if (m_compress == BI_RGB) {
        m_isRle = 0;
    } else if (m_compress == BI_RLE8 && m_bpp == 8) {
        m_isRle = 1; m_rleBits = 8;
    } else if (m_compress == BI_RLE4 && m_bpp == 4) {
        m_isRle = 1; m_rleBits = 4;
    } else {
        return IMG_ERR_UNSUP;
    }

    /* Palette: BGR(A) quads, or BGR triples in the old core header. */
    memset(info->pal, 0, 768);
    if (m_bpp <= 8) {
        int entSize = (hdrSize == 12) ? 3 : 4;
        if (ncolors > 256) ncolors = 256;
        for (i = 0; i < (int)ncolors; i++) {
            int b = fgetc(f), g = fgetc(f), r = fgetc(f);
            if (entSize == 4) fgetc(f);
            if (r == EOF) return IMG_ERR_IO;
            info->pal[i * 3 + 0] = (unsigned char)r;
            info->pal[i * 3 + 1] = (unsigned char)g;
            info->pal[i * 3 + 2] = (unsigned char)b;
        }
    }

    info->width     = m_w;
    info->height    = m_h;
    info->truecolor = (m_bpp >= 24) ? 1 : 0;

    if (!m_isRle) {
        /* Source rows are padded to a multiple of 4 bytes. */
        m_srcPitch = (int)((((long)m_w * m_bpp + 31) / 32) * 4);
        m_raw = (unsigned char far *)malloc((unsigned)m_srcPitch);
        if (!m_raw) return IMG_ERR_MEM;
    }

    if (fseek(f, m_dataOfs, SEEK_SET) != 0) return IMG_ERR_IO;

    m_y = 0;
    return IMG_OK;
}

/* Decode one row of a BI_RLE4 / BI_RLE8 stream into dst (already cleared).
 * Returns 0 normally; the stream's own end is recorded in m_rleEof so the
 * remaining rows come out as background rather than as an error. */
int BmpDecoder::rleRow(unsigned char far *dst)
{
    int x = m_startX;
    int i, v;

    m_startX = 0;

    for (;;) {
        int n = fgetc(m_f);
        int c;
        if (n == EOF) { m_rleEof = 1; return 0; }
        c = fgetc(m_f);
        if (c == EOF) { m_rleEof = 1; return 0; }

        if (n > 0) {
            /* Encoded run: n pixels. At 4 bpp the two nibbles of c alternate. */
            if (m_rleBits == 8) {
                while (n-- > 0) { if (x < m_w) dst[x] = (unsigned char)c; x++; }
            } else {
                for (i = 0; i < n; i++) {
                    int px = (i & 1) ? (c & 0x0F) : (c >> 4);
                    if (x < m_w) dst[x] = (unsigned char)px;
                    x++;
                }
            }
            continue;
        }

        switch (c) {
        case 0:                       /* end of line */
            return 0;
        case 1:                       /* end of bitmap */
            m_rleEof = 1;
            return 0;
        case 2: {                     /* delta */
            int dx = fgetc(m_f), dy = fgetc(m_f);
            if (dy == EOF) { m_rleEof = 1; return 0; }
            if (dy == 0) { x += dx; continue; }
            /* Moving down ends this row; the rows in between stay background. */
            m_skipRows = dy - 1;
            m_startX   = x + dx;
            return 0;
        }
        default: {                    /* absolute run of c literal pixels */
            if (m_rleBits == 8) {
                for (i = 0; i < c; i++) {
                    v = fgetc(m_f);
                    if (v == EOF) { m_rleEof = 1; return 0; }
                    if (x < m_w) dst[x] = (unsigned char)v;
                    x++;
                }
                if (c & 1) fgetc(m_f);           /* pad to a word boundary */
            } else {
                int nb = (c + 1) / 2;            /* bytes holding c nibbles */
                v = 0;
                for (i = 0; i < c; i++) {
                    int px;
                    if ((i & 1) == 0) {
                        v = fgetc(m_f);
                        if (v == EOF) { m_rleEof = 1; return 0; }
                    }
                    px = (i & 1) ? (v & 0x0F) : (v >> 4);
                    if (x < m_w) dst[x] = (unsigned char)px;
                    x++;
                }
                if (nb & 1) fgetc(m_f);          /* pad to a word boundary */
            }
            continue;
        }
        }
    }
}

int BmpDecoder::next_row(unsigned char far *dst, int *row)
{
    int x;

    if (m_y >= m_h) return 0;

    /* Stored bottom-up unless the header said otherwise. */
    *row = m_topdown ? m_y : (m_h - 1 - m_y);

    if (m_isRle) {
        memset(dst, 0, (unsigned)m_w);
        if (m_skipRows > 0)      m_skipRows--;    /* blank row owed by a delta */
        else if (!m_rleEof)      rleRow(dst);
        m_y++;
        return 1;
    }

    if (fread(m_raw, 1, (unsigned)m_srcPitch, m_f) != (unsigned)m_srcPitch)
        return IMG_ERR_IO;

    switch (m_bpp) {
    case 32:
        /* BGRA; the fourth byte is padding in BI_RGB, alpha only in the V4/V5
         * headers, and treating it as opaque is what every viewer does. */
        for (x = 0; x < m_w; x++) {
            dst[x * 3 + 0] = m_raw[x * 4 + 2];
            dst[x * 3 + 1] = m_raw[x * 4 + 1];
            dst[x * 3 + 2] = m_raw[x * 4 + 0];
        }
        break;
    case 24:
        /* Stored BGR, wanted RGB. */
        for (x = 0; x < m_w; x++) {
            dst[x * 3 + 0] = m_raw[x * 3 + 2];
            dst[x * 3 + 1] = m_raw[x * 3 + 1];
            dst[x * 3 + 2] = m_raw[x * 3 + 0];
        }
        break;
    case 8:
        memcpy(dst, m_raw, (unsigned)m_w);
        break;
    case 4:
        for (x = 0; x < m_w; x++)
            dst[x] = (x & 1) ? (unsigned char)(m_raw[x >> 1] & 0x0F)
                             : (unsigned char)(m_raw[x >> 1] >> 4);
        break;
    case 1:
        for (x = 0; x < m_w; x++)
            dst[x] = (unsigned char)((m_raw[x >> 3] >> (7 - (x & 7))) & 1);
        break;
    }

    m_y++;
    return 1;
}

void BmpDecoder::close()
{
    if (m_raw) { free(m_raw); m_raw = 0; }
}

ImgDecoder *img_bmp_create(void) { return new BmpDecoder(); }
