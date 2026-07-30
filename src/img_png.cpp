/* =============================================================================
 * img_png.cpp - PNG decoder
 * -----------------------------------------------------------------------------
 * Colour types 0 (grey), 2 (RGB), 3 (palette), 4 (grey+alpha) and 6 (RGBA);
 * bit depths 1/2/4/8/16. Alpha is composited against a mid grey rather than
 * ignored, so cut-outs do not come out with black fringes. All five filter
 * types are implemented.
 *
 * Interlaced (Adam7) images are rejected with IMG_ERR_UNSUP. The row index
 * that carries interlaced GIF does not help here: Adam7 subsamples columns as
 * well as rows, so a pass delivers partial rows and reassembly needs the whole
 * image resident. That costs w*h bytes, which defeats the point of streaming.
 * Interlaced PNGs are rare enough that a clear refusal beats the memory.
 *
 * The compressed data spans any number of IDAT chunks; inflate.cpp pulls
 * through a callback that hops from chunk to chunk, so the file is never held
 * in memory.
 *
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgdec.h"
#include "inflate.h"

class PngDecoder : public ImgDecoder {
public:
    PngDecoder();
    virtual int  open(FILE *f, ImgInfo *info);
    virtual int  next_row(unsigned char far *dst, int *row);
    virtual void close();

    /* Called by inflate when it needs more compressed bytes. */
    int  feed(unsigned char far *buf, int max);
private:
    int  nextIdat(void);
    void unfilter(unsigned char far *cur, const unsigned char far *prev,
                  int filter, int len, int bpp);
    void expand(const unsigned char far *src, unsigned char far *dst);

    FILE *m_f;
    int   m_w, m_h;
    int   m_depth, m_color;
    int   m_channels;
    int   m_bpp;              /* bytes per pixel, rounded up */
    int   m_rowbytes;
    int   m_y;
    int   m_truecolor;

    unsigned long m_idatLeft;  /* bytes remaining in the current IDAT */
    int   m_idatDone;
    int   m_zskip;             /* zlib header bytes still to discard */
    int   m_zcm;               /* zlib compression method byte */

    InflState *m_infl;
    unsigned char far *m_cur;
    unsigned char far *m_prev;
    unsigned char far *m_pal;   /* 768, palette images */
};

static int png_feed_cb(void *user, unsigned char far *buf, int max)
{
    return ((PngDecoder *)user)->feed(buf, max);
}

PngDecoder::PngDecoder()
    : m_f(0), m_w(0), m_h(0), m_depth(0), m_color(0), m_channels(0),
      m_bpp(0), m_rowbytes(0), m_y(0), m_truecolor(0),
      m_idatLeft(0), m_idatDone(0), m_zskip(2), m_zcm(-1), m_infl(0),
      m_cur(0), m_prev(0), m_pal(0) {}

static unsigned long rd32be(FILE *f)
{
    int a = fgetc(f), b = fgetc(f), c = fgetc(f), d = fgetc(f);
    if (d == EOF) return 0xFFFFFFFFUL;
    return ((unsigned long)a << 24) | ((unsigned long)b << 16) |
           ((unsigned long)c << 8)  | (unsigned long)d;
}

/* Advance to the next IDAT chunk's data. Returns 1 when positioned, 0 at the
 * end of the image data. */
int PngDecoder::nextIdat(void)
{
    char type[5];

    if (m_idatDone) return 0;
    for (;;) {
        unsigned long len = rd32be(m_f);
        if (len == 0xFFFFFFFFUL) { m_idatDone = 1; return 0; }
        if (fread(type, 1, 4, m_f) != 4) { m_idatDone = 1; return 0; }
        type[4] = '\0';
        if (memcmp(type, "IDAT", 4) == 0) {
            m_idatLeft = len;
            if (len == 0) continue;      /* empty IDAT is legal */
            return 1;
        }
        if (memcmp(type, "IEND", 4) == 0) { m_idatDone = 1; return 0; }
        fseek(m_f, (long)len + 4, SEEK_CUR);   /* skip data + CRC */
    }
}

int PngDecoder::feed(unsigned char far *buf, int max)
{
    int n = 0;

    /* The IDAT payload is a zlib stream, but inflate.cpp consumes raw DEFLATE.
     * The 2-byte zlib header therefore has to be taken off the COMPRESSED
     * side here - reading it through infl_read would ask the decompressor to
     * produce two output bytes, which is a different thing entirely. */
    while (m_zskip > 0) {
        unsigned char b;
        if (m_idatLeft == 0) {
            if (!nextIdat()) return 0;
            continue;
        }
        if (fread(&b, 1, 1, m_f) != 1) return 0;
        m_idatLeft--;
        if (m_zcm < 0) m_zcm = b;
        if (m_idatLeft == 0) fseek(m_f, 4L, SEEK_CUR);
        m_zskip--;
    }

    while (n < max) {
        if (m_idatLeft == 0) {
            if (n > 0) break;                 /* hand back what we have */
            if (!nextIdat()) break;
            continue;
        }
        {
            int want = max - n;
            size_t got;
            if ((unsigned long)want > m_idatLeft) want = (int)m_idatLeft;
            got = fread(buf + n, 1, (unsigned)want, m_f);
            if (got == 0) break;
            n += (int)got;
            m_idatLeft -= got;
            if (m_idatLeft == 0) fseek(m_f, 4L, SEEK_CUR);   /* chunk CRC */
        }
    }
    return n;
}

int PngDecoder::open(FILE *f, ImgInfo *info)
{
    unsigned char sig[8];
    int  interlace = 0;
    int  havePal = 0;

    m_f = f;
    if (fread(sig, 1, 8, f) != 8) return IMG_ERR_IO;
    if (sig[0] != 0x89 || sig[1] != 'P' || sig[2] != 'N' || sig[3] != 'G')
        return IMG_ERR_FORMAT;

    memset(info->pal, 0, 768);

    /* Walk chunks up to the first IDAT, which is where the header info ends. */
    for (;;) {
        unsigned long len;
        char type[5];
        long here;

        len = rd32be(f);
        if (len == 0xFFFFFFFFUL) return IMG_ERR_FORMAT;
        if (fread(type, 1, 4, f) != 4) return IMG_ERR_IO;
        type[4] = '\0';
        here = ftell(f);

        if (memcmp(type, "IHDR", 4) == 0) {
            unsigned long w = rd32be(f), h = rd32be(f);
            m_depth     = fgetc(f);
            m_color     = fgetc(f);
            fgetc(f);                       /* compression, always 0 */
            fgetc(f);                       /* filter, always 0      */
            interlace   = fgetc(f);
            if (w == 0 || h == 0 || w > IMG_MAX_WIDTH || h > 20000)
                return IMG_ERR_SIZE;
            m_w = (int)w;
            m_h = (int)h;
        } else if (memcmp(type, "PLTE", 4) == 0) {
            int n = (int)(len / 3), i;
            if (n > 256) n = 256;
            for (i = 0; i < n; i++) {
                info->pal[i * 3 + 0] = (unsigned char)fgetc(f);
                info->pal[i * 3 + 1] = (unsigned char)fgetc(f);
                info->pal[i * 3 + 2] = (unsigned char)fgetc(f);
            }
            havePal = 1;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            /* Rewind to the chunk header: feed() re-reads it. */
            fseek(f, here - 8, SEEK_SET);
            break;
        } else if (memcmp(type, "IEND", 4) == 0) {
            return IMG_ERR_FORMAT;
        }

        fseek(f, here + (long)len + 4, SEEK_SET);
    }

    if (interlace) return IMG_ERR_UNSUP;   /* Adam7: see note below */

    switch (m_color) {
    case 0: m_channels = 1; break;         /* grey            */
    case 2: m_channels = 3; break;         /* RGB             */
    case 3: m_channels = 1; break;         /* palette         */
    case 4: m_channels = 2; break;         /* grey + alpha    */
    case 6: m_channels = 4; break;         /* RGB + alpha     */
    default: return IMG_ERR_FORMAT;
    }
    if (m_depth != 1 && m_depth != 2 && m_depth != 4 &&
        m_depth != 8 && m_depth != 16) return IMG_ERR_FORMAT;
    if (m_color == 3 && !havePal) return IMG_ERR_FORMAT;
    if (m_color != 3 && m_depth < 8 && m_color != 0) return IMG_ERR_FORMAT;

    m_rowbytes = (int)(((long)m_w * m_channels * m_depth + 7) / 8);
    m_bpp      = (m_channels * m_depth + 7) / 8;
    if (m_bpp < 1) m_bpp = 1;

    /* Palette images stay indexed; everything else becomes RGB triples. */
    m_truecolor = (m_color == 3) ? 0 : 1;
    /* A greyscale image can stay indexed too, with a grey ramp palette -
     * cheaper and exact, instead of going through the colour cube. */
    if (m_color == 0 && m_depth <= 8) {
        int i, maxv = (1 << m_depth) - 1;
        m_truecolor = 0;
        for (i = 0; i <= maxv; i++) {
            int v = i * 255 / maxv;
            info->pal[i * 3 + 0] = (unsigned char)v;
            info->pal[i * 3 + 1] = (unsigned char)v;
            info->pal[i * 3 + 2] = (unsigned char)v;
        }
    }

    m_cur  = (unsigned char far *)malloc((unsigned)m_rowbytes + 8);
    m_prev = (unsigned char far *)malloc((unsigned)m_rowbytes + 8);
    if (!m_cur || !m_prev) return IMG_ERR_MEM;
    memset(m_prev, 0, (unsigned)m_rowbytes + 8);

    m_infl = infl_create(png_feed_cb, this);
    if (!m_infl) return IMG_ERR_MEM;

    /* The zlib header is dropped by feed() on the first pull; m_zcm records
     * its first byte so the compression method can still be checked. */

    info->width     = m_w;
    info->height    = m_h;
    info->truecolor = m_truecolor;

    m_y = 0;
    return IMG_OK;
}

static int paeth(int a, int b, int c)
{
    int p  = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

void PngDecoder::unfilter(unsigned char far *cur, const unsigned char far *prev,
                          int filter, int len, int bpp)
{
    int i;

    switch (filter) {
    case 0: break;                                  /* None    */
    case 1:                                         /* Sub     */
        for (i = bpp; i < len; i++) cur[i] = (unsigned char)(cur[i] + cur[i - bpp]);
        break;
    case 2:                                         /* Up      */
        for (i = 0; i < len; i++) cur[i] = (unsigned char)(cur[i] + prev[i]);
        break;
    case 3:                                         /* Average */
        for (i = 0; i < bpp && i < len; i++)
            cur[i] = (unsigned char)(cur[i] + (prev[i] >> 1));
        for (; i < len; i++)
            cur[i] = (unsigned char)(cur[i] + ((cur[i - bpp] + prev[i]) >> 1));
        break;
    case 4:                                         /* Paeth   */
        for (i = 0; i < bpp && i < len; i++)
            cur[i] = (unsigned char)(cur[i] + prev[i]);
        for (; i < len; i++)
            cur[i] = (unsigned char)(cur[i] +
                     paeth(cur[i - bpp], prev[i], prev[i - bpp]));
        break;
    default: break;
    }
}

/* Turn one unfiltered row into what the viewer wants. */
void PngDecoder::expand(const unsigned char far *src, unsigned char far *dst)
{
    int x;
    int step = (m_depth == 16) ? 2 : 1;   /* 16-bit samples: take the high byte */

    if (!m_truecolor) {
        if (m_depth == 8) {
            memcpy(dst, src, (unsigned)m_w);
        } else if (m_depth == 16) {
            for (x = 0; x < m_w; x++) dst[x] = src[x * 2];
        } else {
            int ppb  = 8 / m_depth;
            int mask = (1 << m_depth) - 1;
            for (x = 0; x < m_w; x++) {
                int shift = (ppb - 1 - (x % ppb)) * m_depth;
                dst[x] = (unsigned char)((src[x / ppb] >> shift) & mask);
            }
        }
        return;
    }

    /* Truecolour output: RGB triples, alpha composited on mid grey. */
    for (x = 0; x < m_w; x++) {
        const unsigned char far *p = src + (long)x * m_channels * step;
        int r, g, b, a = 255;

        switch (m_color) {
        case 0:  r = g = b = p[0]; break;
        case 2:  r = p[0]; g = p[step]; b = p[step * 2]; break;
        case 4:  r = g = b = p[0]; a = p[step]; break;
        default: r = p[0]; g = p[step]; b = p[step * 2]; a = p[step * 3]; break;
        }
        if (a != 255) {
            r = (r * a + 128 * (255 - a)) / 255;
            g = (g * a + 128 * (255 - a)) / 255;
            b = (b * a + 128 * (255 - a)) / 255;
        }
        dst[x * 3 + 0] = (unsigned char)r;
        dst[x * 3 + 1] = (unsigned char)g;
        dst[x * 3 + 2] = (unsigned char)b;
    }
}

int PngDecoder::next_row(unsigned char far *dst, int *row)
{
    unsigned char filt;
    int rc;

    if (m_y >= m_h) return 0;
    *row = m_y;

    rc = infl_read(m_infl, &filt, 1);
    if (rc != INFL_OK) return (rc < 0) ? IMG_ERR_FORMAT : 0;
    /* Now that the first pull has happened, the zlib header has been seen. */
    if (m_zcm >= 0 && (m_zcm & 0x0F) != 8) return IMG_ERR_FORMAT;
    rc = infl_read(m_infl, m_cur, (unsigned)m_rowbytes);
    if (rc < 0)  return IMG_ERR_FORMAT;
    if (rc == 1) return 0;                    /* stream ended early */

    unfilter(m_cur, m_prev, filt, m_rowbytes, m_bpp);
    expand(m_cur, dst);

    {
        unsigned char far *t = m_prev;
        m_prev = m_cur;
        m_cur  = t;
    }

    m_y++;
    return 1;
}

void PngDecoder::close()
{
    if (m_infl) { infl_destroy(m_infl); m_infl = 0; }
    if (m_cur)  { free(m_cur);  m_cur  = 0; }
    if (m_prev) { free(m_prev); m_prev = 0; }
    if (m_pal)  { free(m_pal);  m_pal  = 0; }
}

ImgDecoder *img_png_create(void) { return new PngDecoder(); }
