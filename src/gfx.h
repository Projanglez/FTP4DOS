/* =============================================================================
 * gfx.h - Graphics display layer for the image viewer
 * -----------------------------------------------------------------------------
 * Today this is VGA mode 13h (320x200x256), which every VGA can do without any
 * detection. The mode is described by a GfxMode struct rather than assumed, so
 * a banked VESA mode can be added later without the decoders or the viewer
 * knowing about it.
 *
 * Pixel aspect: mode 13h pixels are 1 : 1.2 (taller than wide) on a 4:3
 * display, which is why GfxMode carries the correction the scaler needs.
 *
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#ifndef GFX_H
#define GFX_H

struct GfxMode {
    int width, height;      /* visible pixels                                */
    int colors;             /* palette entries (256)                         */
    int aspect_num;         /* pixel aspect correction, as num/den:          */
    int aspect_den;         /*   a w x h pixel block looks w : h*num/den      */
};

/* Describe the mode without switching to it - the viewer needs the geometry
 * to scale into its framebuffer before any mode change happens. */
void gfx_query(GfxMode *m);

/* Switch to graphics. Returns 0 on success. */
int  gfx_open(void);

/* Back to an 80x25 text mode. The caller still has to re-init the TUI
 * (tui_init) to restore its own state, exactly as after spawning a child. */
void gfx_close(void);

/* Load the palette: 256 RGB triples, 0..255 per channel (shifted down to the
 * DAC's 6 bits internally). */
void gfx_palette(const unsigned char far *pal768);

/* Copy a full-screen framebuffer (width*height bytes) to the display. */
void gfx_blit(const unsigned char far *fb);

/* Draw text straight onto the screen using the BIOS 8x8 graphics font, in
 * character cells (mode 13h gives a 40x25 grid). Used for the zoom readout,
 * which has to sit on top of the picture rather than in the framebuffer.
 * The glyph background is painted black, so it stays readable over anything. */
void gfx_text(int col, int row, const char *s, unsigned char color);

/* Character-cell dimensions of the current mode. */
#define GFX_TEXT_COLS 40
#define GFX_TEXT_ROWS 25

#endif /* GFX_H */
