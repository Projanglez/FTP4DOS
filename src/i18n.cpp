/* =============================================================================
 * i18n.cpp - Spracherkennung ueber die DOS-Laendereinstellung
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#include <dos.h>
#include <i86.h>

#include "i18n.h"

int g_english = 0;             /* Default: Deutsch (wird in i18n_init gesetzt) */

const char *L(const char *de, const char *en)
{
    return g_english ? en : de;
}

void i18n_init(void)
{
    union REGS   in, out;
    struct SREGS sr;
    static unsigned char ctybuf[34];     /* Puffer fuer die Laenderinfo (DS:DX) */
    void far *p = (void far *)ctybuf;

    segread(&sr);
    in.h.ah = 0x38;            /* DOS: Get Country Dependent Information */
    in.h.al = 0x00;            /* 0 = aktuelles Land                    */
    in.x.dx = FP_OFF(p);
    sr.ds   = FP_SEG(p);
    intdosx(&in, &out, &sr);

    /* Fehler (Carry) -> sicherheitshalber Englisch. */
    if (out.x.cflag) { g_english = 1; return; }

    /* Laendercode in BX. Deutschsprachig: DE=49, AT=43, CH=41. */
    g_english = (out.x.bx == 49 || out.x.bx == 43 || out.x.bx == 41) ? 0 : 1;
}
