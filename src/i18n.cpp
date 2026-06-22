/* =============================================================================
 * i18n.cpp - Language detection via the DOS country setting
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#include <dos.h>
#include <i86.h>

#include "i18n.h"

int g_english = 0;             /* Default: German (set in i18n_init) */

/* Default to US conventions; matches the previous English behaviour and is
 * used whenever the DOS country query fails. */
Locale g_locale = { ',', '.', '/', ':', 0 /*MDY*/, 0 /*12h*/ };

const char *L(const char *en, const char *de)
{
    return g_english ? en : de;
}

void i18n_init(void)
{
    union REGS   in, out;
    struct SREGS sr;
    static unsigned char ctybuf[34];     /* buffer for the country info (DS:DX) */
    void far *p = (void far *)ctybuf;
    unsigned date_fmt;

    segread(&sr);
    in.h.ah = 0x38;            /* DOS: Get Country Dependent Information */
    in.h.al = 0x00;            /* 0 = current country                   */
    in.x.dx = FP_OFF(p);
    sr.ds   = FP_SEG(p);
    intdosx(&in, &out, &sr);

    /* Error (carry) -> default to English + US formatting to be safe. */
    if (out.x.cflag) { g_english = 1; return; }

    /* Country code in BX. German-speaking: DE=49, AT=43, CH=41. */
    g_english = (out.x.bx == 49 || out.x.bx == 43 || out.x.bx == 41) ? 0 : 1;

    /* Regional formatting from the country info block (DOS layout):
     *   off 0  (word) : date format  0=MDY 1=DMY 2=YMD
     *   off 7  (byte) : thousands separator (ASCIIZ, char at off 7)
     *   off 9  (byte) : decimal separator   (ASCIIZ, char at off 9)
     *   off 11 (byte) : date separator      (ASCIIZ, char at off 11)
     *   off 13 (byte) : time separator      (ASCIIZ, char at off 13)
     *   off 17 (byte) : time format, bit 0  0=12h 1=24h
     * Fall back to a sensible default for any field DOS leaves blank. */
    date_fmt = (unsigned)ctybuf[0] | ((unsigned)ctybuf[1] << 8);
    g_locale.date_order    = (date_fmt <= 2) ? (int)date_fmt : 0;
    g_locale.thousands_sep = ctybuf[7]  ? (char)ctybuf[7]  : ',';
    g_locale.decimal_sep   = ctybuf[9]  ? (char)ctybuf[9]  : '.';
    g_locale.date_sep      = ctybuf[11] ? (char)ctybuf[11] : '/';
    g_locale.time_sep      = ctybuf[13] ? (char)ctybuf[13] : ':';
    g_locale.time_24h      = (ctybuf[17] & 0x01) ? 1 : 0;
}
