/* =============================================================================
 * dialog.h - Modale Dialoge fuer NCFTP386
 * -----------------------------------------------------------------------------
 * Jeder Dialog zeichnet sich zentriert ueber den aktuellen Bildschirm, faehrt
 * seine eigene Tastaturschleife und stellt den Bildschirm beim Schliessen
 * wieder her - der Aufrufer muss NICHT neu zeichnen.
 *
 * Mehrzeilige Nachrichten: Zeilen mit '\n' trennen (max. DLG_MAX_LINES).
 * ===========================================================================*/
#ifndef DIALOG_H
#define DIALOG_H

#define DLG_MAX_LINES 8     /* max. Textzeilen in Message-/Confirm-Dialogen */

/* Hinweis-/Fehlerbox mit einem [ OK ]-Knopf. is_error!=0 -> rote Darstellung.
 * Schliesst bei Enter, Esc oder Leertaste. */
void dlg_message(const char *title, const char *msg, int is_error);
void dlg_error(const char *title, const char *msg);   /* = dlg_message(...,1)  */

/* Ja/Nein-Abfrage. Rueckgabe: 1 = Ja, 0 = Nein.
 * J/N als Direkttasten, Links/Rechts/Tab wechselt, Enter waehlt, Esc = Nein. */
int dlg_confirm(const char *title, const char *msg);

/* Einzeiliges Eingabefeld (Basis fuer den Verbindungsdialog).
 * buf wird vorbefuellt angezeigt und editiert; maxlen = max. Zeichen (buf muss
 * maxlen+1 gross sein). is_password!=0 zeigt '*'. Rueckgabe: 1 = OK, 0 = Abbruch. */
int dlg_input(const char *title, const char *prompt,
              char *buf, int maxlen, int is_password);

/* Nicht-modaler Fortschrittsdialog fuer Datentransfers (F5).
 * Ablauf: begin() oeffnet die Box (sichert den Bildschirm), update() wird
 * waehrend des blockierenden Transfers aus dem FtpProgressCb aufgerufen,
 * end() schliesst die Box und stellt den Bildschirm wieder her.
 *   total >  0 : Fortschrittsbalken mit Prozentanzeige
 *   total == 0 : Groesse unbekannt -> nur uebertragene Bytes
 * update() zeichnet aus Effizienzgruenden nur bei sichtbarer Aenderung neu. */
void dlg_progress_begin(const char *title, const char *fromname);
void dlg_progress_update(unsigned long sofar, unsigned long total);
void dlg_progress_end(void);

#endif /* DIALOG_H */
