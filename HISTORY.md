# FTP4DOS — Release History

The road from the first public pre-release to version 1.0, in chronological
order. All releases are available at
<https://github.com/Projanglez/ftp4dos/releases>; every release ships the
DOS executable plus the exact mTCP sources it links against (GPLv3 §6).

## v0.9.0 — 2026-06-09 (first public release, as "NCFTP386")

Norton Commander-style dual-pane FTP client for MS-DOS on real x86
hardware, built with Open Watcom and mTCP.

- Two panes: local (DOS) and remote (FTP, passive mode)
- Copy in both directions (F5), including recursive directory trees
- Multiple selection with the Ins key (Norton style) for copy/delete
- Create (F7), rename (F6), recursive delete with pre-count confirmation (F8)
- File viewer (F3, up to 32 KB; remote files download to a temp file first)
  and a minimal local text editor (F4)
- Connection keepalive (NOOP every 60 s)
- Bilingual German/English UI, auto-detected from the DOS country setting
- Tested on real 386 hardware with a genuine LAN card

## v0.9.1 — 2026-06-17

- Migrated from the retrohun mTCP fork to the **official mTCP 2025-01-10**
  release (brutman.com, GPLv3)
- All source code comments translated to English

## v0.9.2 — 2026-06-21

- **Renamed NCFTP386 → FTP4DOS** (thanks to Yoghoo on VOGONS for the
  suggestion)
- Swap panes with Ctrl+U (remembered across launches)
- Move with F6 (copy + delete source, recursive); rename moved to Alt+F6
- FTP pane preserves the server's original (case-sensitive) file names
- Monochrome (MDA/Hercules) support; `/MONO` and `/COLOR` overrides
- ALT command bar (Norton-style): Alt+F1 Drive, Alt+F6 Rename
- Confirmation dialogs default to Yes

## v0.9.4 — 2026-06-22

- Live transfer telemetry: current/average speed, per-file and batch ETA
- Pause (P) and cancel (ESC) during transfers
- Copy/Move/Delete confirmations show recursive counts and total size
- Alt+F9: file checksums (CRC32 + MD5), local and remote
- Alt+F3: configurable per-pane sorting
- Compact M/G size display; locale-aware number/date/time formatting
- Comfortable input fields everywhere (cursor movement, Home/End, Del,
  mid-line insert)

## v0.9.4a — 2026-06-24

- Alt+F2 "Detail": full (untruncated) name and exact size of an entry
- Alt+F5 / F9: refresh the active pane
- No more `..` entry at the FTP root
- Consistent 1024-based KB/MB/GB sizes throughout

## v0.9.5 — 2026-06-25

- **Site manager**: multiple named connection profiles in `FTP4DOS.SIT`,
  reached via [Manage...] in the connect dialog
- **`/EXMEM`**: store large remote listings (thousands of entries) in
  extended (XMS) or expanded (EMS) memory instead of the 512-entry default
- Search / jump-to-name (Alt+F7 / Ctrl+F)
- **Long remote file names** kept in full and used for transfers
- Full-screen pane toggle (Alt+F8)
- FTP start directory (connect dialog and `/D:DIR`); remembered sort order
- Fix: entering remote directories with names longer than 39 characters

## v0.9.5a — 2026-06-26

- Fix: downloading files with multi-dot / long names (e.g. `apack-1.00.zip`)
  maps to a valid DOS 8.3 target instead of failing
- Ctrl+C as shortcut for "compare panes" (BIOS keyboard read, no more
  stray `^C`)
- Date and Time sorting merged into one Date/Time criterion

## v0.9.6 — 2026-07-10

Driven by feedback in the VOGONS thread — thanks to mbbrutman, ntalaec,
Falcosoft, Grzyb and fly_indiz.

- **Faster transfers**: downloads write to disk in large buffered blocks
  instead of many small per-packet writes; buffer sizes tunable via
  `FTP4DOS_TCP_BUFFER` / `FTP4DOS_FILE_BUFFER` in `MTCP.CFG` (the mTCP FTP
  client's `FTP_TCP_BUFFER` / `FTP_FILE_BUFFER` are read as fallbacks)
- **UTF-8 file names** (RFC 2640): converted to the active DOS codepage
  (CP437, CP850/858, CP866; `FTP4DOS_CODEPAGE` override) for display and
  local names; uploads to UTF-8 servers are encoded back (`OPTS UTF8 ON`)
- **Long file names (LFN) in the local pane** on Windows 9x DOS,
  MS-DOS 7.x, or DOSLFN; includes a fix for a false-positive LFN detection
  on MS-DOS 6.22
- mTCP is now referenced as a **git submodule** of the official repository
  <https://github.com/mbbrutman/mTCP>, pinned to the 2025-01-10 release tag

## v1.0 — upcoming

The first stable release. Final scope to be determined.
