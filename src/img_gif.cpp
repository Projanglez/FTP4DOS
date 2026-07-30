/* =============================================================================
 * img_gif.cpp - GIF87a/89a decoder (first frame)
 * -----------------------------------------------------------------------------
 * Decodes the first image in the file, which is what a viewer wants; later
 * frames of an animation are ignored. Handles the global and local colour
 * tables and interlaced images - the latter purely through the row index
 * next_row() reports, so no full-size buffer is needed.
 *
 * LZW here is the GIF variant: codes are packed LSB-first and the code width
 * grows from the initial size as the table fills. The dictionary is three
 * parallel arrays on the far heap (4096 entries) rather than a linked
 * structure, and strings are emitted through a stack because the dictionary
 * chains backwards.
 *
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgdec.h"

#define LZW_MAX_CODES 4096

class GifDecoder : public ImgDecoder {
public:
    GifDecoder();
    virtual int  open(FILE *f, ImgInfo *info);
    virtual int  next_row(unsigned char far *dst, int *row);
    virtual void close();
private:
    int  nextByte(void);          /* next byte of the sub-block stream */
    int  nextCode(void);          /* next LZW code, -1 at end */
    int  decodeRow(unsigned char far *dst);

    FILE *m_f;
    int   m_w, m_h;
    int   m_interlace;
    int   m_y;                    /* rows emitted so far */
    int   m_pass, m_passRow;      /* interlace state */

    /* sub-block reader */
    int   m_blockLeft;
    int   m_eod;

    /* bit reader */
    unsigned long m_bitBuf;
    int   m_bitCnt;

    /* LZW state */
    int   m_minCode, m_codeSize, m_clear, m_end, m_next, m_prev;
    unsigned short far *m_pfx;
    unsigned char  far *m_sfx;
    unsigned char  far *m_stack;
    int   m_sp;
    int   m_firstByte;
};

GifDecoder::GifDecoder()
    : m_f(0), m_w(0), m_h(0), m_interlace(0), m_y(0), m_pass(0), m_passRow(0),
      m_blockLeft(0), m_eod(0), m_bitBuf(0), m_bitCnt(0),
      m_minCode(0), m_codeSize(0), m_clear(0), m_end(0), m_next(0), m_prev(-1),
      m_pfx(0), m_sfx(0), m_stack(0), m_sp(0), m_firstByte(0) {}

/* --- GIF sub-block stream ------------------------------------------------- */
int GifDecoder::nextByte(void)
{
    if (m_eod) return -1;
    if (m_blockLeft == 0) {
        int n = fgetc(m_f);
        if (n <= 0) { m_eod = 1; return -1; }   /* 0 = block terminator */
        m_blockLeft = n;
    }
    m_blockLeft--;
    return fgetc(m_f);
}

int GifDecoder::nextCode(void)
{
    while (m_bitCnt < m_codeSize) {
        int b = nextByte();
        if (b < 0) return -1;
        m_bitBuf |= (unsigned long)(unsigned char)b << m_bitCnt;
        m_bitCnt += 8;
    }
    {
        int code = (int)(m_bitBuf & ((1UL << m_codeSize) - 1));
        m_bitBuf >>= m_codeSize;
        m_bitCnt -= m_codeSize;
        return code;
    }
}

int GifDecoder::open(FILE *f, ImgInfo *info)
{
    unsigned char hdr[13];
    int  gctFlag, gctSize;
    int  i;

    m_f = f;
    if (fread(hdr, 1, 13, f) != 13) return IMG_ERR_IO;
    if (memcmp(hdr, "GIF87a", 6) != 0 && memcmp(hdr, "GIF89a", 6) != 0)
        return IMG_ERR_FORMAT;

    gctFlag = (hdr[10] & 0x80) ? 1 : 0;
    gctSize = 2 << (hdr[10] & 7);

    memset(info->pal, 0, 768);
    if (gctFlag) {
        for (i = 0; i < gctSize; i++) {
            int r = fgetc(f), g = fgetc(f), b = fgetc(f);
            if (b == EOF) return IMG_ERR_IO;
            if (i < 256) {
                info->pal[i * 3 + 0] = (unsigned char)r;
                info->pal[i * 3 + 1] = (unsigned char)g;
                info->pal[i * 3 + 2] = (unsigned char)b;
            }
        }
    }

    /* Walk the block stream to the first image descriptor, skipping the
     * extension blocks (graphic control, comment, application, ...). */
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) return IMG_ERR_FORMAT;
        if (c == 0x3B) return IMG_ERR_FORMAT;        /* trailer, no image */
        if (c == 0x21) {                             /* extension         */
            fgetc(f);                                /* label             */
            for (;;) {
                int n = fgetc(f);
                if (n <= 0) break;
                fseek(f, (long)n, SEEK_CUR);
            }
            continue;
        }
        if (c == 0x2C) break;                        /* image descriptor  */
        /* Anything else is a malformed stream. */
        return IMG_ERR_FORMAT;
    }

    {
        unsigned char d[9];
        int lctFlag, lctSize;
        if (fread(d, 1, 9, f) != 9) return IMG_ERR_IO;
        m_w = d[4] | (d[5] << 8);
        m_h = d[6] | (d[7] << 8);
        lctFlag     = (d[8] & 0x80) ? 1 : 0;
        m_interlace = (d[8] & 0x40) ? 1 : 0;
        lctSize     = 2 << (d[8] & 7);

        if (m_w <= 0 || m_h <= 0 || m_w > IMG_MAX_WIDTH || m_h > 20000)
            return IMG_ERR_SIZE;

        /* A local table overrides the global one for this image. */
        if (lctFlag) {
            memset(info->pal, 0, 768);
            for (i = 0; i < lctSize; i++) {
                int r = fgetc(f), g = fgetc(f), b = fgetc(f);
                if (b == EOF) return IMG_ERR_IO;
                if (i < 256) {
                    info->pal[i * 3 + 0] = (unsigned char)r;
                    info->pal[i * 3 + 1] = (unsigned char)g;
                    info->pal[i * 3 + 2] = (unsigned char)b;
                }
            }
        } else if (!gctFlag) {
            return IMG_ERR_FORMAT;                   /* no palette at all */
        }
    }

    m_minCode = fgetc(m_f);
    if (m_minCode < 2 || m_minCode > 8) return IMG_ERR_FORMAT;

    m_pfx   = (unsigned short far *)malloc(LZW_MAX_CODES * sizeof(short));
    m_sfx   = (unsigned char  far *)malloc(LZW_MAX_CODES);
    m_stack = (unsigned char  far *)malloc(LZW_MAX_CODES);
    if (!m_pfx || !m_sfx || !m_stack) return IMG_ERR_MEM;

    m_clear    = 1 << m_minCode;
    m_end      = m_clear + 1;
    m_next     = m_clear + 2;
    m_codeSize = m_minCode + 1;
    m_prev     = -1;
    m_sp       = 0;

    info->width     = m_w;
    info->height    = m_h;
    info->truecolor = 0;

    m_y = 0;
    m_pass = 0;
    m_passRow = 0;
    return IMG_OK;
}

/* Produce exactly m_w pixels. The LZW stream is a flat pixel sequence, so a
 * row boundary is just a count - runs freely cross it. */
int GifDecoder::decodeRow(unsigned char far *dst)
{
    int n = 0;

    while (n < m_w) {
        /* Anything already expanded on the stack comes out first. */
        while (m_sp > 0 && n < m_w) dst[n++] = m_stack[--m_sp];
        if (n >= m_w) break;

        {
            int code = nextCode();
            if (code < 0) {
                /* Truncated stream: pad the rest so the image still shows. */
                while (n < m_w) dst[n++] = 0;
                return 1;
            }
            if (code == m_clear) {
                m_next     = m_clear + 2;
                m_codeSize = m_minCode + 1;
                m_prev     = -1;
                continue;
            }
            if (code == m_end) {
                while (n < m_w) dst[n++] = 0;
                return 1;
            }

            {
                int cur = code;
                if (code >= m_next) {
                    /* KwKwK: the code is not in the table yet, so its string
                     * is the previous string plus its own first byte. */
                    if (m_prev < 0) { while (n < m_w) dst[n++] = 0; return 1; }
                    m_stack[m_sp++] = (unsigned char)m_firstByte;
                    cur = m_prev;
                }
                while (cur >= m_clear) {
                    if (m_sp >= LZW_MAX_CODES - 1) break;
                    m_stack[m_sp++] = m_sfx[cur];
                    cur = m_pfx[cur];
                }
                m_firstByte = cur;
                m_stack[m_sp++] = (unsigned char)cur;

                if (m_prev >= 0 && m_next < LZW_MAX_CODES) {
                    m_pfx[m_next] = (unsigned short)m_prev;
                    m_sfx[m_next] = (unsigned char)m_firstByte;
                    m_next++;
                    if (m_next == (1 << m_codeSize) && m_codeSize < 12)
                        m_codeSize++;
                }
                m_prev = code;
            }
        }
    }
    return 1;
}

int GifDecoder::next_row(unsigned char far *dst, int *row)
{
    /* Interlaced GIFs arrive in four passes; the row index is what puts them
     * back in the right place. */
    static const int startRow[4] = { 0, 4, 2, 1 };
    static const int stepRow[4]  = { 8, 8, 4, 2 };

    if (m_y >= m_h) return 0;

    if (m_interlace) {
        while (m_pass < 4 && m_passRow >= m_h) {
            m_pass++;
            if (m_pass < 4) m_passRow = startRow[m_pass];
        }
        if (m_pass >= 4) return 0;
        *row = m_passRow;
        m_passRow += stepRow[m_pass];
    } else {
        *row = m_y;
    }

    if (decodeRow(dst) != 1) return IMG_ERR_FORMAT;
    m_y++;
    return 1;
}

void GifDecoder::close()
{
    if (m_pfx)   { free(m_pfx);   m_pfx = 0; }
    if (m_sfx)   { free(m_sfx);   m_sfx = 0; }
    if (m_stack) { free(m_stack); m_stack = 0; }
}

ImgDecoder *img_gif_create(void) { return new GifDecoder(); }
