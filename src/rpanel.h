/* =============================================================================
 * rpanel.h - FTP-Remote-Panel (rechte Bildschirmseite)
 * -----------------------------------------------------------------------------
 * Holt das Verzeichnis-Listing ueber FtpClient::list() und wandelt die rohen
 * LIST-Textzeilen in PanelEntry um. Unterstuetzt das Unix-"ls -l"-Format und
 * (als Bonus) das MS-DOS/IIS-Format. Enter wechselt per CWD in Verzeichnisse,
 * Backspace per CDUP nach oben.
 *
 * Das Panel haelt nur einen Zeiger auf den (extern verwalteten) FtpClient.
 * Ohne Verbindung zeigt es eine leere Liste mit Hinweis im Header.
 * ===========================================================================*/
#ifndef RPANEL_H
#define RPANEL_H

#include "panel.h"
#include "ftpcli.h"

class RemotePanel : public Panel {
public:
    RemotePanel();

    /* Den (extern angelegten) FTP-Client zuordnen. */
    void attach(FtpClient *client) { ftp = client; }

    int  refresh();             /* aktuelles Remote-Verzeichnis neu listen     */
    int  enter_selected();      /* override: 1 = Verzeichniswechsel, 0 = Datei */
    void go_parent();           /* override: CDUP                              */

    const char *path() const { return cwd; }
    int  online() const { return (ftp && ftp->is_connected()) ? 1 : 0; }

    /* Letzte Navigations-/Listing-Aktion fehlgeschlagen? + Fehlertext. */
    int         nav_failed() const { return navFailed; }
    const char *last_error() const { return ftp ? ftp->last_error() : ""; }

private:
    FtpClient *ftp;
    char cwd[PANEL_HEADER_MAX];  /* aktueller Remote-Pfad (per PWD)             */
    int  navFailed;             /* 1 = letzte Aktion meldete einen Fehler      */
    int  curYear;               /* aktuelles Jahr (fuer Datumszeilen mit Zeit) */

    /* LIST-Callback (ftpcli ruft pro Roh-Zeile auf). */
    static void on_line(void *ctx, const char *line);
    void add_line(const char *line);

    /* Parser fuer die beiden gaengigen Listing-Formate.
     * Rueckgabe 1 = erkannt + e gefuellt, 0 = nicht dieses Format. */
    int  parse_unix(const char *line, PanelEntry *e);
    int  parse_dos (const char *line, PanelEntry *e);

    static int compare(const void *a, const void *b);  /* qsort-Vergleich */
};

#endif /* RPANEL_H */
