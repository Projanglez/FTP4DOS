/* =============================================================================
 * gfx.cpp - VGA mode 13h display layer
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 *
 * Mode 13h is 320x200x256 with a linear framebuffer at A000:0000, 64000 bytes -
 * under 64 KB, so one far pointer addresses the whole screen with no banking.
 * That is the reason this mode is the baseline: no VESA detection, no window
 * switching, and it exists on every VGA ever made.
 * ===========================================================================*/
#include <dos.h>       /* int86, MK_FP        */
#include <conio.h>     /* outp                */
#include <string.h>    /* memcpy              */

#include "gfx.h"

#define VGA_SEG        0xA000U
#define MODE13_W       320
#define MODE13_H       200
#define MODE13_CELLS   ((unsigned)MODE13_W * MODE13_H)   /* 64000 */

#define DAC_WRITE_IDX  0x3C8
#define DAC_DATA       0x3C9


void gfx_query(GfxMode *m)
{
    m->width      = MODE13_W;
    m->height     = MODE13_H;
    m->colors     = 256;
    /* 320x200 shown on a 4:3 screen: pixels are 1.2x taller than wide, so a
     * square source needs 5/6 of the rows it would otherwise get. */
    m->aspect_num = 5;
    m->aspect_den = 6;
}


int gfx_open(void)
{
    union REGS r;

    r.h.ah = 0x00;
    r.h.al = 0x13;
    int86(0x10, &r, &r);

    /* Confirm the BIOS actually took it (AH=0Fh returns the current mode). */
    r.h.ah = 0x0F;
    int86(0x10, &r, &r);
    return (r.h.al == 0x13) ? 0 : -1;
}


void gfx_close(void)
{
    union REGS r;

    /* Plain 80x25 colour text. The caller re-runs tui_init() afterwards, which
     * sets the mode it actually wants (03h or 07h on a mono adapter). */
    r.h.ah = 0x00;
    r.h.al = 0x03;
    int86(0x10, &r, &r);
}


void gfx_palette(const unsigned char far *pal768)
{
    int i;

    /* The DAC takes 6 bits per channel, so the 0..255 values shift down by 2.
     * Writing index 0 once then streaming 768 bytes is the fast path: the
     * write index auto-increments after every third byte. */
    outp(DAC_WRITE_IDX, 0);
    for (i = 0; i < 768; i++)
        outp(DAC_DATA, (unsigned char)(pal768[i] >> 2));
}


void gfx_blit(const unsigned char far *fb)
{
    unsigned char far *vid = (unsigned char far *)MK_FP(VGA_SEG, 0);

    /* 64000 bytes fits a 16-bit size_t and stays inside one segment. */
    memcpy(vid, fb, MODE13_CELLS);
}


void gfx_text(int col, int row, const char *s, unsigned char color)
{
    union REGS r;

    /* AH=09h writes a glyph from the BIOS 8x8 font; in a graphics mode BL is
     * the foreground colour and the glyph's background pixels come out as
     * colour 0. It does not advance the cursor, so each character is placed
     * explicitly. */
    for (; *s && col < GFX_TEXT_COLS; s++, col++) {
        r.h.ah = 0x02;              /* set cursor position */
        r.h.bh = 0;
        r.h.dh = (unsigned char)row;
        r.h.dl = (unsigned char)col;
        int86(0x10, &r, &r);

        r.h.ah = 0x09;              /* write character with attribute */
        r.h.al = (unsigned char)*s;
        r.h.bh = 0;
        r.h.bl = color;
        r.x.cx = 1;
        int86(0x10, &r, &r);
    }
}
