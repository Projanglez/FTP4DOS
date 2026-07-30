/* =============================================================================
 * sha256.cpp - SHA-256 (FIPS 180-4) for FTP4DOS
 * -----------------------------------------------------------------------------
 * Straight reference implementation, no inline assembly, no floating point.
 *
 * Note on integer width: under Open Watcom 16-bit DOS, 'unsigned long' is
 * exactly 32 bits, so arithmetic wraps mod 2^32 automatically, as the algorithm
 * requires. The project builds with -0 (8086 instruction set, see the comment
 * block in MAKEFILE), so the compiler synthesises these 32-bit operations from
 * 16-bit ones. That costs speed - roughly 2-3 seconds for a 271 KB executable
 * on a 25 MHz 386 - but it is paid once when installing an update, and building
 * this module with -3 instead would tie the whole binary to a 386 or better.
 *
 * The message schedule is kept as a rolling 16-word window rather than the full
 * 64 words: same result, 192 bytes less stack, no slower.
 * ===========================================================================*/
#include <stdio.h>
#include <string.h>

#include "sha256.h"

/* First 32 bits of the fractional parts of the cube roots of the first 64
 * primes (FIPS 180-4, section 4.2.2). */
static const unsigned long K[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

#define ROTR(x, n)  ((((x) >> (n)) | ((x) << (32 - (n)))) & 0xFFFFFFFFUL)
#define SHR(x, n)   ((x) >> (n))

#define BSIG0(x)    (ROTR(x,  2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BSIG1(x)    (ROTR(x,  6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SSIG0(x)    (ROTR(x,  7) ^ ROTR(x, 18) ^ SHR(x,  3))
#define SSIG1(x)    (ROTR(x, 17) ^ ROTR(x, 19) ^ SHR(x, 10))

#define CH(x, y, z)  (((x) & (y)) ^ ((~(x)) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

static void sha256_block(struct Sha256Ctx *c, const unsigned char *p)
{
    unsigned long w[16];
    unsigned long a, b, cc, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
               ((unsigned long)p[2] <<  8) | ((unsigned long)p[3]);
        p += 4;
    }

    a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];
    e = c->state[4]; f = c->state[5]; g  = c->state[6]; h = c->state[7];

    for (i = 0; i < 64; i++) {
        if (i >= 16) {
            /* Rolling window: w[i & 15] is w[i-16] on entry. */
            w[i & 15] = (w[i & 15] + SSIG0(w[(i + 1) & 15]) +
                         w[(i + 9) & 15] + SSIG1(w[(i + 14) & 15]))
                        & 0xFFFFFFFFUL;
        }
        t1 = (h + BSIG1(e) + CH(e, f, g) + K[i] + w[i & 15]) & 0xFFFFFFFFUL;
        t2 = (BSIG0(a) + MAJ(a, b, cc)) & 0xFFFFFFFFUL;
        h = g; g = f; f = e;
        e = (d + t1) & 0xFFFFFFFFUL;
        d = cc; cc = b; b = a;
        a = (t1 + t2) & 0xFFFFFFFFUL;
    }

    c->state[0] = (c->state[0] + a)  & 0xFFFFFFFFUL;
    c->state[1] = (c->state[1] + b)  & 0xFFFFFFFFUL;
    c->state[2] = (c->state[2] + cc) & 0xFFFFFFFFUL;
    c->state[3] = (c->state[3] + d)  & 0xFFFFFFFFUL;
    c->state[4] = (c->state[4] + e)  & 0xFFFFFFFFUL;
    c->state[5] = (c->state[5] + f)  & 0xFFFFFFFFUL;
    c->state[6] = (c->state[6] + g)  & 0xFFFFFFFFUL;
    c->state[7] = (c->state[7] + h)  & 0xFFFFFFFFUL;
}

void sha256_init(struct Sha256Ctx *c)
{
    /* First 32 bits of the fractional parts of the square roots of the first
     * eight primes. */
    c->state[0] = 0x6a09e667UL; c->state[1] = 0xbb67ae85UL;
    c->state[2] = 0x3c6ef372UL; c->state[3] = 0xa54ff53aUL;
    c->state[4] = 0x510e527fUL; c->state[5] = 0x9b05688cUL;
    c->state[6] = 0x1f83d9abUL; c->state[7] = 0x5be0cd19UL;
    c->lenlo = c->lenhi = 0UL;
    c->buflen = 0;
}

void sha256_update(struct Sha256Ctx *c, const void *data, unsigned int len)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned long oldlo;
    unsigned int take;

    /* Message length in bits, as a 64-bit counter split across two longs. */
    oldlo = c->lenlo;
    c->lenlo = (c->lenlo + ((unsigned long)len << 3)) & 0xFFFFFFFFUL;
    if (c->lenlo < oldlo) c->lenhi++;
    c->lenhi = (c->lenhi + ((unsigned long)len >> 29)) & 0xFFFFFFFFUL;

    /* Top up a partial block first. */
    if (c->buflen) {
        take = 64 - c->buflen;
        if (take > len) take = len;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        len -= take;
        if (c->buflen == 64) {
            sha256_block(c, c->buf);
            c->buflen = 0;
        }
    }

    while (len >= 64) {
        sha256_block(c, p);
        p += 64;
        len -= 64;
    }

    if (len) {
        memcpy(c->buf, p, len);
        c->buflen = len;
    }
}

void sha256_final(struct Sha256Ctx *c, unsigned char out[SHA256_DIGEST_LEN])
{
    unsigned char tail[8];
    unsigned long lo, hi;
    unsigned char pad = 0x80;
    unsigned char zero = 0x00;
    int i;

    /* Capture the length before the padding bytes extend it. */
    lo = c->lenlo;
    hi = c->lenhi;

    sha256_update(c, &pad, 1);
    while (c->buflen != 56)
        sha256_update(c, &zero, 1);

    tail[0] = (unsigned char)((hi >> 24) & 0xFFUL);
    tail[1] = (unsigned char)((hi >> 16) & 0xFFUL);
    tail[2] = (unsigned char)((hi >>  8) & 0xFFUL);
    tail[3] = (unsigned char)( hi        & 0xFFUL);
    tail[4] = (unsigned char)((lo >> 24) & 0xFFUL);
    tail[5] = (unsigned char)((lo >> 16) & 0xFFUL);
    tail[6] = (unsigned char)((lo >>  8) & 0xFFUL);
    tail[7] = (unsigned char)( lo        & 0xFFUL);
    sha256_update(c, tail, 8);

    for (i = 0; i < 8; i++) {
        out[i * 4 + 0] = (unsigned char)((c->state[i] >> 24) & 0xFFUL);
        out[i * 4 + 1] = (unsigned char)((c->state[i] >> 16) & 0xFFUL);
        out[i * 4 + 2] = (unsigned char)((c->state[i] >>  8) & 0xFFUL);
        out[i * 4 + 3] = (unsigned char)( c->state[i]        & 0xFFUL);
    }
}

void sha256_buf(const void *data, unsigned int len,
                unsigned char out[SHA256_DIGEST_LEN])
{
    struct Sha256Ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

int sha256_file(const char *path, unsigned char out[SHA256_DIGEST_LEN],
                Sha256ProgressCb cb, void *ctx)
{
    /* On the stack rather than malloc'd: DGROUP is the scarce resource here and
     * the stack is 32 KB (see 'option stack' in MAKEFILE).
     *
     * 4 KB rather than 512 B to keep the per-call overhead down. Note this does
     * not help under QEMU, where each emulated 512-byte PIO sector transfer
     * dominates regardless of how much is asked for at once - measured at
     * ~35 KB/s there against 1551 KB/s for the in-memory hash. That is an
     * emulation artefact, so judge this on real hardware, not in the VM. */
    unsigned char buf[4096];
    struct Sha256Ctx c;
    FILE *f;
    unsigned long total = 0UL, sofar = 0UL, lastcb = 0UL;
    size_t n;
    int rc = 0;

    f = fopen(path, "rb");
    if (!f) return -1;

    /* Total size up front so the callback can show a real percentage. */
    if (fseek(f, 0L, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz > 0L) total = (unsigned long)sz;
    }
    if (fseek(f, 0L, SEEK_SET) != 0) { fclose(f); return -1; }

    sha256_init(&c);

    for (;;) {
        n = fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        sha256_update(&c, buf, (unsigned int)n);
        sofar += (unsigned long)n;

        /* Hashing 271 KB takes seconds on a 386, so the UI has to be given a
         * chance to redraw and to notice ESC. Every 16 KB is often enough to
         * feel responsive without the callback dominating the loop. */
        if (cb && (sofar - lastcb) >= 16384UL) {
            lastcb = sofar;
            if (cb(ctx, sofar, total)) { rc = -2; break; }
        }
    }

    if (rc == 0 && ferror(f)) rc = -1;
    fclose(f);
    if (rc != 0) return rc;

    if (cb) cb(ctx, sofar, total);
    sha256_final(&c, out);
    return 0;
}

void sha256_hex(const unsigned char h[SHA256_DIGEST_LEN], char *out)
{
    static const char hexd[] = "0123456789abcdef";
    int i;
    for (i = 0; i < SHA256_DIGEST_LEN; i++) {
        out[i * 2 + 0] = hexd[(h[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hexd[h[i] & 0x0F];
    }
    out[SHA256_DIGEST_LEN * 2] = '\0';
}

int sha256_parse_hex(const char *hex, unsigned char out[SHA256_DIGEST_LEN])
{
    int i, j;
    unsigned char v[2];

    if (!hex) return -1;
    for (i = 0; i < SHA256_DIGEST_LEN; i++) {
        for (j = 0; j < 2; j++) {
            char ch = hex[i * 2 + j];
            if      (ch >= '0' && ch <= '9') v[j] = (unsigned char)(ch - '0');
            else if (ch >= 'a' && ch <= 'f') v[j] = (unsigned char)(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') v[j] = (unsigned char)(ch - 'A' + 10);
            else return -1;
        }
        out[i] = (unsigned char)((v[0] << 4) | v[1]);
    }
    /* Reject trailing junk: exactly 64 digits, nothing more. */
    if (hex[SHA256_DIGEST_LEN * 2] != '\0') return -1;
    return 0;
}
