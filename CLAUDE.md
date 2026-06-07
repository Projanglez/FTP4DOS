# NCFTP386 — Norton Commander FTP Client for DOS/386

## Ziel
Dualer Dateimanager fuer MS-DOS 6.22 auf echter 386-Hardware mit Norton-Commander-Look.
- **Linkes Panel**: Lokales DOS-Dateisystem
- **Rechtes Panel**: FTP-Remote ueber mTCP TCP/IP-Stack
- Tastaturgesteuert, kein Maus-Support noetig

## mTCP-Quelle
GitHub-Fork: https://github.com/retrohun/mTCP (GPLv3, Version 2015-10-12)
Klonen mit: git clone https://github.com/retrohun/mTCP mtcp

Verzeichnisstruktur des geklonten Repos:
```
mtcp/
  TCPLIB/    <- TCP/IP Bibliotheks-Quellen (.CPP)
  TCPINC/    <- TCP/IP Header-Dateien
  INCLUDE/   <- App-uebergreifende Includes
  APPS/FTP/  <- Referenz-FTP-Client (zum Nachschlagen der mTCP-API!)
```

## Build-Umgebung
- Compiler: Open Watcom C/C++ (wmake + wpp fuer C++, wcc fuer C)
- Target: 16-bit Real-Mode DOS, Large Memory Model
- CPU: 386 (-3 Flag)
- Windows-Host fuer Cross-Compilation

## Compiler-Flags
```
CFLAGS = -ml -3 -zu -s -ox -bt=dos -zp1 -ei
INCPATH = -I./src -I./mtcp/TCPINC -I./mtcp/INCLUDE
```
| Flag | Bedeutung |
|------|-----------|
| `-ml` | Large memory model |
| `-3` | 386-Instruktionssatz |
| `-zu` | SS != DS (zwingend fuer mTCP!) |
| `-s` | Kein Stack-Overflow-Check |
| `-ox` | Maximale Optimierung |
| `-bt=dos` | DOS-Target |
| `-zp1` | Byte-Alignment in Structs |
| `-ei` | enum als int |

## Verzeichnisstruktur (unser Projekt)
```
NCFTP386/
+-- MAKEFILE              Open Watcom wmake
+-- CLAUDE.md             Diese Datei
+-- src/
|   +-- ncftp.cpp         Main + Event-Loop
|   +-- tui.h / tui.cpp   TUI-Engine: Videospeicher, Farben, Box-Drawing
|   +-- panel.h / panel.cpp   Abstrakte Panel-Basisklasse
|   +-- lpanel.h / lpanel.cpp Lokales Dateisystem-Panel
|   +-- rpanel.h / rpanel.cpp FTP-Remote-Panel
|   +-- ftpcli.h / ftpcli.cpp FTP-Protokoll-Statemachine ueber mTCP
|   +-- dialog.h / dialog.cpp Modale Dialoge (Connect, Progress, Fehler)
|   +-- keymap.h          Key-Codes und Action-Enum
+-- mtcp/                 git clone https://github.com/retrohun/mTCP mtcp
    +-- TCPLIB/           TCP/IP Bibliotheks-Quellen
    +-- TCPINC/           Header-Dateien
    +-- INCLUDE/          Gemeinsame Includes
    +-- APPS/FTP/         Referenz-FTP-Client
```

## Screen-Layout (80x25 Text-Mode)
```
Zeile  0:  [Menue]  " Left   Files   Commands   Options   Right "
Zeilen 1-21: Zwei Panels (je 40 Spalten breit)
             Linkes Panel (Cols 0-39)        Rechtes Panel (Cols 40-79)
             +=====================+        +=====================+
             | C:\GAMES            |        | ftp.host.de /pub    |
             +=====================+        +=====================+
             | Name      Groesse D |        | Name      Groesse D |
             | ..                  |        | ..                  |
             | ULTIMA    <DIR>  07 |        | games     <DIR>  01 |
             |*file.txt   1234  07 |        | readme.txt 512   01 |
             +=====================+        +=====================+
Zeile 22:  [Statusleiste: Dateiinfo + Verbindungsstatus]
Zeile 23:  [Kommandozeile]
Zeile 24:  [F1Hilfe F2Verb F3Anzeig F4Edit F5Kopier F6Umben F7Neu F8Losch F9Menue F10Ende]
```

## Farbschema (DOS-Attribute-Bytes)
```c
#define ATTR_PANEL      0x1F  /* weiss auf blau  (Panelinhalt)      */
#define ATTR_SELECTED   0x30  /* schwarz auf cyan (Cursor-Zeile)    */
#define ATTR_HEADER     0x30  /* schwarz auf cyan (Pfad-Header)     */
#define ATTR_BORDER     0x1F  /* weiss auf blau  (Rahmen)           */
#define ATTR_MENUBAR    0x70  /* schwarz auf hellgrau               */
#define ATTR_STATUSBAR  0x70  /* schwarz auf hellgrau               */
#define ATTR_FNKEY_NUM  0x30  /* schwarz auf cyan (F1, F2, ...)    */
#define ATTR_FNKEY_LBL  0x70  /* schwarz auf hellgrau (Label)      */
#define ATTR_DIALOG_BG  0x70  /* Dialog-Hintergrund                */
#define ATTR_DIALOG_HL  0x0F  /* Dialog-Highlight                  */
#define ATTR_ERROR      0x4F  /* weiss auf rot (Fehlerdialoge)     */
```

## Videospeicher-Zugriff (tui.cpp)
```cpp
/* Direktzugriff auf CGA-Textspeicher 0xB800:0000 */
/* Im Large Model: far-Pointer verwenden           */
#include <dos.h>
#define VIDEO_SEG 0xB800U
static unsigned char far *vidmem = (unsigned char far *)MK_FP(VIDEO_SEG, 0);

void putchar_at(int row, int col, char ch, unsigned char attr) {
    int offset = (row * 80 + col) * 2;
    vidmem[offset]     = (unsigned char)ch;
    vidmem[offset + 1] = attr;
}
void fill_rect(int row, int col, int rows, int cols, char ch, unsigned char attr);
void draw_box(int row, int col, int rows, int cols, unsigned char attr, int dbl);
void draw_text(int row, int col, const char far *text, unsigned char attr, int maxlen);
```

Box-Drawing-Zeichen (CP437):
- Doppelrahmen: = 0xCD, | 0xBA, Ecken: 0xC9 0xBB 0xC8 0xBC, T-Stuecke: 0xCC 0xB9 0xCB 0xCA
- Einfachrahmen: - 0xC4, | 0xB3, Ecken: 0xDA 0xBF 0xC0 0xD9

## Tastatur-Handling (keymap.h)
```cpp
int readkey() {
    int k = getch();
    if (k == 0) k = 0x100 | getch();  /* Extended key */
    return k;
}
#define KEY_UP      0x148
#define KEY_DOWN    0x150
#define KEY_LEFT    0x14B
#define KEY_RIGHT   0x14D
#define KEY_ENTER   0x0D
#define KEY_ESC     0x1B
#define KEY_TAB     0x09
#define KEY_F1      0x13B
#define KEY_F2      0x13C
#define KEY_F3      0x13D
#define KEY_F4      0x13E
#define KEY_F5      0x13F
#define KEY_F6      0x140
#define KEY_F7      0x141
#define KEY_F8      0x142
#define KEY_F9      0x143
#define KEY_F10     0x144
#define KEY_PGUP    0x149
#define KEY_PGDN    0x151
#define KEY_HOME    0x147
#define KEY_END     0x14F
#define KEY_BACKSP  0x08
#define KEY_INS     0x152
#define KEY_DEL     0x153
```

## mTCP-Integration
Schau zuerst in mtcp/APPS/FTP/ um die korrekte API-Verwendung zu verstehen!

### Initialisierung (in main, vor allem anderen)
```cpp
#include "tcp.h"
#include "utils.h"
#include "timer.h"

Utils::parseEnv();           /* MTCPCFG env-Variable lesen */
if (Tcp::init()) { /* Fehler */ }
Timer::init();
```

### Event-Loop: mTCP-Polling (zwingend in jeder Schleife!)
```cpp
PACKET_PROCESS_SINGLE;   /* Macro aus packet.h */
Arp::driveArp();
Tcp::drivePackets();
```

### TCP-Socket fuer FTP-Controlverbindung
```cpp
#include "tcpsockm.h"
TcpSocket *ctrl = TcpSocket::getSocket();
IpAddr serverIp;
Dns::resolve("ftp.example.com", serverIp, 1);
ctrl->connect(serverIp, 21, 5000);  /* 5s Timeout */
/* Warten bis ctrl->isConnectComplete() */
```

## FTP-Protokoll-Statemachine (ftpcli.cpp)
Zustaende:
  DISCONNECTED -> CONNECTING -> BANNER -> AUTH_USER -> AUTH_PASS -> IDLE
                                                                      |
                              DISCONNECTING <- ERROR <- LISTING/TRANSFERRING

Immer PASV-Modus verwenden (aktiver Modus funktioniert oft nicht durch NAT).

Verzeichnisinhalt abrufen:
1. PASV senden -> IP:Port aus Antwort parsen
2. Zweiten TcpSocket zu dieser IP:Port oeffnen
3. LIST auf Kontrollverbindung senden
4. Datenverbindung lesen bis EOF
5. Auf 226 Transfer complete warten

Datei herunterladen (RETR) / hochladen (STOR):
1. TYPE I (Binaer-Modus!) -> PASV -> Datenverbindung
2. RETR/STOR dateiname senden
3. Daten schreiben/lesen
4. Datenverbindung schliessen -> 226 abwarten

## LIST-Parsing
Unix-Format: "drwxr-xr-x  2 user group  4096 Jan  1 12:00 name"
Felder: permissions, links, owner, group, size, month, day, time/year, name
Erstes Zeichen 'd' = Verzeichnis, '-' = Datei.

## Lokales Panel (lpanel.cpp)
```cpp
#include <dos.h>
struct find_t ff;
unsigned attrib = _A_NORMAL | _A_SUBDIR | _A_RDONLY | _A_ARCH;
_dos_findfirst("*.*", attrib, &ff);
/* ff.name, ff.size, ff.wr_date, ff.wr_time, ff.attrib */
_dos_findnext(&ff);
```
Sortierung: ".." zuerst, dann [VERZEICHNISSE] alphabetisch, dann Dateien alphabetisch.

## Funktionstasten
| Taste | Funktion |
|-------|----------|
| F1    | Hilfe (Modal) |
| F2    | FTP-Verbindungsdialog |
| F3    | Anzeigen (lokal/remote) |
| F4    | Bearbeiten (nur lokal) |
| F5    | Kopieren (lokal<->remote, mit Fortschritt) |
| F6    | Umbenennen/Verschieben |
| F7    | Verzeichnis erstellen |
| F8    | Loeschen (mit Bestaetigung) |
| F9    | Menue |
| F10   | Beenden |
| Tab   | Aktives Panel wechseln |
| Enter | Verzeichnis betreten / Datei starten |
| Backsp| Uebergeordnetes Verzeichnis |
| PgUp/PgDn | Seitenweise scrollen |

## Verbindungsdialog
```
+================================+
|   FTP-Verbindung herstellen    |
+================================+
| Host: [ftp.example.com       ] |
| Port: [21  ]                   |
| User: [anonymous             ] |
| Pass: [**********************] |
+================================+
|    [Verbinden]   [Abbruch]     |
+================================+
```

## Fehlerbehandlung
- Alle FTP-Fehler: Nicht-fataler Modal-Dialog, Rueckkehr zu IDLE
- Netzwerk-Timeout: 30s Kontrollbefehle, 60s Datentransfer
- TcpSocket immer aufraumen: socket->close() + TcpSocket::freeSocket()
- Utils::endStack() bei Programmende aufrufen!

## Build & Ausfuehren
```bat
cd C:\NCFTP386
wmake

REM Auf der DOS-386:
SET MTCPCFG=C:\NET\MTCP.CFG
NCFTP.EXE
```

## Implementierungs-Reihenfolge
1. tui.cpp/h: putchar_at, fill_rect, draw_box, draw_text, clear_screen
2. lpanel.cpp/h: Lokales Verzeichnis lesen + anzeigen
3. ncftp.cpp: Hauptschleife + Tastaturnavigation (lokales Panel)
4. dialog.cpp/h: Einfache Modale (Fehler, Bestaetigung)
5. ftpcli.cpp/h: TCP-Verbindung + FTP-Login + LIST (Referenz: mtcp/APPS/FTP/)
6. rpanel.cpp/h: Remote-Listing anzeigen
7. F5 Copy: RETR (Download) mit Fortschrittsbalken
8. F5 Copy: STOR (Upload)
9. F7/F8: MKD, DELE
10. Polish: F6 Rename, Statusleiste, Fehlerbehandlung

## Wichtiger Hinweis zu mTCP-Quelldateien
Nach dem git clone: Schau zuerst in mtcp/APPS/FTP/makefile!
Dort steht die exakte Liste der zu kompilierenden TCPLIB-Dateien.
Passe MAKEFILE entsprechend an, falls Dateien fehlen oder anders heissen.
