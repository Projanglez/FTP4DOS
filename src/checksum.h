/* =============================================================================
 * checksum.h - CRC32 and MD5 file checksums for FTP4DOS
 * -----------------------------------------------------------------------------
 * Self-contained, no DOS/TUI/mTCP dependencies. Both sums are computed in a
 * single streaming pass over the file's bytes (one fread loop).
 *
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 *   unsigned long is exactly 32 bits here, which both algorithms rely on.
 * ===========================================================================*/
#ifndef CHECKSUM_H
#define CHECKSUM_H

/* Compute CRC32 (IEEE) and MD5 (RFC 1321) of the file at 'path' in one pass.
 * crc_out: buffer of at least 9 bytes  -> 8 lowercase hex chars + NUL.
 * md5_out: buffer of at least 33 bytes -> 32 lowercase hex chars + NUL.
 * Returns 0 on success, -1 if the file cannot be opened or a read fails. */
int checksum_file(const char *path, char *crc_out, char *md5_out);

#endif /* CHECKSUM_H */
