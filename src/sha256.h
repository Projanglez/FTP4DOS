/* =============================================================================
 * sha256.h - SHA-256 (FIPS 180-4) for FTP4DOS
 * -----------------------------------------------------------------------------
 * Used by the auto-updater: the RSA signature is computed over the SHA-256 of
 * the manifest, and the downloaded executable is checked against the hash the
 * (signed) manifest states.
 *
 * Self-contained - no DOS/TUI/mTCP dependencies, so this module builds and can
 * be tested on the host as well as under Open Watcom.
 *
 * Why not MD5, which the project already has in checksum.cpp: MD5 and SHA-1 are
 * vulnerable to practical chosen-prefix collisions, which is exactly the attack
 * that matters when a hash is what a signature commits to. checksum.cpp stays
 * as it is - it serves Alt+F9, where the job is spotting corruption.
 *
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 *   'unsigned long' is exactly 32 bits here, which the algorithm relies on.
 * ===========================================================================*/
#ifndef SHA256_H
#define SHA256_H

#define SHA256_DIGEST_LEN 32
#define SHA256_HEX_LEN    65   /* 64 hex chars + NUL */

struct Sha256Ctx {
    unsigned long state[8];       /* h0..h7                                  */
    unsigned long lenlo, lenhi;   /* message length in BITS (64-bit, lo/hi)  */
    unsigned char buf[64];        /* partial 64-byte block                   */
    unsigned int  buflen;         /* bytes currently buffered (0..63)        */
};

/* Progress/cancel hook for sha256_file(). Deliberately the same shape as
 * FtpProgressCb (ftpcli.h) so ncftp.cpp's copy_progress() can be passed
 * straight in. Returning non-zero aborts the hash. */
typedef int (*Sha256ProgressCb)(void *ctx, unsigned long sofar,
                                unsigned long total);

void sha256_init(struct Sha256Ctx *c);
void sha256_update(struct Sha256Ctx *c, const void *data, unsigned int len);
void sha256_final(struct Sha256Ctx *c, unsigned char out[SHA256_DIGEST_LEN]);

/* One-shot hash of a memory buffer. */
void sha256_buf(const void *data, unsigned int len,
                unsigned char out[SHA256_DIGEST_LEN]);

/* Hash a whole file. Returns 0 on success, -1 if it cannot be opened or read,
 * -2 if the callback asked to abort. 'cb' may be NULL. */
int sha256_file(const char *path, unsigned char out[SHA256_DIGEST_LEN],
                Sha256ProgressCb cb, void *ctx);

/* 32 raw bytes -> 64 lowercase hex chars + NUL. 'out' needs SHA256_HEX_LEN. */
void sha256_hex(const unsigned char h[SHA256_DIGEST_LEN], char *out);

/* Parse 64 lowercase/uppercase hex chars into 32 bytes.
 * Returns 0 on success, -1 if the text is not exactly 64 hex digits. */
int sha256_parse_hex(const char *hex, unsigned char out[SHA256_DIGEST_LEN]);

#endif /* SHA256_H */
