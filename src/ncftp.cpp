/* =============================================================================
 * ncftp.cpp - NCFTP386: Main, Bildschirmaufbau und Event-Loop
 * -----------------------------------------------------------------------------
 * Norton-Commander-artiger Dual-Panel-Dateimanager. In diesem Schritt sind
 * BEIDE Panels lokal (klassischer NC-Start). Sobald der FTP-Teil steht
 * (Schritt 6), wird das rechte Panel durch ein RemotePanel ersetzt - die
 * Hauptschleife arbeitet bereits polymorph ueber Panel*, daher genuegt dort
 * spaeter ein Austausch des Objekts.
 *
 * Bildschirm (80x25):
 *   Zeile  0   : Menueleiste
 *   Zeilen 1-21: zwei Panels (je 40 Spalten)
 *   Zeile  22  : Statusleiste
 *   Zeile  23  : Kommandozeile (Pfad-Prompt)
 *   Zeile  24  : Funktionstastenleiste
 * ===========================================================================*/
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "tui.h"
#include "panel.h"
#include "lpanel.h"
#include "rpanel.h"
#include "ftpcli.h"
#include "keymap.h"
#include "dialog.h"

/* ---- Bildschirm-Layout ---- */
#define ROW_MENU      0
#define PANEL_TOP     1
#define PANEL_ROWS    21
#define PANEL_COLS    40
#define ROW_STATUS    22
#define ROW_CMDLINE   23
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
static void draw_menubar(void)
{
    fill_rect(ROW_MENU, 0, 1, SCREEN_COLS, ' ', ATTR_MENUBAR);
    draw_text(ROW_MENU, 1, " Left   Files   Commands   Options   Right ",
              ATTR_MENUBAR, SCREEN_COLS - 1);
}

static const char *g_fkey_labels[10] = {
    "Hilfe", "Verb", "Anzeig", "Edit", "Kopier",
    "Umben", "Neu",  "Loesch", "Menue", "Ende"
};

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
        draw_text(ROW_FKEYS, col,        num,             ATTR_FNKEY_NUM, nlen);
        draw_text(ROW_FKEYS, col + nlen, g_fkey_labels[i], ATTR_FNKEY_LBL, 8 - nlen);
    }
}

static void draw_statusbar(void)
{
    char info[80];
    char conn[40];
    int clen;
    PanelEntry *e = g_active ? g_active->selected() : 0;

    if (g_ftp.is_connected())
        sprintf(conn, "Verbunden: %.24s", g_ftp.host_name());
    else
        strcpy(conn, "Nicht verbunden");
    clen = (int)strlen(conn);

    fill_rect(ROW_STATUS, 0, 1, SCREEN_COLS, ' ', ATTR_STATUSBAR);

    if (e) {
        if (e->is_dir)
            sprintf(info, " %-12s   <DIR>", e->name);
        else
            sprintf(info, " %-12s   %lu Bytes", e->name, e->size);
    } else {
        strcpy(info, " (leer)");
    }
    draw_text(ROW_STATUS, 0, info, ATTR_STATUSBAR, SCREEN_COLS - 2 - clen);
    draw_text(ROW_STATUS, SCREEN_COLS - clen, conn, ATTR_STATUSBAR, clen);
}

static void draw_cmdline(void)
{
    char line[PANEL_HEADER_MAX + 4];
    fill_rect(ROW_CMDLINE, 0, 1, SCREEN_COLS, ' ', ATTR_PANEL);
    if (g_active) {
        sprintf(line, "%s>", g_active->title());
        draw_text(ROW_CMDLINE, 0, line, ATTR_PANEL, SCREEN_COLS - 1);
    }
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
    draw_menubar();
    g_left.draw();
    g_right.draw();
    draw_statusbar();
    draw_cmdline();
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
        dlg_error("FTP nicht verfuegbar",
                  "TCP/IP konnte nicht gestartet werden.\n"
                  "MTCPCFG pruefen und Programm neu starten.");
        redraw_all();
        return;
    }

    /* Bereits verbunden -> Trennen anbieten. */
    if (g_ftp.is_connected()) {
        if (dlg_confirm("Trennen", "Bestehende FTP-Verbindung trennen?")) {
            g_ftp.disconnect();
            g_right.refresh();
        }
        redraw_all();
        return;
    }

    if (!dlg_input("FTP-Verbindung", "Host:",     host,    FTP_HOST_MAX - 1, 0)) { redraw_all(); return; }
    if (host[0] == '\0')                                                         { redraw_all(); return; }
    if (!dlg_input("FTP-Verbindung", "Port:",     portStr, 6,                0)) { redraw_all(); return; }
    if (!dlg_input("FTP-Verbindung", "Benutzer:", user,    38,               0)) { redraw_all(); return; }
    if (!dlg_input("FTP-Verbindung", "Passwort:", pass,    38,               1)) { redraw_all(); return; }

    port = (unsigned)atoi(portStr);
    if (port == 0) port = 21;

    redraw_all();
    flash_status(" Verbinde mit FTP-Server ...");

    rc = g_ftp.connect(host, port, user, pass);
    if (rc != FTP_OK) {
        dlg_error("Verbindung fehlgeschlagen", g_ftp.last_error());
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
        dlg_error("Kopieren", "Keine FTP-Verbindung.\nMit F2 zuerst verbinden.");
        redraw_all();
        return;
    }

    e = g_active->selected();
    if (e == 0 || e->is_parent) { redraw_all(); return; }
    if (e->is_dir) {
        dlg_error("Kopieren", "Nur einzelne Dateien koennen kopiert\nwerden, keine Verzeichnisse.");
        redraw_all();
        return;
    }

    if (g_active == (Panel *)&g_right) {
        /* --- Download: Remote -> lokal --- */
        join_local(target, (int)sizeof(target), g_left.path(), e->name);
        sprintf(prompt, "\"%.20s\" laden nach:", e->name);
        if (!dlg_input("Download", prompt, target, (int)sizeof(target) - 1, 0)) { redraw_all(); return; }
        if (target[0] == '\0') { redraw_all(); return; }

        redraw_all();
        dlg_progress_begin("Download", e->name);
        rc = g_ftp.retr(e->name, target, copy_progress, 0);
        dlg_progress_end();

        if (rc != FTP_OK) dlg_error("Download fehlgeschlagen", g_ftp.last_error());
        g_left.refresh();   /* neue lokale Datei sichtbar machen */
    } else {
        /* --- Upload: lokal -> Remote --- */
        char localpath[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        join_local(localpath, (int)sizeof(localpath), g_left.path(), e->name);
        strncpy(target, e->name, sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
        sprintf(prompt, "\"%.20s\" senden als:", e->name);
        if (!dlg_input("Upload", prompt, target, (int)sizeof(target) - 1, 0)) { redraw_all(); return; }
        if (target[0] == '\0') { redraw_all(); return; }

        redraw_all();
        dlg_progress_begin("Upload", e->name);
        rc = g_ftp.stor(localpath, target, copy_progress, 0);
        dlg_progress_end();

        if (rc != FTP_OK) dlg_error("Upload fehlgeschlagen", g_ftp.last_error());
        g_right.refresh();  /* neue Remote-Datei sichtbar machen */
    }

    redraw_all();
}

/* -------------------------------------------------------------------------
 * Main / Event-Loop
 * ---------------------------------------------------------------------- */
int main(void)
{
    int running = 1;

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
            draw_cmdline();
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
                draw_cmdline();
                if (g_active == (Panel *)&g_right && g_right.nav_failed())
                    flash_status(g_right.last_error());
            }
            /* sonst: Datei - Anzeigen/Starten folgt in spaeteren Schritten */
            break;

        case KEY_BACKSP:
            g_active->go_parent();
            g_active->draw();
            draw_statusbar();
            draw_cmdline();
            if (g_active == (Panel *)&g_right && g_right.nav_failed())
                flash_status(g_right.last_error());
            break;

        /* Funktionstasten. */
        case KEY_F1:
            dlg_message("Hilfe",
                "Tab        Panel wechseln\n"
                "Pfeile     Auswahl bewegen\n"
                "Bild auf/ab  Seitenweise\n"
                "Enter      Verzeichnis betreten\n"
                "Backspace  uebergeordnet\n"
                "F2 Verbinden  F5 Kopieren  F8 Loeschen\n"
                "F10        Beenden", 0);
            break;
        case KEY_F2:  do_connect(); break;
        case KEY_F3:  flash_status(" F3  Anzeigen - folgt"); break;
        case KEY_F4:  flash_status(" F4  Bearbeiten - folgt"); break;
        case KEY_F5:  do_copy(); break;
        case KEY_F6:  flash_status(" F6  Umbenennen - folgt"); break;
        case KEY_F7:  flash_status(" F7  Verzeichnis erstellen - folgt"); break;
        case KEY_F8:  flash_status(" F8  Loeschen - folgt"); break;
        case KEY_F9:  flash_status(" F9  Menue - folgt"); break;

        case KEY_F10:
            if (dlg_confirm("Beenden", "NCFTP386 wirklich beenden?"))
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
