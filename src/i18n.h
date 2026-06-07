/* =============================================================================
 * i18n.h - Zweisprachigkeit (Deutsch / Englisch) fuer NCFTP386
 * -----------------------------------------------------------------------------
 * Die Sprache wird beim Start aus der DOS-Laendereinstellung (INT 21h/38h,
 * gesetzt ueber COUNTRY= in CONFIG.SYS) ermittelt: Deutschland/Oesterreich/
 * Schweiz -> Deutsch, alles andere -> Englisch.
 *
 * Verwendung im Code:  dlg_error(L("Fehler", "Error"), L("...", "..."));
 * ===========================================================================*/
#ifndef I18N_H
#define I18N_H

extern int g_english;          /* 0 = Deutsch, 1 = English */

/* Sprache aus der DOS-Laendereinstellung bestimmen (einmal beim Start). */
void i18n_init(void);

/* Liefert je nach erkannter Sprache den deutschen (de) oder englischen (en)
 * Text. Beide Zeichenketten sind statische Literale, daher gefahrlos. */
const char *L(const char *de, const char *en);

#endif /* I18N_H */
