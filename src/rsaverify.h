/* =============================================================================
 * rsaverify.h - RSA-2048 / PKCS#1 v1.5 signature verification for FTP4DOS
 * -----------------------------------------------------------------------------
 * Verify only. Signing happens on the maintainer's machine with OpenSSL; the
 * private key never comes near this code or this repository.
 *
 * Why this exists: the updater fetches over plain HTTP because mTCP has no TLS,
 * so anyone on the network path can substitute the manifest and the executable.
 * The signature - not the transport - is what makes an update trustworthy.
 *
 * Fixed parameters, all hard-coded on purpose:
 *   modulus   2048 bit (256 bytes, big-endian)
 *   exponent  65537, implemented as 16 squarings plus one multiply
 *   padding   PKCS#1 v1.5 over a SHA-256 digest
 * Anything else is rejected. tools/pubkey-to-c.ps1 enforces the same limits on
 * the key side so a mismatch cannot reach a build.
 *
 * Self-contained: no DOS/TUI/mTCP dependencies, and the arithmetic uses fixed
 * widths ('unsigned short' limbs, 'unsigned long' products) so a host build
 * computes bit-identically to the 16-bit DOS build and can be tested directly.
 * ===========================================================================*/
#ifndef RSAVERIFY_H
#define RSAVERIFY_H

#define RSA_BYTES 256   /* 2048 bit */

/* Verify 'sig' (siglen bytes, big-endian) over the 32-byte SHA-256 'hash',
 * against the big-endian 'modulus' of RSA_BYTES bytes.
 *
 * Returns 1 if and only if the signature is valid; 0 in every other case,
 * including malformed input. There is deliberately no error detail: a caller
 * must not be able to treat "almost valid" as anything but invalid. */
int rsa_verify_sha256(const unsigned char *sig, unsigned int siglen,
                      const unsigned char *hash,
                      const unsigned char *modulus);

/* Progress/cancel hook, same shape as the other callbacks in the project so
 * ncftp.cpp's copy_progress() can be passed straight in. Returning non-zero
 * aborts the verification.
 *
 * Verifying takes about 3.5 s on a 25 MHz 386, which is long enough that the
 * user deserves a moving bar rather than a frozen screen. The work is reported
 * in weighted units: building R^2 mod n is roughly 30% of the total and is
 * counted, so the bar advances evenly instead of stalling at the start. */
typedef int (*RsaProgressCb)(void *ctx, unsigned long sofar,
                             unsigned long total);

/* As above, with progress. Returns 1 = valid, 0 = invalid, -1 = aborted.
 * Note -1 is distinct from 0 on purpose: a cancelled check must never be
 * reported to the user as a failed signature. */
int rsa_verify_sha256_cb(const unsigned char *sig, unsigned int siglen,
                         const unsigned char *hash,
                         const unsigned char *modulus,
                         RsaProgressCb cb, void *ctx);

#endif /* RSAVERIFY_H */
