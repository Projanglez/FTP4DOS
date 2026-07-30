/* =============================================================================
 * imgview.cpp - Image viewer frontend: probe, decode, pan/zoom, display
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 *
 * The picture is decoded ONCE into a full-size store of 8-bit palette indices
 * and kept there while the viewer runs, so panning and zooming only re-sample
 * an existing image instead of decoding the file again. That store is normally
 * an XMS/EMS block (extmem.cpp): a 720x400 screenshot is 288000 bytes, which
 * conventional memory cannot spare next to a 64000-byte framebuffer. Without
 * extended memory the viewer falls back to a single fitted view, decoded
 * straight into the framebuffer, exactly as before.
 *
 * Truecolour is quantised to palette indices during the decode, so the store
 * costs one byte per pixel whatever the source was.
 *
 * Keys: arrows pan, +/- zoom, Enter/f fit, * 1:1, Esc closes.
 * ===========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>     /* kbhit, getch                */
#include <dos.h>       /* _dos_allocmem, _dos_freemem */
#include <malloc.h>    /* _heapshrink, _fheapshrink   */

#include "imgview.h"
#include "imgdec.h"
#include "gfx.h"
#include "extmem.h"
#include "tui.h"
#include "keymap.h"
#include "dialog.h"
#include "i18n.h"
#include "umlaut.h"   /* always the last include */


/* --- Format probing ------------------------------------------------------ */

static int probe_bytes(const unsigned char *b, int n)
{
    if (n >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G' &&
        b[4] == 0x0D && b[5] == 0x0A && b[6] == 0x1A && b[7] == 0x0A)
        return IMG_FMT_PNG;

    if (n >= 6 && b[0] == 'G' && b[1] == 'I' && b[2] == 'F' && b[3] == '8' &&
        (b[4] == '7' || b[4] == '9') && b[5] == 'a')
        return IMG_FMT_GIF;

    if (n >= 2 && b[0] == 'B' && b[1] == 'M')
        return IMG_FMT_BMP;

    /* PCX has only a one-byte signature, so check the neighbouring fields too:
     * version is 0..5 and the only encoding ever defined is 1. */
    if (n >= 4 && b[0] == 0x0A && b[1] <= 5 && b[2] == 1 &&
        (b[3] == 1 || b[3] == 2 || b[3] == 4 || b[3] == 8))
        return IMG_FMT_PCX;

    return IMG_FMT_NONE;
}

int img_probe_file(const char *path)
{
    FILE *f;
    unsigned char head[16];
    int n;

    f = fopen(path, "rb");
    if (!f) return IMG_FMT_NONE;
    n = (int)fread(head, 1, sizeof(head), f);
    fclose(f);
    return probe_bytes(head, n);
}

int img_ext_known(const char *ext)
{
    static const char *known[] = { "bmp", "pcx", "gif", "png", "dib", 0 };
    int i;

    if (ext == 0 || ext[0] == '\0') return 0;
    for (i = 0; known[i]; i++)
        if (stricmp(ext, known[i]) == 0) return 1;
    return 0;
}

int img_is_image_ext(const char *ext)
{
    static const char *img[] = {
        "bmp", "dib", "jpg", "jpeg", "gif", "pcx", "png",
        "tif", "tiff", "tga", "lbm", "iff", 0
    };
    int i;

    if (ext == 0 || ext[0] == '\0') return 0;
    for (i = 0; img[i]; i++)
        if (stricmp(ext, img[i]) == 0) return 1;
    return 0;
}

const char *img_ext_of(const char *name)
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

unsigned img_free_kb(void)
{
    unsigned seg = 0;

    /* Hand idle heap blocks back to DOS first. Watcom's far heap keeps every
     * block it has ever taken from DOS - free() only returns it to the heap's
     * own free list - so without this the figure drops after each viewed file
     * and never recovers, reporting 0 KB while the memory is in fact still
     * available. */
    _heapshrink();
    _fheapshrink();

    /* The oversized request is meant to fail: DOS then reports the largest
     * available block, in paragraphs, in 'seg'. */
    if (_dos_allocmem(0xFFFFu, &seg) != 0)
        return (unsigned)(((unsigned long)seg * 16UL) / 1024UL);

    _dos_freemem(seg);           /* a full megabyte was actually free */
    return 1024u;
}

static ImgDecoder *make_decoder(int fmt)
{
    switch (fmt) {
    case IMG_FMT_BMP: return img_bmp_create();
    case IMG_FMT_PCX: return img_pcx_create();
    case IMG_FMT_GIF: return img_gif_create();
    case IMG_FMT_PNG: return img_png_create();
    default:          return 0;
    }
}


/* --- Colour --------------------------------------------------------------- */

/* Truecolour is quantised to a fixed 6x6x6 cube at indices 0..215, which needs
 * no analysis pass over the image and no per-image palette. Index 216 upwards
 * is a grey ramp, which keeps photographic greys from banding into the cube's
 * coarse steps. */
#define CUBE_GREYS  40

static void build_cube_palette(unsigned char far *pal)
{
    int r, g, b, i;

    i = 0;
    for (r = 0; r < 6; r++)
        for (g = 0; g < 6; g++)
            for (b = 0; b < 6; b++) {
                pal[i * 3 + 0] = (unsigned char)(r * 51);
                pal[i * 3 + 1] = (unsigned char)(g * 51);
                pal[i * 3 + 2] = (unsigned char)(b * 51);
                i++;
            }
    for (g = 0; g < CUBE_GREYS; g++) {
        int v = g * 255 / (CUBE_GREYS - 1);
        pal[i * 3 + 0] = (unsigned char)v;
        pal[i * 3 + 1] = (unsigned char)v;
        pal[i * 3 + 2] = (unsigned char)v;
        i++;
    }
    while (i < 256) { pal[i*3] = pal[i*3+1] = pal[i*3+2] = 0; i++; }
}

/* 4x4 ordered (Bayer) dither. Adding threshold/16 of a cube step before
 * truncating spreads the quantisation error spatially instead of banding. */
static const unsigned char bayer4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5
};

static unsigned char quantize_rgb(int r, int g, int b, int x, int y)
{
    int t = (int)bayer4[((y & 3) << 2) | (x & 3)];
    int d = (t * 51) / 16;            /* 0..47, just under one cube step */
    int ri, gi, bi;

    /* Near-neutral colours go to the grey ramp: the cube only has 6 grey
     * levels, which is visibly coarse on photographs. */
    if (r - g < 12 && g - r < 12 && g - b < 12 && b - g < 12 &&
        r - b < 12 && b - r < 12) {
        int v = (r + g + b) / 3 + d - 24;
        if (v < 0) v = 0; else if (v > 255) v = 255;
        return (unsigned char)(216 + (v * (CUBE_GREYS - 1) + 127) / 255);
    }

    ri = (r + d) / 51; if (ri > 5) ri = 5;
    gi = (g + d) / 51; if (gi > 5) gi = 5;
    bi = (b + d) / 51; if (bi > 5) bi = 5;
    return (unsigned char)(ri * 36 + gi * 6 + bi);
}


/* --- Full-image store ----------------------------------------------------- */

/* One byte per pixel, held in extended memory when it will not fit the far
 * heap. Rows are addressed individually because decoders deliver them out of
 * order (bottom-up BMP, interlaced GIF). */
struct Store {
    ExtMem            *ext;   /* extended-memory block, or 0 */
    unsigned char far *mem;   /* far-heap buffer, or 0       */
    int                w, h;
};

/* Small images stay in conventional memory; this is the ceiling for that,
 * chosen to leave the far heap room for the framebuffer and a decoder. */
#define STORE_CONV_MAX  40000L

static int store_open(Store *st, int w, int h)
{
    long need = (long)w * h;

    st->ext = 0;
    st->mem = 0;
    st->w   = w;
    st->h   = h;

    if (need <= STORE_CONV_MAX) {
        st->mem = (unsigned char far *)malloc((unsigned)need);
        if (st->mem) { memset(st->mem, 0, (unsigned)need); return 1; }
    }

    st->ext = extmem_create(0);          /* XMS first, then EMS */
    if (st->ext) {
        if (st->ext->alloc(need) >= need) return 1;
        delete st->ext;
        st->ext = 0;
    }
    return 0;
}

static void store_close(Store *st)
{
    if (st->mem) { free(st->mem); st->mem = 0; }
    if (st->ext) { delete st->ext; st->ext = 0; }
}

static void store_put(Store *st, int y, const unsigned char far *row)
{
    if (y < 0 || y >= st->h) return;
    if (st->mem) memcpy(st->mem + (long)y * st->w, row, (unsigned)st->w);
    else         st->ext->write((long)y * st->w, (void *)row, st->w);
}

static void store_get(Store *st, int y, unsigned char far *row)
{
    if (y < 0 || y >= st->h) { memset(row, 0, (unsigned)st->w); return; }
    if (st->mem) memcpy(row, st->mem + (long)y * st->w, (unsigned)st->w);
    else         st->ext->read((long)y * st->w, (void *)row, st->w);
}

/* Extended memory is not pre-cleared, and a decoder may skip rows (an RLE
 * delta, a truncated file), so blank every row first. */
static void store_clear(Store *st, unsigned char far *scratch)
{
    int y;
    if (st->mem) return;                 /* already memset in store_open */
    memset(scratch, 0, (unsigned)st->w);
    for (y = 0; y < st->h; y++) st->ext->write((long)y * st->w, (void *)scratch, st->w);
}


/* --- View geometry -------------------------------------------------------- */

/* zoom is a percentage; 0 means "fit to screen". */
struct View {
    int  zoom;
    long offx, offy;      /* top-left source pixel when panning   */
    int  ox, oy;          /* destination offset when centring     */
    int  dw, dh;          /* destination extent in pixels         */
};

static void view_layout(View *v, int w, int h, const GfxMode *m)
{
    if (v->zoom == 0) {
        /* Fit: aspect-corrected, never enlarged past the source width. */
        long dw = m->width;
        long dh = (long)h * dw * m->aspect_num / ((long)w * m->aspect_den);
        if (dh > m->height) {
            dh = m->height;
            dw = (long)w * dh * m->aspect_den / ((long)h * m->aspect_num);
        }
        if (dw > w) {
            dw = w;
            dh = (long)h * m->aspect_num / m->aspect_den;
            if (dh > m->height) dh = m->height;
        }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        v->dw = (int)dw;
        v->dh = (int)dh;
        v->ox = (m->width  - v->dw) / 2;
        v->oy = (m->height - v->dh) / 2;
        v->offx = v->offy = 0;
        return;
    }

    /* A percentage zoom is a pure pixel ratio - no aspect correction, because
     * "1:1" has to mean one source pixel per screen pixel. */
    {
        long dw = (long)w * v->zoom / 100;
        long dh = (long)h * v->zoom / 100;
        long vw, vh, maxx, maxy;

        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;

        if (dw <= m->width)  { v->ox = (int)((m->width  - dw) / 2); v->offx = 0; v->dw = (int)dw; }
        else                 { v->ox = 0; v->dw = m->width;  }
        if (dh <= m->height) { v->oy = (int)((m->height - dh) / 2); v->offy = 0; v->dh = (int)dh; }
        else                 { v->oy = 0; v->dh = m->height; }

        /* Clamp the pan so the visible window stays on the image. */
        vw = (long)m->width  * 100 / v->zoom;
        vh = (long)m->height * 100 / v->zoom;
        maxx = (long)w - vw; if (maxx < 0) maxx = 0;
        maxy = (long)h - vh; if (maxy < 0) maxy = 0;
        if (v->offx > maxx) v->offx = maxx;
        if (v->offy > maxy) v->offy = maxy;
        if (v->offx < 0) v->offx = 0;
        if (v->offy < 0) v->offy = 0;
    }
}

/* Render the current view from the store into the framebuffer. */
static void view_render(Store *st, const View *v, const GfxMode *m,
                        unsigned char far *fb, unsigned char far *srcrow,
                        int *xmap)
{
    int x, y;
    int lastsy = -1;

    memset(fb, 0, (unsigned)m->width * m->height);

    /* One divide per column instead of per pixel. */
    for (x = 0; x < m->width; x++) {
        long sx;
        if (x < v->ox || x >= v->ox + v->dw) { xmap[x] = -1; continue; }
        if (v->zoom == 0) sx = (long)(x - v->ox) * st->w / v->dw;
        else              sx = v->offx + (long)(x - v->ox) * 100 / v->zoom;
        xmap[x] = (sx >= 0 && sx < st->w) ? (int)sx : -1;
    }

    for (y = 0; y < m->height; y++) {
        long sy;
        unsigned char far *out;

        if (y < v->oy || y >= v->oy + v->dh) continue;
        if (v->zoom == 0) sy = (long)(y - v->oy) * st->h / v->dh;
        else              sy = v->offy + (long)(y - v->oy) * 100 / v->zoom;
        if (sy < 0 || sy >= st->h) continue;

        /* Zoomed in, several screen rows share one source row - only fetch it
         * again when it actually changes. That matters when the store lives in
         * XMS and every fetch is a block move. */
        if ((int)sy != lastsy) { store_get(st, (int)sy, srcrow); lastsy = (int)sy; }

        out = fb + (long)y * m->width;
        for (x = 0; x < m->width; x++)
            if (xmap[x] >= 0) out[x] = srcrow[xmap[x]];
    }
}

static void view_label(const View *v)
{
    char s[16];

    if (v->zoom == 0) strcpy(s, " Fit ");
    else              sprintf(s, " %d%c ", v->zoom, '%');
    /* Right-aligned in the 40-column character grid. */
    gfx_text(GFX_TEXT_COLS - (int)strlen(s), 0, s, 15);
}


/* --- Decode --------------------------------------------------------------- */

/* Returns IMG_OK, a negative IMG_ERR_*, or 1 if the user pressed Esc. */
static int decode_to_store(ImgDecoder *dec, const ImgInfo *info, Store *st,
                           unsigned char far *rowbuf, unsigned char far *idxrow)
{
    int rc, x;

    for (;;) {
        int srow;

        rc = dec->next_row(rowbuf, &srow);
        if (rc < 0) return rc;
        if (rc == 0) break;

        if ((srow & 15) == 0 && kbhit()) {
            int k = getch();
            if (k == 0) getch();
            if (k == 27) return 1;
        }
        if (srow < 0 || srow >= info->height) continue;

        if (info->truecolor) {
            for (x = 0; x < info->width; x++) {
                const unsigned char far *p = rowbuf + (long)x * 3;
                idxrow[x] = quantize_rgb(p[0], p[1], p[2], x, srow);
            }
            store_put(st, srow, idxrow);
        } else {
            store_put(st, srow, rowbuf);
        }
    }
    return IMG_OK;
}


/* Fallback for machines with no extended memory: scale each row straight into
 * the framebuffer as it arrives and keep nothing. Gives the fitted view only -
 * there is nowhere to pan or zoom from - but it is what the viewer did before
 * the store existed, so /NOEXMEM does not lose the ability to see a picture. */
static int decode_fit_to_fb(ImgDecoder *dec, const ImgInfo *info,
                            const View *v, const GfxMode *m,
                            unsigned char far *fb, unsigned char far *rowbuf,
                            int *xmap)
{
    int rc, x;
    int last_drawn = -1;

    memset(fb, 0, (unsigned)m->width * m->height);
    for (x = 0; x < m->width; x++) {
        long sx;
        if (x < v->ox || x >= v->ox + v->dw) { xmap[x] = -1; continue; }
        sx = (long)(x - v->ox) * info->width / v->dw;
        xmap[x] = (sx >= 0 && sx < info->width) ? (int)sx : -1;
    }

    for (;;) {
        int srow, drow;

        rc = dec->next_row(rowbuf, &srow);
        if (rc < 0) return rc;
        if (rc == 0) break;

        if ((srow & 15) == 0 && kbhit()) {
            int k = getch();
            if (k == 0) getch();
            if (k == 27) return 1;
        }
        if (srow < 0 || srow >= info->height) continue;

        drow = (int)((long)srow * v->dh / info->height);
        if (drow == last_drawn || drow < 0 || drow >= v->dh) continue;
        last_drawn = drow;

        {
            unsigned char far *out = fb + (long)(v->oy + drow) * m->width;
            for (x = 0; x < m->width; x++) {
                if (xmap[x] < 0) continue;
                if (info->truecolor) {
                    const unsigned char far *p = rowbuf + (long)xmap[x] * 3;
                    out[x] = quantize_rgb(p[0], p[1], p[2], x, drow);
                } else {
                    out[x] = rowbuf[xmap[x]];
                }
            }
        }
    }
    return IMG_OK;
}


/* --- Errors --------------------------------------------------------------- */

static void img_error(int rc)
{
    const char *msg;
    char        buf[160];

    if (rc == IMG_ERR_MEM) {
        sprintf(buf, L("Not enough memory to decode this image.\n\n"
                       "%u KB free; a picture needs about 100 KB.",
                       "Zu wenig Speicher f" ue "r dieses Bild.\n\n"
                       "%u KB frei; ein Bild braucht etwa 100 KB."),
                img_free_kb());
        dlg_error(L("Image", "Bild"), buf);
        return;
    }

    switch (rc) {
    case IMG_ERR_UNSUP:
        msg = L("This file uses a variant of the format that\n"
                "FTP4DOS cannot decode.",
                "Diese Datei nutzt eine Variante des Formats,\n"
                "die FTP4DOS nicht dekodieren kann.");
        break;
    case IMG_ERR_IO:
        msg = L("The file could not be read completely.",
                "Die Datei konnte nicht vollst" ae "ndig gelesen werden.");
        break;
    case IMG_ERR_SIZE:
        msg = L("The image is too large to display.",
                "Das Bild ist zu gro" ss " zum Anzeigen.");
        break;
    default:
        msg = L("The file is damaged or not a valid image.",
                "Die Datei ist besch" ae "digt oder kein g" ue "ltiges Bild.");
        break;
    }
    dlg_error(L("Image", "Bild"), msg);
}


/* --- Entry point ---------------------------------------------------------- */

/* Zoom steps cycled by +/-. 0 = fit to screen. */
static const int zoomSteps[] = { 0, 25, 50, 100, 200, 400, 800 };
#define ZOOM_STEPS (int)(sizeof(zoomSteps) / sizeof(zoomSteps[0]))

int img_view(const char *path, const char *title, int video_pref)
{
    GfxMode        mode;
    View           view;
    ImgInfo        info;
    Store          store;
    ImgDecoder    *dec  = 0;
    FILE          *f    = 0;
    unsigned char far *fb     = 0;
    unsigned char far *rowbuf = 0;
    unsigned char far *idxrow = 0;
    unsigned char far *pal    = 0;
    int           *xmap   = 0;
    int            fmt, rc, shown = 0, zi, have_store = 0;

    gfx_query(&mode);
    memset(&store, 0, sizeof(store));

    fmt = img_probe_file(path);
    dec = make_decoder(fmt);
    if (!dec) { img_error(IMG_ERR_FORMAT); return -1; }

    f = fopen(path, "rb");
    if (!f) {
        delete dec;
        dlg_error(L("Image", "Bild"),
                  L("Cannot open file.", "Datei nicht lesbar."));
        return -1;
    }

    /* The framebuffer is claimed BEFORE the decoder opens, because it is the
     * one big contiguous request and PNG's 32 KB inflate window would
     * otherwise be sitting in the middle of the free space by the time we ask. */
    fb   = (unsigned char far *)malloc((unsigned)mode.width * mode.height);
    pal  = (unsigned char far *)malloc(768);
    xmap = (int *)malloc((unsigned)mode.width * sizeof(int));
    if (!fb || !pal || !xmap) { rc = IMG_ERR_MEM; goto fail; }

    memset(&info, 0, sizeof(info));
    rc = dec->open(f, &info);
    if (rc != IMG_OK) goto fail;

    if (info.width < 1 || info.height < 1 || info.width > IMG_MAX_WIDTH) {
        rc = IMG_ERR_SIZE;
        goto fail;
    }

    rowbuf = (unsigned char far *)malloc((unsigned)info.width * 3 + 4);
    idxrow = (unsigned char far *)malloc((unsigned)info.width + 4);
    if (!rowbuf || !idxrow) { rc = IMG_ERR_MEM; goto fail; }

    have_store = store_open(&store, info.width, info.height);

    {
        char note[80];
        sprintf(note, L(" Decoding %.30s ...", " Dekodiere %.30s ..."),
                title ? title : "");
        fill_rect(SCREEN_ROWS - 2, 0, 1, SCREEN_COLS, ' ', ATTR_STATUSBAR);
        draw_text(SCREEN_ROWS - 2, 0, note, ATTR_STATUSBAR, SCREEN_COLS);
    }

    memset(&view, 0, sizeof(view));
    view.zoom = 0;                       /* fit, until we know better */

    if (have_store) {
        store_clear(&store, idxrow);
        rc = decode_to_store(dec, &info, &store, rowbuf, idxrow);
    } else {
        view_layout(&view, info.width, info.height, &mode);
        rc = decode_fit_to_fb(dec, &info, &view, &mode, fb, rowbuf, xmap);
    }
    if (rc < 0) goto fail;
    if (rc == 1) { rc = IMG_OK; goto done; }   /* Esc during decode */

    /* The file is no longer needed once everything is in the store. */
    dec->close();
    delete dec; dec = 0;
    fclose(f);  f = 0;
    free(rowbuf); rowbuf = 0;

    if (info.truecolor) build_cube_palette(pal);
    else                memcpy(pal, info.pal, 768);

    /* Start fitted when the picture is bigger than the screen, 1:1 when it
     * already fits - showing a small image shrunk would be perverse. */
    zi = (info.width > mode.width || info.height > mode.height) ? 0 : 3;
    view.zoom = zoomSteps[zi];

    if (gfx_open() == 0) {
        int running = 1;
        shown = 1;
        gfx_palette(pal);

        while (running) {
            int k, step;

            if (have_store) {
                view_layout(&view, info.width, info.height, &mode);
                view_render(&store, &view, &mode, fb, idxrow, xmap);
            }
            gfx_blit(fb);
            view_label(&view);

            /* Without a store there is nothing to pan or zoom from: the
             * framebuffer already holds the only view we have. */
            if (!have_store) { readkey(); break; }

            k = readkey();
            /* Pan by an eighth of the visible width, so the step stays
             * sensible whether zoomed right in or right out. */
            step = (view.zoom > 0) ? (mode.width * 100 / view.zoom / 8) : 0;
            if (step < 1) step = 1;

            switch (k) {
            case KEY_ESC: case 'q': case 'Q':
                running = 0; break;
            case '+': case '=':
                if (zi < ZOOM_STEPS - 1) view.zoom = zoomSteps[++zi];
                break;
            case '-': case '_':
                if (zi > 0) view.zoom = zoomSteps[--zi];
                break;
            case 'f': case 'F': case KEY_ENTER:
                zi = 0; view.zoom = zoomSteps[0]; break;
            case '*':
                zi = 3; view.zoom = zoomSteps[3];       /* 100% */
                view.offx = view.offy = 0;
                break;
            case KEY_LEFT:  view.offx -= step; break;
            case KEY_RIGHT: view.offx += step; break;
            case KEY_UP:    view.offy -= step; break;
            case KEY_DOWN:  view.offy += step; break;
            case KEY_HOME:  view.offx = view.offy = 0; break;
            case KEY_END:   view.offx = info.width; view.offy = info.height; break;
            case KEY_PGUP:  view.offy -= (long)mode.height * 100 / (view.zoom ? view.zoom : 100); break;
            case KEY_PGDN:  view.offy += (long)mode.height * 100 / (view.zoom ? view.zoom : 100); break;
            default: break;
            }
            if (view.offx < 0) view.offx = 0;
            if (view.offy < 0) view.offy = 0;
        }
    }
    rc = IMG_OK;

done:
    if (dec) { dec->close(); delete dec; }
    if (f) fclose(f);
    store_close(&store);
    if (fb)     free(fb);
    if (rowbuf) free(rowbuf);
    if (idxrow) free(idxrow);
    if (pal)    free(pal);
    if (xmap)   free(xmap);

    if (shown) gfx_close();
    tui_init(video_pref);
    return 0;

fail:
    /* Release the big buffers before the dialog, so a memory failure is not
     * made worse by the dialog needing memory of its own. */
    store_close(&store);
    if (fb)     { free(fb);     fb = 0; }
    if (rowbuf) { free(rowbuf); rowbuf = 0; }
    if (idxrow) { free(idxrow); idxrow = 0; }
    if (pal)    { free(pal);    pal = 0; }
    if (xmap)   { free(xmap);   xmap = 0; }
    if (dec) { dec->close(); delete dec; }
    if (f) fclose(f);
    img_error(rc);
    return -1;
}
