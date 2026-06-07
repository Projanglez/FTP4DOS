/* =============================================================================
 * viewer.h - Einfacher Vollbild-Textbetrachter (F3)
 * -----------------------------------------------------------------------------
 * Zeigt eine Datei (bis ca. 60 KB) scrollbar an. Steuerung: Pfeile, Bild auf/ab,
 * Pos1/Ende, Links/Rechts (horizontal), Esc/Q beendet. Groessere Dateien werden
 * abgeschnitten ([gekuerzt]-Hinweis). Steuerzeichen werden als '.' dargestellt.
 * ===========================================================================*/
#ifndef VIEWER_H
#define VIEWER_H

/* path  = vollstaendiger Pfad der (lokalen) Datei.
 * title = Anzeigename in der Kopfzeile (z.B. der reine Dateiname). */
void view_file(const char *path, const char *title);

#endif /* VIEWER_H */
