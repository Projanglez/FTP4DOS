/* =============================================================================
 * img_pcx.cpp - ZSoft PCX decoder
 * -----------------------------------------------------------------------------
 * Handles the common cases: 8 bpp single plane with the 256-colour VGA palette
 * appended to the file, and 1/4 bpp multi-plane EGA images using the 16-colour
 * palette in the header. Rows are stored top-down and in order, so this is the
 * simplest of the decoders.
 *
 * PCX stores each plane separately within a row (plane 0 for the whole row,
 * then plane 1, ...), so multi-plane rows are recombined bit by bit.
 *
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgdec.h"

class PcxDecoder : public ImgDecoder {
public:
    PcxDecoder();
    virtual int  open(FILE *f, ImgInfo *info);
    virtual int  next_row(unsigned char far *dst, int *row);
    virtual void close();
private:
    int  readRle(unsigned char far *out, int len);
    FILE *m_f;
    int   m_w, m_h;
    int   m_bpp;              /* bits per pixel per plane */
    int   m_planes;
    int   m_bpl;              /* bytes per line per plane */
    int   m_y;
    unsigned char far *m_raw; /* bpl * planes */
    int   m_pend;             /* pending RLE run: count */
    int   m_pval;             /* pending RLE run: value */
};

PcxDecoder::PcxDecoder()
    : m_f(0), m_w(0), m_h(0), m_bpp(0), m_planes(0), m_bpl(0), m_y(0),
      m_raw(0), m_pend(0), m_pval(0) {}

int PcxDecoder::open(FILE *f, ImgInfo *info)
{
    unsigned char hdr[128];
    int  xmin, ymin, xmax, ymax;
    int  i;

    m_f = f;
    if (fread(hdr, 1, 128, f) != 128) return IMG_ERR_IO;
    if (hdr[0] != 0x0A) return IMG_ERR_FORMAT;
    if (hdr[2] != 1)    return IMG_ERR_UNSUP;    /* only RLE encoding exists */

    m_bpp    = hdr[3];
    xmin     = hdr[4]  | (hdr[5]  << 8);
    ymin     = hdr[6]  | (hdr[7]  << 8);
    xmax     = hdr[8]  | (hdr[9]  << 8);
    ymax     = hdr[10] | (hdr[11] << 8);
    m_planes = hdr[65];
    m_bpl    = hdr[66] | (hdr[67] << 8);

    m_w = xmax - xmin + 1;
    m_h = ymax - ymin + 1;
    if (m_w <= 0 || m_h <= 0 || m_w > IMG_MAX_WIDTH || m_h > 20000)
        return IMG_ERR_SIZE;
    if (m_bpl <= 0 || m_planes <= 0 || m_planes > 4) return IMG_ERR_UNSUP;
    if (m_bpp != 1 && m_bpp != 2 && m_bpp != 4 && m_bpp != 8)
        return IMG_ERR_UNSUP;
    /* 24-bit PCX is 8 bits across 3 planes; anything else multi-plane at
     * 8 bpp is not something we can make sense of. */
    if (m_bpp == 8 && m_planes != 1 && m_planes != 3) return IMG_ERR_UNSUP;

    memset(info->pal, 0, 768);

    if (m_bpp == 8 && m_planes == 1) {
        /* The 256-colour palette sits in the last 769 bytes, introduced by a
         * 0x0C marker. Older files without it fall back to the EGA header
         * palette, which is why failure here is not fatal. */
        long here = ftell(f);
        if (fseek(f, -769L, SEEK_END) == 0) {
            int marker = fgetc(f);
            if (marker == 0x0C) {
                for (i = 0; i < 256; i++) {
                    int r = fgetc(f), g = fgetc(f), b = fgetc(f);
                    if (b == EOF) break;
                    info->pal[i * 3 + 0] = (unsigned char)r;
                    info->pal[i * 3 + 1] = (unsigned char)g;
                    info->pal[i * 3 + 2] = (unsigned char)b;
                }
            } else {
                for (i = 0; i < 16; i++) {
                    info->pal[i * 3 + 0] = hdr[16 + i * 3 + 0];
                    info->pal[i * 3 + 1] = hdr[16 + i * 3 + 1];
                    info->pal[i * 3 + 2] = hdr[16 + i * 3 + 2];
                }
            }
        }
        fseek(f, here, SEEK_SET);
    } else if (m_bpp != 8) {
        for (i = 0; i < 16; i++) {
            info->pal[i * 3 + 0] = hdr[16 + i * 3 + 0];
            info->pal[i * 3 + 1] = hdr[16 + i * 3 + 1];
            info->pal[i * 3 + 2] = hdr[16 + i * 3 + 2];
        }
    }

    m_raw = (unsigned char far *)malloc((unsigned)m_bpl * m_planes + 8);
    if (!m_raw) return IMG_ERR_MEM;

    info->width     = m_w;
    info->height    = m_h;
    info->truecolor = (m_bpp == 8 && m_planes == 3) ? 1 : 0;

    m_y = 0;
    return IMG_OK;
}

/* PCX RLE: a byte with the top two bits set is a count for the next byte.
 * Runs may cross the end of a row, so the remainder is carried over. */
int PcxDecoder::readRle(unsigned char far *out, int len)
{
    int n = 0;

    while (n < len) {
        if (m_pend > 0) {
            out[n++] = (unsigned char)m_pval;
            m_pend--;
            continue;
        }
        {
            int b = fgetc(m_f);
            if (b == EOF) return -1;
            if ((b & 0xC0) == 0xC0) {
                int v = fgetc(m_f);
                if (v == EOF) return -1;
                m_pend = b & 0x3F;
                m_pval = v;
                if (m_pend == 0) continue;
            } else {
                out[n++] = (unsigned char)b;
            }
        }
    }
    return 0;
}

int PcxDecoder::next_row(unsigned char far *dst, int *row)
{
    int x, p;

    if (m_y >= m_h) return 0;
    *row = m_y;

    if (readRle(m_raw, m_bpl * m_planes) != 0) return IMG_ERR_IO;

    if (m_bpp == 8 && m_planes == 1) {
        memcpy(dst, m_raw, (unsigned)m_w);
    } else if (m_bpp == 8 && m_planes == 3) {
        for (x = 0; x < m_w; x++) {
            dst[x * 3 + 0] = m_raw[x];
            dst[x * 3 + 1] = m_raw[m_bpl + x];
            dst[x * 3 + 2] = m_raw[m_bpl * 2 + x];
        }
    } else {
        /* Sub-byte pixels spread across planes: bit b of plane p contributes
         * bit p of the palette index. */
        int ppb = 8 / m_bpp;                 /* pixels per byte in one plane */
        int mask = (1 << m_bpp) - 1;
        for (x = 0; x < m_w; x++) dst[x] = 0;
        for (p = 0; p < m_planes; p++) {
            const unsigned char far *src = m_raw + (long)m_bpl * p;
            for (x = 0; x < m_w; x++) {
                int shift = (ppb - 1 - (x % ppb)) * m_bpp;
                int v = (src[x / ppb] >> shift) & mask;
                dst[x] = (unsigned char)(dst[x] | (v << (p * m_bpp)));
            }
        }
    }

    m_y++;
    return 1;
}

void PcxDecoder::close()
{
    if (m_raw) { free(m_raw); m_raw = 0; }
}

ImgDecoder *img_pcx_create(void) { return new PcxDecoder(); }
