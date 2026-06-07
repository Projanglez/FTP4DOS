/* =============================================================================
 * dircopy.h - Rekursives Kopieren ganzer Verzeichnisbaeume
 * -----------------------------------------------------------------------------
 * Kopiert Verzeichnisse inkl. aller Unterverzeichnisse zwischen lokalem
 * Dateisystem und FTP-Server. Beide Richtungen arbeiten pfadbasiert (keine
 * CWD-Zustandsaenderung auf dem Server), damit der Remote-Arbeitsstand nach
 * dem Kopieren unveraendert bleibt.
 *
 *   Upload   : lokales Verzeichnis -> Remote (MKD + STOR, rekursiv)
 *   Download : Remote-Verzeichnis  -> lokal  (mkdir + RETR, rekursiv)
 *
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#ifndef DIRCOPY_H
#define DIRCOPY_H

#include "ftpcli.h"

/* Wird vor jeder Datei / jedem Verzeichnis aufgerufen (UI: aktuellen Namen
 * anzeigen). is_dir != 0 => es handelt sich um ein Verzeichnis. */
typedef void (*DirCopyItemCb)(void *ctx, const char *name, int is_dir);

/* Entscheidung des Konflikt-Callbacks, wenn die Zieldatei bereits existiert. */
#define DC_OVERWRITE 0   /* diese Datei ueberschreiben     */
#define DC_SKIP      1   /* diese Datei ueberspringen      */
#define DC_ABORT     2   /* gesamten Vorgang abbrechen     */

/* Wird aufgerufen, wenn eine Zieldatei bereits existiert. 'name' ist der
 * Dateiname. Rueckgabe: DC_OVERWRITE / DC_SKIP / DC_ABORT. Darf 0 sein
 * (dann wird immer ueberschrieben). */
typedef int (*DirCopyConflictCb)(void *ctx, const char *name);

/* Rekursiver Upload: lokales Verzeichnis 'localDir' (vollstaendiger Pfad)
 * als 'remoteName' im aktuellen Remote-Arbeitsverzeichnis anlegen und den
 * gesamten Inhalt hochladen. Rueckgabe FTP_OK, FTP_ERR_ABORT (Benutzerabbruch)
 * oder ein anderer FTP_ERR_* Code. */
int dircopy_upload(FtpClient *ftp, const char *localDir, const char *remoteName,
                   DirCopyItemCb itemcb, FtpProgressCb progcb,
                   DirCopyConflictCb conflictcb, void *ctx);

/* Rekursiver Download: Remote-Verzeichnis 'remotePath' (relativ zum aktuellen
 * Remote-Verzeichnis) nach 'localDir' (vollstaendiger lokaler Pfad) holen. */
int dircopy_download(FtpClient *ftp, const char *remotePath, const char *localDir,
                     DirCopyItemCb itemcb, FtpProgressCb progcb,
                     DirCopyConflictCb conflictcb, void *ctx);

/* Baum-Zaehlung fuer die Loesch-Warnung. Zaehlt rekursiv ALLE Dateien und
 * Verzeichnisse unterhalb von 'path' UND das Wurzelverzeichnis selbst; die
 * Werte werden zu *nfiles / *ndirs ADDIERT (vom Aufrufer mit 0 vorbelegen).
 * Rueckgabe FTP_OK oder ein FTP_ERR_* Code. */
int dircopy_count_local (const char *path, unsigned *nfiles, unsigned *ndirs);
int dircopy_count_remote(FtpClient *ftp, const char *path,
                         unsigned *nfiles, unsigned *ndirs);

/* Rekursiv loeschen (inkl. aller Unterverzeichnisse und 'path' selbst). */
int dircopy_delete_local (const char *path, DirCopyItemCb itemcb, void *ctx);
int dircopy_delete_remote(FtpClient *ftp, const char *path,
                          DirCopyItemCb itemcb, void *ctx);

#endif /* DIRCOPY_H */
