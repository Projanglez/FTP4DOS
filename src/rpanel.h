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

/* Eine rohe LIST-Zeile in einen PanelEntry parsen (Unix- oder DOS/IIS-Format).
 * curYear liefert das Jahr fuer Zeilen, die nur eine Uhrzeit (kein Jahr) tragen.
 * Rueckgabe 1 = erkannt (e gefuellt: name, size, date, is_dir, marked=0),
 *           0 = Zeile nicht als Eintrag erkennbar. Wird auch vom rekursiven
 * Verzeichnis-Download (dircopy.cpp) genutzt. */
int ftp_parse_list_line(const char *line, int curYear, PanelEntry *e);

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

    static int compare(const void *a, const void *b);  /* qsort-Vergleich */
};

#endif /* RPANEL_H */
