/* =============================================================================
 * dialog.cpp - Modale Dialoge
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#include <string.h>
#include <stdio.h>

#include "dialog.h"
#include "tui.h"
#include "keymap.h"

/* Puffer fuer den gesicherten Bildschirm hinter dem Dialog (4000 Bytes). */
static unsigned char dlg_screen[SCREEN_CELLS * 2];

#define DLG_SHADOW 0x08   /* Attribut der Schlagschatten-Zellen (dunkel) */

/* -------------------------------------------------------------------------
 * Hilfsfunktionen
 * ---------------------------------------------------------------------- */

/* Nachricht an '\n' in Zeilen zerlegen. Rueckgabe: Zeilenzahl (>=1). */
static int split_lines(const char *msg, char lines[][72])
{
    int nl = 0, ci = 0;
    const char *p = msg ? msg : "";

    lines[0][0] = '\0';
    while (*p && nl < DLG_MAX_LINES) {
        if (*p == '\n') {
            lines[nl][ci] = '\0';
            nl++;
            ci = 0;
            if (nl < DLG_MAX_LINES) lines[nl][0] = '\0';
        } else {
            if (ci < 71) lines[nl][ci++] = *p;
        }
        p++;
    }
    if (nl < DLG_MAX_LINES) {
        lines[nl][ci] = '\0';
        nl++;
    }
    return nl;
}

/* Schlagschatten rechts und unten am Dialog (Attribut-only). */
static void draw_shadow(int top, int left, int rows, int cols)
{
    int r, c;
    for (r = top + 1; r <= top + rows; r++)
        putattr_at(r, left + cols, DLG_SHADOW);
    for (c = left + 1; c <= left + cols; c++)
        putattr_at(top + rows, c, DLG_SHADOW);
}

/* Rahmen + Fuellung + zentrierter Titel auf der oberen Rahmenlinie. */
static void draw_dialog_frame(int top, int left, int rows, int cols,
                              const char *title, unsigned char bg)
{
    fill_rect(top, left, rows, cols, ' ', bg);
    draw_box(top, left, rows, cols, bg, 1);
    draw_shadow(top, left, rows, cols);

    if (title && *title) {
        int tl = (int)strlen(title);
        int tcol;
        if (tl > cols - 4) tl = cols - 4;
        tcol = left + (cols - tl) / 2;
        putchar_at(top, tcol - 1, ' ', bg);
        draw_text(top, tcol, title, bg, tl);
        putchar_at(top, tcol + tl, ' ', bg);
    }
}

/* Knopf "[ label ]" zeichnen; focused -> hervorgehoben. */
static void draw_button(int row, int col, const char *label, int focused)
{
    char tmp[40];
    unsigned char a = focused ? ATTR_DIALOG_HL : ATTR_DIALOG_BG;
    sprintf(tmp, "[ %s ]", label);
    draw_text(row, col, tmp, a, (int)strlen(tmp));
}

static int button_width(const char *label)
{
    return (int)strlen(label) + 4;   /* "[ " + label + " ]" */
}

/* Breite/Position eines zentrierten Dialogs aus Inhaltsbreite ableiten. */
static int clamp_cols(int content_w)
{
    int cols = content_w + 4;        /* 2 Rahmen + je 1 Innenrand */
    if (cols > 76) cols = 76;
    if (cols < 12) cols = 12;
    return cols;
}

/* -------------------------------------------------------------------------
 * Hinweis-/Fehlerbox
 * ---------------------------------------------------------------------- */
void dlg_message(const char *title, const char *msg, int is_error)
{
    char lines[DLG_MAX_LINES][72];
    int nlines = split_lines(msg, lines);
    int w = 0, i, cols, rows, top, left, bcol;
    unsigned char bg = is_error ? ATTR_ERROR : ATTR_DIALOG_BG;
    static const char *lbl = "OK";

    if (title) { int t = (int)strlen(title); if (t > w) w = t; }
    for (i = 0; i < nlines; i++) {
        int l = (int)strlen(lines[i]);
        if (l > w) w = l;
    }
    if (button_width(lbl) > w) w = button_width(lbl);

    cols = clamp_cols(w);
    rows = nlines + 4;               /* Rahmen + Zeilen + Leerzeile + Knopf + Rahmen */
    top  = (SCREEN_ROWS - rows) / 2;
    left = (SCREEN_COLS - cols) / 2;

    save_screen(dlg_screen);
    draw_dialog_frame(top, left, rows, cols, title, bg);
    for (i = 0; i < nlines; i++)
        draw_text(top + 1 + i, left + 2, lines[i], bg, cols - 4);

    bcol = left + (cols - button_width(lbl)) / 2;
    draw_button(top + rows - 2, bcol, lbl, 1);

    for (;;) {
        int k = readkey();
        if (k == KEY_ENTER || k == KEY_ESC || k == KEY_SPACE)
            break;
    }
    restore_screen(dlg_screen);
}

void dlg_error(const char *title, const char *msg)
{
    dlg_message(title, msg, 1);
}

/* -------------------------------------------------------------------------
 * Ja/Nein-Abfrage
 * ---------------------------------------------------------------------- */
int dlg_confirm(const char *title, const char *msg)
{
    char lines[DLG_MAX_LINES][72];
    int nlines = split_lines(msg, lines);
    int w = 0, i, cols, rows, top, left;
    int btnrow, bw_ja, bw_nein, gap = 3, btotal, b0;
    int focus = 1;                   /* Default: "Nein" (sicherer) */
    int result;
    unsigned char bg = ATTR_DIALOG_BG;

    bw_ja   = button_width("Ja");
    bw_nein = button_width("Nein");
    btotal  = bw_ja + gap + bw_nein;

    if (title) { int t = (int)strlen(title); if (t > w) w = t; }
    for (i = 0; i < nlines; i++) {
        int l = (int)strlen(lines[i]);
        if (l > w) w = l;
    }
    if (btotal > w) w = btotal;

    cols   = clamp_cols(w);
    rows   = nlines + 4;
    top    = (SCREEN_ROWS - rows) / 2;
    left   = (SCREEN_COLS - cols) / 2;
    btnrow = top + rows - 2;
    b0     = left + (cols - btotal) / 2;

    save_screen(dlg_screen);
    draw_dialog_frame(top, left, rows, cols, title, bg);
    for (i = 0; i < nlines; i++)
        draw_text(top + 1 + i, left + 2, lines[i], bg, cols - 4);

    for (;;) {
        int k;
        draw_button(btnrow, b0, "Ja", focus == 0);
        draw_button(btnrow, b0 + bw_ja + gap, "Nein", focus == 1);

        k = readkey();
        if (k == KEY_LEFT || k == KEY_RIGHT || k == KEY_TAB) {
            focus = !focus;
        } else if (k == 'j' || k == 'J') {
            result = 1; break;
        } else if (k == 'n' || k == 'N' || k == KEY_ESC) {
            result = 0; break;
        } else if (k == KEY_ENTER) {
            result = (focus == 0) ? 1 : 0; break;
        }
    }
    restore_screen(dlg_screen);
    return result;
}

/* -------------------------------------------------------------------------
 * Einzeiliges Eingabefeld
 * ---------------------------------------------------------------------- */
int dlg_input(const char *title, const char *prompt,
              char *buf, int maxlen, int is_password)
{
    static const char *hint = "[Enter] OK   [Esc] Abbruch";
    int promptlen = prompt ? (int)strlen(prompt) : 0;
    int hintlen   = (int)strlen(hint);
    int fieldw, w, cols, rows, top, left, frow, fcol;
    int len, result;
    unsigned char bg = ATTR_DIALOG_BG;

    fieldw = maxlen;
    if (fieldw > 50) fieldw = 50;
    if (fieldw < 8)  fieldw = 8;

    w = fieldw;
    if (promptlen > w) w = promptlen;
    if (hintlen   > w) w = hintlen;
    if (title) { int t = (int)strlen(title); if (t > w) w = t; }

    cols = clamp_cols(w);
    rows = 6;                        /* Rahmen, Prompt, Feld, Leer, Hinweis, Rahmen */
    top  = (SCREEN_ROWS - rows) / 2;
    left = (SCREEN_COLS - cols) / 2;
    frow = top + 2;
    fcol = left + 2;

    len = (int)strlen(buf);
    if (len > maxlen) { len = maxlen; buf[len] = '\0'; }

    save_screen(dlg_screen);
    draw_dialog_frame(top, left, rows, cols, title, bg);
    if (prompt) draw_text(top + 1, left + 2, prompt, bg, cols - 4);
    draw_text(top + 4, left + 2, hint, bg, cols - 4);

    show_cursor(1);
    for (;;) {
        int caret = len;
        int start = (caret > fieldw - 1) ? (caret - (fieldw - 1)) : 0;
        int vis   = len - start;
        int i, k;

        if (vis > fieldw) vis = fieldw;
        fill_rect(frow, fcol, 1, fieldw, ' ', ATTR_DIALOG_HL);
        for (i = 0; i < vis; i++) {
            char c = is_password ? '*' : buf[start + i];
            putchar_at(frow, fcol + i, c, ATTR_DIALOG_HL);
        }
        set_cursor(frow, fcol + (caret - start));

        k = readkey();
        if (k == KEY_ENTER) { result = 1; break; }
        if (k == KEY_ESC)   { result = 0; break; }
        if (k == KEY_BACKSP) {
            if (len > 0) { len--; buf[len] = '\0'; }
            continue;
        }
        if (k >= 0x20 && k <= 0x7E && len < maxlen) {
            buf[len++] = (char)k;
            buf[len] = '\0';
        }
    }
    show_cursor(0);
    restore_screen(dlg_screen);
    return result;
}

/* -------------------------------------------------------------------------
 * Fortschrittsdialog (nicht-modal, per Callback aktualisiert)
 * -----------------------------------------------------------------------------
 * Teilt sich den Sicherungspuffer dlg_screen mit den uebrigen Dialogen: der
 * Fortschrittsdialog ist nie gleichzeitig mit einem anderen offen (dlg_input
 * schliesst vor dem Transfer, dlg_error oeffnet erst danach).
 * ---------------------------------------------------------------------- */
static int           prog_active = 0;
static int           prog_top, prog_left, prog_cols, prog_rows;
static int           prog_barrow, prog_barw;
static long          prog_lastpct;     /* zuletzt gezeichnete %, -1 = noch nie  */
static unsigned long prog_lastunit;    /* zuletzt gezeichnete 8-KB-Einheit       */

/* Balken "[####....] NNN%" in die Bar-Zeile zeichnen (0xDB voll, 0xB0 leer). */
static void prog_draw_bar(long pct)
{
    char bar[84];
    int  i, o = 0, fill;

    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    fill = (int)((long)prog_barw * pct / 100);
    if (fill > prog_barw) fill = prog_barw;

    bar[o++] = '[';
    for (i = 0; i < prog_barw; i++)
        bar[o++] = (char)((i < fill) ? 0xDB : 0xB0);
    bar[o++] = ']';
    o += sprintf(bar + o, " %3ld%%", pct);
    bar[o] = '\0';
    draw_text(prog_barrow, prog_left + 2, bar, ATTR_DIALOG_BG, prog_cols - 4);
}

void dlg_progress_begin(const char *title, const char *fromname)
{
    char buf[80];
    unsigned char bg = ATTR_DIALOG_BG;

    prog_cols = 50;
    if (prog_cols > SCREEN_COLS - 2) prog_cols = SCREEN_COLS - 2;
    prog_rows = 4;                          /* Rahmen, Dateizeile, Balken, Rahmen */
    prog_top  = (SCREEN_ROWS - prog_rows) / 2;
    prog_left = (SCREEN_COLS - prog_cols) / 2;
    prog_barrow = prog_top + 2;
    prog_barw   = (prog_cols - 4) - 7;      /* Rest fuer "[" "]" " 100%"           */
    if (prog_barw < 4) prog_barw = 4;
    prog_lastpct  = -1;
    prog_lastunit = (unsigned long)-1L;
    prog_active   = 1;

    save_screen(dlg_screen);
    draw_dialog_frame(prog_top, prog_left, prog_rows, prog_cols, title, bg);

    sprintf(buf, "Datei: %.34s", fromname ? fromname : "");
    draw_text(prog_top + 1, prog_left + 2, buf, bg, prog_cols - 4);
    prog_draw_bar(0);
}

void dlg_progress_update(unsigned long sofar, unsigned long total)
{
    if (!prog_active) return;

    if (total > 0) {
        /* Prozent berechnen, ueberlaufsicher fuer grosse Dateien. */
        unsigned long pct;
        if (sofar >= total)          pct = 100;
        else if (total > 42000000UL) pct = sofar / (total / 100UL);
        else                         pct = (sofar * 100UL) / total;

        if ((long)pct == prog_lastpct) return;   /* nur bei Aenderung neu zeichnen */
        prog_lastpct = (long)pct;
        prog_draw_bar((long)pct);
    } else {
        /* Unbekannte Groesse: uebertragene Bytes, alle 8 KB aktualisieren. */
        char buf[80];
        unsigned long unit = sofar >> 13;
        if (unit == prog_lastunit) return;
        prog_lastunit = unit;
        sprintf(buf, "%lu Bytes uebertragen ...", sofar);
        fill_rect(prog_barrow, prog_left + 2, 1, prog_cols - 4, ' ', ATTR_DIALOG_BG);
        draw_text(prog_barrow, prog_left + 2, buf, ATTR_DIALOG_BG, prog_cols - 4);
    }
}

void dlg_progress_end(void)
{
    if (!prog_active) return;
    restore_screen(dlg_screen);
    prog_active = 0;
}
