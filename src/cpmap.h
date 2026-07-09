/* =============================================================================
 * cpmap.h - UTF-8 <-> DOS codepage conversion for FTP file names
 * -----------------------------------------------------------------------------
 * Modern FTP servers send file names as UTF-8 (RFC 2640). For display in the
 * panels and for local file names those are converted to the active DOS
 * codepage (CP437/CP850/CP866 tables; unknown codepages fall back to CP437,
 * CP858 uses the CP850 table). The reverse direction encodes local names to
 * UTF-8 when uploading to a server that announced UTF8 in FEAT.
 *
 * The wire names (what is sent in RETR/CWD/DELE/...) are never converted;
 * conversion is display/local-name only. See rpanel.cpp (PanelEntry.fullname
 * keeps the original bytes) and dircopy.cpp (make_local_83).
 * ===========================================================================*/
#ifndef CPMAP_H
#define CPMAP_H

/* Select the codepage: 0 = auto-detect the active DOS codepage
 * (INT 21h AX=6601h; DOS < 3.3 or errors fall back to 437).
 * Called once at startup; every conversion also lazily auto-detects if
 * this was never called. */
void cpmap_init(unsigned codepage);

/* The codepage the tables are currently mapping for (437/850/866). */
unsigned cpmap_codepage(void);

/* 1 if 's' contains at least one valid UTF-8 multi-byte sequence and ALL
 * bytes >= 0x80 in it are part of valid sequences. Pure ASCII and CP/Latin-1
 * encoded names return 0 - those are left untouched everywhere. */
int cpmap_is_utf8(const char *s);

/* Convert UTF-8 'src' to the active DOS codepage. Unmappable codepoints
 * become '_'. dst is always 0-terminated (dstsz >= 1). */
void cpmap_utf8_to_cp(const char *src, char *dst, int dstsz);

/* Encode a codepage string as UTF-8 (for upload names on UTF-8 servers).
 * dst is always 0-terminated (dstsz >= 1). */
void cpmap_cp_to_utf8(const char *src, char *dst, int dstsz);

#endif /* CPMAP_H */
