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

## v1.0.0 — 2026-07-18

The first stable release, verified on real 386 hardware. Driven by the
v0.9.6 feedback round on VOGONS — thanks to fly_indiz and ntalaec.

- **Steadier download speeds**: disk writes no longer starve the network
  stack — the file buffer is flushed in slices with packet processing in
  between, avoiding the burst/stall sawtooth seen on real hardware
- **Cancelling a transfer is instant** instead of freezing the progress
  dialog for up to 10 seconds (ABOR is now sent while the data connection
  is still open, mirroring the upload path)
- **Control-connection resync**: stale replies left over from aborted or
  timed-out transfers no longer desync later commands (the reported
  "PASV refused (257)"); a server-side idle timeout (421) now cleanly
  reports "connection lost" instead of failing later
- **Long filenames actually work now** (the v0.9.6 LFN support was only
  ever exercised on plain DOS): detection uses the documented Get Volume
  Information call instead of the undocumented probe that only Windows 9x
  answers (fixes DOSLFN on MS-DOS 7.x), the 714Eh find-data record layout
  and attribute mask were wrong (garbage names, missing directories), and
  all file operations route through the LFN API or the 8.3 alias — enter
  long-named directories, view/copy/delete/rename long-named files,
  download to long names, create long-named directories
- **UTF-8 remote paths** are codepage-converted in the pane header (file
  lists already were); cursor reselection after leaving a UTF-8-named
  directory works
- **`/SITES` switch**: open the site manager directly on startup
- Deterministic 8.3 download names, Move+Skip no longer loses files,
  overflow guard for >4 GB counters

## v1.0.1 — 2026-07-29

A bug-fix and minor improvement release for the v1.0.0 feedback round on
VOGONS — thanks to Yoghoo, ntalaec, Grzyb and fly_indiz.

- **Downloads no longer fail or silently produce 0-byte files on systems
  with long filename support** (DOSBox-X, MS-DOS 7.x + DOSLFN, Windows 9x
  DOS): a long-named target was created through the DOS extended-open call
  and the handle handed to the C library, which does not accept a handle it
  did not open itself. The long name is now created first and all file I/O
  goes through its 8.3 alias
- **Directories with thousands of long file names work in full.** The name
  buffer was capped at one conventional-memory segment, so past roughly
  600-1700 entries names were silently shortened to a 39-character prefix —
  and that prefix went out verbatim in `RETR`/`CWD`/`DELE`, producing a
  "550 No such file" on entries plainly visible in the pane. Name storage
  now moves into XMS/EMS along with the entry records; a name that still
  does not fit is marked `>` and refused up front
- **Extended memory is used automatically** when available instead of
  requiring `/EXMEM` — but only when it actually yields more entries than
  the 512 of the conventional list; `/NOEXMEM` opts out entirely
- **Recursive copies are no longer limited to 400 entries per directory**
  (2048 now, stepping down only as far as free conventional memory
  requires; the message names the limit actually in force)
- **Cancelling a transfer completes in well under a second** (~0.9 s on a
  20 MB download) instead of freezing the progress dialog for up to 20
  seconds
- **Progress, speed and ETA keep moving during a recursive copy**: small
  files that arrived in one go never reported their bytes, freezing the
  display on the last large file. The estimate now also accounts for
  per-file overhead (`PASV`/`RETR`/`226` round trip, open/close, listing),
  and directory copies show the total remaining time, not just the
  per-file one
- **Real timestamps for remote files** via `MLSD` (RFC 3659), with fallback
  to `LIST`; `ls -l` omits the time of day for entries older than about six
  months, which is why archive directories showed `00:00` on every row.
  Where `LIST` is used, those entries now show the year. Note that `MLSD`
  reports UTC
- **`/LASTCON`**: connect straight to the last used connection on startup
  with no dialog — `FTP4DOS /Q /LASTCON` needs no batch file
- **Transfer diagnostics**: `FTP4DOS_XFERLOG` in `MTCP.CFG` writes one line
  per second during a transfer (elapsed, bytes total, bytes in the last
  second, receive/idle/disk-write counts, buffer fill); off by default
- The local pane preserves the case of long file names (the Norton
  uppercase/lowercase convention now applies to genuine 8.3 names only),
  and 8.3 names on the FTP side are shown lowercase to match
- Fixes: an empty directory on an LFN system could show no `..` entry; the
  name buffer could fail to allocate at all with little free conventional
  memory

## v1.1.0 — 2026-07-30

Automatic updates

- **Alt+F10 checks for a new version**, downloads it and installs it on exit.
  The previous executable is kept as `FTP4DOS.BAK`.
- **Every update is RSA-2048 signed.** The manifest carries the SHA-256 of the
  executable and is signed with a key whose private half never leaves the
  maintainer's machine; the public keys are compiled into FTP4DOS. A manifest
  that does not verify is refused outright — there is no override.
  This matters because the transport cannot be encrypted: mTCP has no TLS, so
  authenticity comes from the signature rather than from the connection.
- **Optional startup check**, off by default, at most once a week, and it never
  does more than write a line in the status bar. New switches `/UPDATECHECK`
  and `/NOUPDATECHECK`; the setting lives in `FTP4DOS.SAV` as `updcheck`.
- The update channel is a small Cloudflare Worker that reads this repository's
  latest GitHub release and serves it over plain HTTP, because GitHub
  requires HTTPS.
- New: SHA-256 (`sha256.cpp`) and an RSA-2048 PKCS#1 v1.5 verifier
  (`rsaverify.cpp`), both 16-bit real mode, no assembly, still `-0`/8086.
- The socket-to-disk transfer core moved to `netcore.cpp` so HTTP downloads use
  the same proven path as FTP — including the Silly Window Syndrome handling
  that large downloads depend on.

## v1.2.0 — 2026-07-30

Image viewing

- **F3 and Enter show images**, in 320x200 with 256 colours on any VGA: BMP
  (1/4/8/24/32 bpp, uncompressed plus RLE8 and RLE4), PCX, GIF including
  interlaced, and PNG. The format is detected from the file's content, not its
  extension, so a mislabelled download still opens.
- **Pan and zoom.** A picture opens fitted to the screen, or 1:1 if it already
  fits; `+`/`-` step through 25/50/100/200/400/800 %, the arrow keys and
  PgUp/PgDn move around, `*` jumps to 1:1, `f` returns to fit, and the current
  zoom is shown in the top right corner. This matters on a 720x400 screenshot,
  where fitting to 320x200 throws away three quarters of the pixels and the
  text in it stops being readable.
- **Palette images get their exact colours**, loaded straight into the VGA DAC.
  Truecolour is quantised to a 6x6x6 cube with a 40-step grey ramp and 4x4
  ordered dithering; PNG alpha is composited rather than discarded.
- **No decoder ever holds the whole picture.** Rows arrive one at a time, each
  carrying its own destination index, which is what makes bottom-up BMP,
  interlaced GIF and RLE deltas work in a fixed amount of memory.
- The decoded picture is kept in **XMS/EMS** (through the same `extmem.cpp` the
  panel store uses) at one byte per pixel, so panning and zooming re-sample an
  image already in memory instead of decoding the file again: a 720x400 picture
  is 288000 bytes and could never live in conventional memory. Without extended
  memory the fitted view still works, just without pan and zoom.
- Decoding happens before the display switches to graphics, so a damaged file
  produces an ordinary dialog on an intact screen, and Esc aborts a slow decode.
- **F1 reports free conventional memory.** A picture needs about 100 KB of it,
  so that figure is what decides whether one can be shown; the out-of-memory
  dialog repeats it.
- Not decoded, and refused with a clear message rather than a guess: JPEG, TIFF,
  interlaced PNG, and BMP variants beyond those listed.

## v1.2.1 — 2026-07-31

Performance fix for /EXMEM (now the default remote-panel backend)

- **Cursor movement no longer rescans the whole listing.** The status bar's
  marked-count/size/dir-count were recomputed by scanning every entry on each
  arrow-key press, even with nothing marked. Under /EXMEM that meant a real-mode
  XMS/EMS driver call per entry per keypress - the main cause of the slowdown
  since /EXMEM became the default. The totals are now tracked incrementally
  instead, so the status bar is O(1) regardless of listing size or backend.
- `clear_marks()` also skips its scan entirely when nothing is marked.
