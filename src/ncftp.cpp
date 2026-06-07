/* =============================================================================
 * ncftp.cpp - NCFTP386: Main, Bildschirmaufbau und Event-Loop
 * -----------------------------------------------------------------------------
 * Norton-Commander-artiger Dual-Panel-Dateimanager: links lokales DOS-Datei-
 * system, rechts FTP-remote ueber mTCP. Die Hauptschleife arbeitet polymorph
 * ueber Panel*.
 *
 * Bildschirm (80x25) - ohne Menue-/Kommandozeile, maximaler Panelinhalt:
 *   Zeilen 0-22 : zwei Panels (je 40 Spalten breit)
 *   Zeile  23   : Statusleiste (Dateiinfo + Verbindungsstatus)
 *   Zeile  24   : Funktionstastenleiste
 *
 * Sprache (Deutsch/Englisch) wird beim Start aus der DOS-Laendereinstellung
 * abgeleitet; alle sichtbaren Texte laufen ueber L("de","en").
 * ===========================================================================*/
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <direct.h>

#include "tui.h"
#include "panel.h"
#include "lpanel.h"
#include "rpanel.h"
#include "ftpcli.h"
#include "keymap.h"
#include "dialog.h"
#include "viewer.h"
#include "i18n.h"

/* ---- Bildschirm-Layout ---- */
#define PANEL_TOP     0
#define PANEL_ROWS    23           /* Zeilen 0..22                     */
#define PANEL_COLS    40
#define ROW_STATUS    23
#define ROW_FKEYS     24

/* ---- Globale Panels ---- */
/* Grosse Objekte (~25 KB je Panel): beim Kompilieren von ncftp.cpp -zt256
 * setzen, damit Open Watcom sie in FAR-Datensegmente legt (DGROUP < 64 KB).
 * Links lokal, rechts FTP-remote. Die Hauptschleife arbeitet polymorph
 * ueber Panel*. */
static LocalPanel  g_left;
static RemotePanel g_right;
static Panel      *g_active = 0;

/* FTP-Client (Steuerung der Remote-Seite). g_ftp_ready=1, sobald der
 * mTCP-Stack erfolgreich initialisiert wurde. */
static FtpClient g_ftp;
static int       g_ftp_ready = 0;

/* -------------------------------------------------------------------------
 * Bildschirm-Chrome
 * ---------------------------------------------------------------------- */

/* Funktionstasten-Beschriftung je Sprache. F7 = "MkDir" (in beiden gleich). */
static const char *fkey_label(int i)
{
    static const char *de[10] = {
        "Hilfe", "Verb", "Anzeig", "Edit", "Kopier",
        "Umben", "MkDir", "Loesch", "Menue", "Ende"
    };
    static const char *en[10] = {
        "Help", "Conn", "View", "Edit", "Copy",
        "Ren",  "MkDir", "Del", "Menu", "Quit"
    };
    return g_english ? en[i] : de[i];
}

static void draw_fkeybar(void)
{
    int i;
    fill_rect(ROW_FKEYS, 0, 1, SCREEN_COLS, ' ', ATTR_FNKEY_LBL);
    for (i = 0; i < 10; i++) {
        int col = i * 8;            /* 10 Zellen a 8 Spalten = 80 */
        char num[4];
        int nlen;
        sprintf(num, "%d", i + 1);  /* 1..10 */
        nlen = (int)strlen(num);
        draw_text(ROW_FKEYS, col,        num,          ATTR_FNKEY_NUM, nlen);
        draw_text(ROW_FKEYS, col + nlen, fkey_label(i), ATTR_FNKEY_LBL, 8 - nlen);
    }
}

/* "1234567" -> "1.234.567" (de) bzw. "1,234,567" (en). out >= 20 Zeichen. */
static void format_thousands(unsigned long v, char *out)
{
    char tmp[16];
    char sep = g_english ? ',' : '.';
    int  ndig = sprintf(tmp, "%lu", v);
    int  i, o = 0;
    for (i = 0; i < ndig; i++) {
        if (i > 0 && ((ndig - i) % 3) == 0) out[o++] = sep;
        out[o++] = tmp[i];
    }
    out[o] = '\0';
}

/* Bytes -> "(123 KB)" bzw. "(1,2 MB)". MB ab > 1000 KB. out >= 24 Zeichen. */
static void format_human(unsigned long bytes, char *out)
{
    char dec = g_english ? '.' : ',';
    unsigned long kb = bytes / 1024UL;
    if (kb > 1000UL) {
        unsigned long MB    = 1048576UL;
        unsigned long whole = bytes / MB;
        unsigned long frac  = (bytes % MB) * 10UL / MB;
        sprintf(out, "(%lu%c%lu MB)", whole, dec, frac);
    } else {
        sprintf(out, "(%lu KB)", kb);
    }
}

static void draw_statusbar(void)
{
    char info[120];
    char conn[48];
    int  clen;
    PanelEntry *e = g_active ? g_active->selected() : 0;

    if (g_ftp.is_connected())
        sprintf(conn, "%s %.20s", L("Verbunden:", "Connected:"), g_ftp.host_name());
    else
        strcpy(conn, L("Nicht verbunden", "Not connected"));
    clen = (int)strlen(conn);

    fill_rect(ROW_STATUS, 0, 1, SCREEN_COLS, ' ', ATTR_STATUSBAR);

    if (e) {
        if (e->is_dir) {
            sprintf(info, " %-12s   <DIR>", e->name);
        } else if (e->size > 999UL) {
            char num[20];
            format_thousands(e->size, num);
            if (e->size >= 1024UL) {
                char hum[24];
                format_human(e->size, hum);
                sprintf(info, " %-12s   %s Bytes %s", e->name, num, hum);
            } else {
                sprintf(info, " %-12s   %s Bytes", e->name, num);
            }
        } else {
            sprintf(info, " %-12s   %lu Bytes", e->name, e->size);
        }
    } else {
        strcpy(info, L(" (leer)", " (empty)"));
    }
    draw_text(ROW_STATUS, 0, info, ATTR_STATUSBAR, SCREEN_COLS - 2 - clen);
    draw_text(ROW_STATUS, SCREEN_COLS - clen, conn, ATTR_STATUSBAR, clen);
}

/* Kurze Meldung in der Statusleiste (bis zur naechsten Aktualisierung). */
static void flash_status(const char *msg)
{
    fill_rect(ROW_STATUS, 0, 1, SCREEN_COLS, ' ', ATTR_STATUSBAR);
    draw_text(ROW_STATUS, 1, msg, ATTR_STATUSBAR, SCREEN_COLS - 2);
}

static void set_active(Panel *p)
{
    g_active = p;
    g_left.set_active(p == (Panel *)&g_left);
    g_right.set_active(p == (Panel *)&g_right);
}

static void redraw_all(void)
{
    g_left.draw();
    g_right.draw();
    draw_statusbar();
    draw_fkeybar();
}

/* -------------------------------------------------------------------------
 * F2 - FTP-Verbindung herstellen / trennen
 * Verbindungsdaten werden ueber vier nacheinander gezeigte Eingabefelder
 * abgefragt (Host/Port/Benutzer/Passwort) und fuer die naechste Sitzung
 * gemerkt. Die eigentliche Verbindung ist blockierend.
 * ---------------------------------------------------------------------- */
static void do_connect(void)
{
    static char host[FTP_HOST_MAX] = "";
    static char portStr[8]         = "21";
    static char user[40]           = "anonymous";
    static char pass[40]           = "";
    unsigned port;
    int rc;

    if (!g_ftp_ready) {
        dlg_error(L("FTP nicht verfuegbar", "FTP unavailable"),
                  L("TCP/IP konnte nicht gestartet werden.\n"
                    "MTCPCFG pruefen und Programm neu starten.",
                    "TCP/IP could not be started.\n"
                    "Check MTCPCFG and restart the program."));
        redraw_all();
        return;
    }

    /* Bereits verbunden -> Trennen anbieten. */
    if (g_ftp.is_connected()) {
        if (dlg_confirm(L("Trennen", "Disconnect"),
                        L("Bestehende FTP-Verbindung trennen?",
                          "Close the current FTP connection?"))) {
            g_ftp.disconnect();
            g_right.refresh();
        }
        redraw_all();
        return;
    }

    if (!dlg_input(L("FTP-Verbindung", "FTP Connection"), "Host:", host, FTP_HOST_MAX - 1, 0)) { redraw_all(); return; }
    if (host[0] == '\0')                                                                        { redraw_all(); return; }
    if (!dlg_input(L("FTP-Verbindung", "FTP Connection"), "Port:", portStr, 6, 0))              { redraw_all(); return; }
    if (!dlg_input(L("FTP-Verbindung", "FTP Connection"), L("Benutzer:", "User:"), user, 38, 0)){ redraw_all(); return; }
    if (!dlg_input(L("FTP-Verbindung", "FTP Connection"), L("Passwort:", "Password:"), pass, 38, 1)) { redraw_all(); return; }

    port = (unsigned)atoi(portStr);
    if (port == 0) port = 21;

    redraw_all();
    flash_status(L(" Verbinde mit FTP-Server ...", " Connecting to FTP server ..."));

    rc = g_ftp.connect(host, port, user, pass);
    if (rc != FTP_OK) {
        dlg_error(L("Verbindung fehlgeschlagen", "Connection failed"), g_ftp.last_error());
        redraw_all();
        return;
    }

    /* Erfolg: rechtes Panel listet das Remote-Verzeichnis, Fokus dorthin. */
    g_right.refresh();
    set_active((Panel *)&g_right);
    redraw_all();
}

/* -------------------------------------------------------------------------
 * F5 - Datei kopieren zwischen lokalem und Remote-Panel
 * Die Richtung ergibt sich aus dem aktiven Panel:
 *   aktiv = lokal  -> Upload   (STOR) lokal  -> Remote
 *   aktiv = remote -> Download (RETR) Remote -> lokal
 * Verzeichnisse werden (noch) nicht rekursiv kopiert. Der Zielname laesst sich
 * vor dem Transfer editieren (Norton-Commander-Manier).
 * ---------------------------------------------------------------------- */

/* Fortschritts-Callback fuer FtpClient::retr/stor (waehrend des Transfers). */
static void copy_progress(void *ctx, unsigned long sofar, unsigned long total)
{
    (void)ctx;
    dlg_progress_update(sofar, total);
}

/* 1, falls die lokale Datei existiert (fuer die Ueberschreiben-Abfrage). */
static int local_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* Lokalen Pfad "dir\name" zusammensetzen (Wurzel "C:\" beachten), laengensicher. */
static void join_local(char *out, int outsz, const char *dir, const char *name)
{
    int n;
    strncpy(out, dir, outsz - 1);
    out[outsz - 1] = '\0';
    n = (int)strlen(out);
    if (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/' && out[n - 1] != ':'
        && n < outsz - 1) {
        out[n++] = '\\';
        out[n] = '\0';
    }
    strncat(out, name, outsz - 1 - (int)strlen(out));
}

static void do_copy(void)
{
    char target[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
    char prompt[64];
    PanelEntry *e;
    int rc;

    if (g_active == 0) return;

    if (!g_ftp.is_connected()) {
        dlg_error(L("Kopieren", "Copy"),
                  L("Keine FTP-Verbindung.\nMit F2 zuerst verbinden.",
                    "No FTP connection.\nConnect with F2 first."));
        redraw_all();
        return;
    }

    e = g_active->selected();
    if (e == 0 || e->is_parent) { redraw_all(); return; }
    if (e->is_dir) {
        dlg_error(L("Kopieren", "Copy"),
                  L("Nur einzelne Dateien koennen kopiert\nwerden, keine Verzeichnisse.",
                    "Only single files can be copied,\nnot directories."));
        redraw_all();
        return;
    }

    if (g_active == (Panel *)&g_right) {
        /* --- Download: Remote -> lokal --- */
        join_local(target, (int)sizeof(target), g_left.path(), e->name);
        sprintf(prompt, L("\"%.20s\" laden nach:", "Download \"%.20s\" to:"), e->name);
        if (!dlg_input(L("Download", "Download"), prompt, target, (int)sizeof(target) - 1, 0)) { redraw_all(); return; }
        if (target[0] == '\0') { redraw_all(); return; }

        /* Existiert die lokale Datei schon? -> vor jedem FTP-Verkehr nachfragen. */
        if (local_exists(target)) {
            char q[120];
            sprintf(q, L("Lokale Datei existiert bereits:\n%.40s\nUeberschreiben?",
                         "Local file already exists:\n%.40s\nOverwrite?"), target);
            if (!dlg_confirm(L("Download", "Download"), q)) { redraw_all(); return; }
        }

        redraw_all();
        dlg_progress_begin(L("Download", "Download"), e->name);
        rc = g_ftp.retr(e->name, target, copy_progress, 0);
        dlg_progress_end();

        if (rc != FTP_OK) dlg_error(L("Download fehlgeschlagen", "Download failed"), g_ftp.last_error());
        g_left.refresh();   /* neue lokale Datei sichtbar machen */
    } else {
        /* --- Upload: lokal -> Remote --- */
        char localpath[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        join_local(localpath, (int)sizeof(localpath), g_left.path(), e->name);
        strncpy(target, e->name, sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
        sprintf(prompt, L("\"%.20s\" senden als:", "Upload \"%.20s\" as:"), e->name);
        if (!dlg_input(L("Upload", "Upload"), prompt, target, (int)sizeof(target) - 1, 0)) { redraw_all(); return; }
        if (target[0] == '\0') { redraw_all(); return; }

        /* Existiert die Remote-Datei schon? -> vor jedem FTP-Verkehr nachfragen. */
        if (g_right.has_entry(target)) {
            char q[120];
            sprintf(q, L("Remote-Datei existiert bereits:\n%.40s\nUeberschreiben?",
                         "Remote file already exists:\n%.40s\nOverwrite?"), target);
            if (!dlg_confirm(L("Upload", "Upload"), q)) { redraw_all(); return; }
        }

        redraw_all();
        dlg_progress_begin(L("Upload", "Upload"), e->name);
        rc = g_ftp.stor(localpath, target, copy_progress, 0);
        dlg_progress_end();

        if (rc != FTP_OK) dlg_error(L("Upload fehlgeschlagen", "Upload failed"), g_ftp.last_error());
        g_right.refresh();  /* neue Remote-Datei sichtbar machen */
    }

    redraw_all();
}

/* -------------------------------------------------------------------------
 * F3 - Datei anzeigen (lokal direkt, remote ueber temporaeren Download)
 * ---------------------------------------------------------------------- */
static void do_view(void)
{
    char path[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
    PanelEntry *e;

    if (g_active == 0) return;
    e = g_active->selected();
    if (e == 0 || e->is_parent || e->is_dir) { redraw_all(); return; }

    if (g_active == (Panel *)&g_left) {
        /* Lokale Datei direkt anzeigen. */
        join_local(path, (int)sizeof(path), g_left.path(), e->name);
        view_file(path, e->name);
    } else {
        /* Remote: in temporaere lokale Datei laden, anzeigen, dann loeschen. */
        int rc;
        if (!g_ftp.is_connected()) {
            dlg_error(L("Anzeigen", "View"),
                      L("Keine FTP-Verbindung.\nMit F2 zuerst verbinden.",
                        "No FTP connection.\nConnect with F2 first."));
            redraw_all();
            return;
        }
        join_local(path, (int)sizeof(path), g_left.path(), "$NCVIEW$.TMP");
        redraw_all();
        dlg_progress_begin(L("Anzeigen", "View"), e->name);
        rc = g_ftp.retr(e->name, path, copy_progress, 0);
        dlg_progress_end();
        if (rc != FTP_OK) {
            remove(path);   /* evtl. angefangene Temp-Datei aufraeumen */
            dlg_error(L("Anzeigen fehlgeschlagen", "View failed"), g_ftp.last_error());
            redraw_all();
            return;
        }
        view_file(path, e->name);
        remove(path);
    }
    redraw_all();
}

/* -------------------------------------------------------------------------
 * F7 - Verzeichnis erstellen (lokal oder remote)
 * ---------------------------------------------------------------------- */
static void do_mkdir(void)
{
    char name[PANEL_NAME_MAX];
    int rc;

    if (g_active == 0) return;
    name[0] = '\0';
    if (!dlg_input(L("Verzeichnis erstellen", "Make Directory"),
                   L("Name:", "Name:"), name, PANEL_NAME_MAX - 1, 0)) {
        redraw_all(); return;
    }
    if (name[0] == '\0') { redraw_all(); return; }

    if (g_active == (Panel *)&g_right) {
        if (!g_ftp.is_connected()) {
            dlg_error(L("Verzeichnis erstellen", "Make Directory"),
                      L("Keine FTP-Verbindung.\nMit F2 zuerst verbinden.",
                        "No FTP connection.\nConnect with F2 first."));
            redraw_all(); return;
        }
        rc = g_ftp.make_dir(name);
        if (rc != FTP_OK)
            dlg_error(L("Verzeichnis erstellen", "Make Directory"), g_ftp.last_error());
        else
            g_right.refresh();
    } else {
        char path[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        join_local(path, (int)sizeof(path), g_left.path(), name);
        if (_mkdir(path) != 0)
            dlg_error(L("Verzeichnis erstellen", "Make Directory"),
                      L("Konnte Verzeichnis nicht anlegen.", "Could not create directory."));
        else
            g_left.refresh();
    }
    redraw_all();
}

/* -------------------------------------------------------------------------
 * F8 - Loeschen mit Bestaetigung (Datei oder leeres Verzeichnis)
 * ---------------------------------------------------------------------- */
static void do_delete(void)
{
    char prompt[80];
    PanelEntry *e;
    int rc;

    if (g_active == 0) return;
    e = g_active->selected();
    if (e == 0 || e->is_parent) { redraw_all(); return; }

    if (e->is_dir)
        sprintf(prompt, L("Verzeichnis \"%.28s\"\nloeschen?", "Delete directory\n\"%.28s\"?"), e->name);
    else
        sprintf(prompt, L("Datei \"%.32s\"\nloeschen?", "Delete file\n\"%.32s\"?"), e->name);

    if (!dlg_confirm(L("Loeschen", "Delete"), prompt)) { redraw_all(); return; }

    if (g_active == (Panel *)&g_right) {
        if (!g_ftp.is_connected()) {
            dlg_error(L("Loeschen", "Delete"),
                      L("Keine FTP-Verbindung.\nMit F2 zuerst verbinden.",
                        "No FTP connection.\nConnect with F2 first."));
            redraw_all(); return;
        }
        rc = e->is_dir ? g_ftp.remove_dir(e->name) : g_ftp.remove_file(e->name);
        if (rc != FTP_OK)
            dlg_error(L("Loeschen fehlgeschlagen", "Delete failed"), g_ftp.last_error());
        else
            g_right.refresh();
    } else {
        char path[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        join_local(path, (int)sizeof(path), g_left.path(), e->name);
        if (e->is_dir) {
            if (_rmdir(path) != 0)
                dlg_error(L("Loeschen fehlgeschlagen", "Delete failed"),
                          L("Verzeichnis nicht leer\noder kein Zugriff.",
                            "Directory not empty\nor access denied."));
            else
                g_left.refresh();
        } else {
            if (remove(path) != 0)
                dlg_error(L("Loeschen fehlgeschlagen", "Delete failed"),
                          L("Datei konnte nicht\ngeloescht werden.",
                            "Could not delete\nthe file."));
            else
                g_left.refresh();
        }
    }
    redraw_all();
}

/* -------------------------------------------------------------------------
 * F6 - Umbenennen (Datei oder Verzeichnis, lokal oder remote)
 * Reines Umbenennen im selben Verzeichnis (kein Verschieben zwischen Panels).
 * ---------------------------------------------------------------------- */
static void do_rename(void)
{
    char newname[PANEL_NAME_MAX];
    char prompt[64];
    PanelEntry *e;
    int rc;

    if (g_active == 0) return;
    e = g_active->selected();
    if (e == 0 || e->is_parent) { redraw_all(); return; }

    strncpy(newname, e->name, sizeof(newname) - 1);
    newname[sizeof(newname) - 1] = '\0';
    sprintf(prompt, L("\"%.20s\" umbenennen in:", "Rename \"%.20s\" to:"), e->name);
    if (!dlg_input(L("Umbenennen", "Rename"), prompt, newname, PANEL_NAME_MAX - 1, 0)) { redraw_all(); return; }
    if (newname[0] == '\0')            { redraw_all(); return; }
    if (strcmp(newname, e->name) == 0) { redraw_all(); return; }   /* unveraendert */

    if (g_active == (Panel *)&g_right) {
        if (!g_ftp.is_connected()) {
            dlg_error(L("Umbenennen", "Rename"),
                      L("Keine FTP-Verbindung.\nMit F2 zuerst verbinden.",
                        "No FTP connection.\nConnect with F2 first."));
            redraw_all(); return;
        }
        rc = g_ftp.rename(e->name, newname);
        if (rc != FTP_OK)
            dlg_error(L("Umbenennen fehlgeschlagen", "Rename failed"), g_ftp.last_error());
        else
            g_right.refresh();
    } else {
        char oldpath[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        char newpath[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        join_local(oldpath, (int)sizeof(oldpath), g_left.path(), e->name);
        join_local(newpath, (int)sizeof(newpath), g_left.path(), newname);
        if (rename(oldpath, newpath) != 0)
            dlg_error(L("Umbenennen fehlgeschlagen", "Rename failed"),
                      L("Konnte nicht umbenennen.\nName ungueltig oder existiert bereits.",
                        "Could not rename.\nName invalid or already exists."));
        else
            g_left.refresh();
    }
    redraw_all();
}

/* -------------------------------------------------------------------------
 * Main / Event-Loop
 * ---------------------------------------------------------------------- */
int main(void)
{
    int running = 1;

    /* Sprache aus der DOS-Laendereinstellung bestimmen (vor jeder Ausgabe). */
    i18n_init();

    /* mTCP-Stack VOR tui_init starten: parseEnv/initStack koennen bei Fehler
     * auf stderr schreiben - tui_init loescht den Schirm anschliessend. Schlaegt
     * es fehl (z.B. MTCPCFG fehlt / kein Packet-Driver), laeuft das Programm
     * als reiner lokaler Dateimanager weiter; F2 meldet dann den Fehler. */
    g_ftp_ready = (FtpClient::init_stack() == FTP_OK) ? 1 : 0;
    g_right.attach(&g_ftp);

    tui_init();

    g_left.set_region(PANEL_TOP, 0,          PANEL_ROWS, PANEL_COLS);
    g_right.set_region(PANEL_TOP, PANEL_COLS, PANEL_ROWS, PANEL_COLS);
    g_left.refresh();
    g_right.refresh();
    set_active((Panel *)&g_left);

    redraw_all();

    while (running) {
        int key = readkey();

        switch (key) {
        case KEY_TAB:
            set_active(g_active == (Panel *)&g_left
                       ? (Panel *)&g_right : (Panel *)&g_left);
            g_left.draw();
            g_right.draw();
            draw_statusbar();
            break;

        case KEY_UP:   g_active->move_up();   g_active->draw(); draw_statusbar(); break;
        case KEY_DOWN: g_active->move_down(); g_active->draw(); draw_statusbar(); break;
        case KEY_PGUP: g_active->page_up();   g_active->draw(); draw_statusbar(); break;
        case KEY_PGDN: g_active->page_down(); g_active->draw(); draw_statusbar(); break;
        case KEY_HOME: g_active->move_home(); g_active->draw(); draw_statusbar(); break;
        case KEY_END:  g_active->move_end();  g_active->draw(); draw_statusbar(); break;

        case KEY_ENTER:
            if (g_active->enter_selected()) {
                g_active->draw();
                draw_statusbar();
                if (g_active == (Panel *)&g_right && g_right.nav_failed())
                    flash_status(g_right.last_error());
            }
            /* sonst: Datei -> Anzeigen (wie F3). */
            else {
                do_view();
            }
            break;

        case KEY_BACKSP:
            g_active->go_parent();
            g_active->draw();
            draw_statusbar();
            if (g_active == (Panel *)&g_right && g_right.nav_failed())
                flash_status(g_right.last_error());
            break;

        /* Funktionstasten. */
        case KEY_F1:
            dlg_message(L("Hilfe", "Help"),
                L("Tab        Panel wechseln\n"
                  "Pfeile     Auswahl bewegen\n"
                  "Bild auf/ab  Seitenweise\n"
                  "Enter      Verzeichnis betreten / Datei anzeigen\n"
                  "Backspace  Uebergeordnetes Verzeichnis\n"
                  "F2 Verbinden  F3 Anzeigen  F5 Kopieren\n"
                  "F6 Umbenennen  F7 MkDir  F8 Loeschen\n"
                  "F10        Beenden",
                  "Tab        Switch panel\n"
                  "Arrows     Move selection\n"
                  "PgUp/PgDn  Page up / down\n"
                  "Enter      Enter directory / view file\n"
                  "Backspace  Parent directory\n"
                  "F2 Connect  F3 View  F5 Copy\n"
                  "F6 Rename  F7 MkDir  F8 Delete\n"
                  "F10        Quit"), 0);
            break;
        case KEY_F2:  do_connect(); break;
        case KEY_F3:  do_view(); break;
        case KEY_F4:  flash_status(L(" F4  Bearbeiten - folgt spaeter", " F4  Edit - coming later")); break;
        case KEY_F5:  do_copy(); break;
        case KEY_F6:  do_rename(); break;
        case KEY_F7:  do_mkdir(); break;
        case KEY_F8:  do_delete(); break;
        case KEY_F9:  flash_status(L(" F9  Menue - folgt spaeter", " F9  Menu - coming later")); break;

        case KEY_F10:
            if (dlg_confirm(L("Beenden", "Quit"),
                            L("NCFTP386 wirklich beenden?", "Really quit NCFTP386?")))
                running = 0;
            break;

        default:
            break;
        }
    }

    /* Sauber trennen und mTCP-Stack zurueckgeben. */
    if (g_ftp.is_connected())
        g_ftp.disconnect();
    if (g_ftp_ready)
        FtpClient::shutdown_stack();

    tui_shutdown();
    return 0;
}
