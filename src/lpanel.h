/* =============================================================================
 * lpanel.h - Lokales Dateisystem-Panel (linke Bildschirmseite)
 * -----------------------------------------------------------------------------
 * Liest das aktuelle DOS-Verzeichnis via _dos_findfirst/_dos_findnext, sortiert
 * (".." zuerst, dann Verzeichnisse, dann Dateien - jeweils alphabetisch) und
 * stellt es als Norton-Commander-Panel dar. Enter wechselt in Verzeichnisse.
 * ===========================================================================*/
#ifndef LPANEL_H
#define LPANEL_H

#include "panel.h"

class LocalPanel : public Panel {
public:
    LocalPanel();

    int  refresh();             /* aktuelles Verzeichnis neu einlesen        */
    int  enter_selected();      /* override: 1 = Verzeichniswechsel, 0 = Datei */
    void go_parent();           /* override: ins uebergeordnete Verzeichnis  */

    const char *path() const { return cwd; }

private:
    char cwd[PANEL_HEADER_MAX]; /* aktuelles Arbeitsverzeichnis (mit Laufwerk)*/

    void read_cwd();            /* cwd aus DOS holen                          */
    static int compare(const void *a, const void *b); /* qsort-Vergleich     */
};

#endif /* LPANEL_H */
