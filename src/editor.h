/* =============================================================================
 * editor.h - Minimaler Vollbild-Texteditor (F4), nur lokale Dateien
 * -----------------------------------------------------------------------------
 * Laedt eine Textdatei (bis ca. 32 KB) in einen Zeilenpuffer im FAR-Heap und
 * erlaubt einfaches Editieren: Zeichen einfuegen/loeschen, Zeilen teilen/
 * verbinden, scrollen. Speichern mit F2, Beenden mit Esc (Rueckfrage bei
 * Aenderungen). Bewusst minimal: kein Undo, keine Suche, kein Remote-Edit.
 * ===========================================================================*/
#ifndef EDITOR_H
#define EDITOR_H

/* path  = vollstaendiger Pfad der (lokalen) Datei.
 * title = Anzeigename in der Kopfzeile (z.B. der reine Dateiname).
 * Rueckgabe: 1 = es wurde (mindestens einmal) gespeichert, sonst 0. */
int edit_file(const char *path, const char *title);

#endif /* EDITOR_H */
