/* =============================================================================
 * rsaverify.cpp - RSA-2048 / PKCS#1 v1.5 signature verification for FTP4DOS
 * -----------------------------------------------------------------------------
 * Computes s^65537 mod n with Montgomery multiplication over 128 limbs of 16
 * bits, then checks the result against a PKCS#1 v1.5 block that this code
 * builds itself.
 *
 * Integer widths are pinned rather than left to 'int': a limb is always 16 bits
 * and a product always 32, on the 16-bit DOS target and on a host test build
 * alike. Every intermediate is written so it cannot exceed 32 bits - the
 * bound is checked in the comment at mont_mul().
 *
 * Cost on a 25 MHz 386: 17 Montgomery multiplications of 128x128 limbs, plus
 * building R^2 mod n by doubling, comes to roughly a second. That is fine for
 * an operation that happens once per update check.
 * ===========================================================================*/
#include <string.h>

#include "rsaverify.h"

typedef unsigned short limb;    /* exactly 16 bits on both targets */
typedef unsigned long  dlimb;   /* exactly 32 bits on both targets */

#define NL        (RSA_BYTES / 2)   /* 128 limbs */
#define LIMB_MASK 0xFFFFUL

/* DER DigestInfo prefix for SHA-256 (RFC 8017, section 9.2, notes on the
 * algorithm identifier):
 *   SEQUENCE { SEQUENCE { OID 2.16.840.1.101.3.4.2.1, NULL }, OCTET STRING(32) }
 */
static const unsigned char SHA256_DIGESTINFO[19] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
    0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
};

#define DIGESTINFO_LEN 19
#define HASH_LEN       32
/* 0x00 0x01 <PS> 0x00 <DigestInfo> <hash> */
#define PS_LEN (RSA_BYTES - 3 - DIGESTINFO_LEN - HASH_LEN)   /* 202 */

/* ---------------------------------------------------------------------------
 * Byte order: the wire format is big-endian, the arithmetic is little-endian
 * limbs (index 0 = least significant).
 * ------------------------------------------------------------------------- */
static void be_to_limbs(const unsigned char *be, limb *out)
{
    int i, j;
    for (i = 0; i < NL; i++) {
        j = RSA_BYTES - 2 - 2 * i;
        out[i] = (limb)(((limb)be[j] << 8) | (limb)be[j + 1]);
    }
}

static void limbs_to_be(const limb *in, unsigned char *be)
{
    int i, j;
    for (i = 0; i < NL; i++) {
        j = RSA_BYTES - 2 - 2 * i;
        be[j]     = (unsigned char)((in[i] >> 8) & 0xFF);
        be[j + 1] = (unsigned char)(in[i] & 0xFF);
    }
}

/* ---------------------------------------------------------------------------
 * Multi-precision helpers over NL limbs
 * ------------------------------------------------------------------------- */
static int cmp_nl(const limb *a, const limb *b)
{
    int i;
    for (i = NL - 1; i >= 0; i--) {
        if (a[i] != b[i]) return (a[i] < b[i]) ? -1 : 1;
    }
    return 0;
}

/* a -= b (mod 2^2048). Returns the final borrow. */
static limb sub_nl(limb *a, const limb *b)
{
    dlimb d, borrow = 0;
    int i;
    for (i = 0; i < NL; i++) {
        /* +0x10000 keeps the subtraction unsigned: the result is >= 0x10000
         * exactly when no borrow is needed. */
        d = (dlimb)a[i] + 0x10000UL - (dlimb)b[i] - borrow;
        a[i] = (limb)(d & LIMB_MASK);
        borrow = 1UL - (d >> 16);
    }
    return (limb)borrow;
}

/* a = 2a mod n, assuming a < n on entry. */
static void dbl_mod(limb *a, const limb *n)
{
    dlimb v;
    limb carry = 0, next;
    int i;
    for (i = 0; i < NL; i++) {
        next = (limb)(a[i] >> 15);
        v = ((dlimb)a[i] << 1) | (dlimb)carry;
        a[i] = (limb)(v & LIMB_MASK);
        carry = next;
    }
    /* a < n before, so 2a < 2n: at most one subtraction is ever needed. A set
     * carry means 2a >= 2^2048 > n, which also calls for the subtraction. */
    if (carry || cmp_nl(a, n) >= 0)
        sub_nl(a, n);
}

/* ---------------------------------------------------------------------------
 * Montgomery arithmetic
 * ------------------------------------------------------------------------- */

/* -n0^-1 mod 2^16, via Newton iteration. x <- x * (2 - n0*x) doubles the number
 * of correct bits per step, so 1 -> 2 -> 4 -> 8 -> 16 takes four rounds.
 * Requires n0 odd, which rsa_verify_sha256() checks before calling. */
static limb mont_n0inv(limb n0)
{
    dlimb x = 1UL, t;
    int i;
    for (i = 0; i < 4; i++) {
        t = ((dlimb)n0 * x) & LIMB_MASK;
        t = (2UL - t) & LIMB_MASK;
        x = (x * t) & LIMB_MASK;
    }
    return (limb)((0x10000UL - x) & LIMB_MASK);
}

/* z = x * y * R^-1 mod n, where R = 2^2048 (CIOS variant).
 *
 * Aliasing is safe: the accumulator is local and only copied to z at the end,
 * so mont_mul(t, t, t, ...) - the squaring case - works.
 *
 * Overflow bound for the inner steps: x[j]*y[i] <= (2^16-1)^2, plus t[j] and a
 * carry of at most 2^16-1 each, totals exactly 2^32-1. Nothing can exceed a
 * 32-bit product, which is why 'dlimb' suffices and no 48-bit path is needed.
 */
static void mont_mul(limb *z, const limb *x, const limb *y,
                     const limb *n, limb n0inv)
{
    limb t[NL + 2];
    dlimb p, c;
    limb m;
    int i, j;

    memset(t, 0, sizeof(t));

    for (i = 0; i < NL; i++) {
        /* t += x * y[i] */
        c = 0;
        for (j = 0; j < NL; j++) {
            p = (dlimb)x[j] * (dlimb)y[i] + (dlimb)t[j] + c;
            t[j] = (limb)(p & LIMB_MASK);
            c = p >> 16;
        }
        p = (dlimb)t[NL] + c;
        t[NL]     = (limb)(p & LIMB_MASK);
        t[NL + 1] = (limb)(p >> 16);

        /* Choose m so that (t + m*n) has a zero low limb, then shift it out. */
        m = (limb)(((dlimb)t[0] * (dlimb)n0inv) & LIMB_MASK);
        p = (dlimb)t[0] + (dlimb)m * (dlimb)n[0];
        c = p >> 16;
        for (j = 1; j < NL; j++) {
            p = (dlimb)t[j] + (dlimb)m * (dlimb)n[j] + c;
            t[j - 1] = (limb)(p & LIMB_MASK);
            c = p >> 16;
        }
        p = (dlimb)t[NL] + c;
        t[NL - 1] = (limb)(p & LIMB_MASK);
        t[NL]     = (limb)(((dlimb)t[NL + 1] + (p >> 16)) & LIMB_MASK);
    }

    /* The CIOS result is < 2n, so a single conditional subtraction normalises
     * it. A set top limb means t >= 2^2048 > n; the borrow out of sub_nl()
     * then cancels that limb exactly. */
    if (t[NL] != 0 || cmp_nl(t, n) >= 0)
        sub_nl(t, n);

    memcpy(z, t, NL * sizeof(limb));
}

/* --- Progress reporting --------------------------------------------------
 * Work is counted in units of one dbl_mod (which costs O(NL) limb operations).
 * A mont_mul costs 2*NL*NL, i.e. 2*NL = 256 such units. Weighting them this way
 * matters: building R^2 needs 2048 doublings, about 30% of the whole
 * verification, so counting only the multiplications would leave the bar frozen
 * for the first third and then jump. */
#define WORK_MONTMUL   (2 * NL)                    /* 256 units            */
#define WORK_MONTMULS  19                          /* 1 + 16 + 1 + 1       */
#define WORK_TOTAL     (2048UL + (unsigned long)WORK_MONTMULS * WORK_MONTMUL)

struct RsaProgress {
    RsaProgressCb cb;
    void         *ctx;
    unsigned long done;
    int           aborted;
};

static void rsaTick(RsaProgress *p, unsigned long units)
{
    if (!p || !p->cb || p->aborted) return;
    p->done += units;
    if (p->cb(p->ctx, p->done, WORK_TOTAL)) p->aborted = 1;
}

/* r2 = R^2 mod n, R = 2^2048.
 *
 * Computed on the device rather than shipped alongside the key: it keeps
 * rsakeys.cpp to nothing but the modulus, so the only generated data anyone has
 * to trust is the key itself. The doubling loop costs about 0.2 s on a 386,
 * which is not worth trading that simplicity for. */
static void mont_r2(limb *r2, const limb *n, RsaProgress *p)
{
    dlimb d, borrow = 0;
    int i;

    /* r2 = R - n = (0 - n) over NL limbs. Valid as R mod n because an RSA
     * modulus has its top bit set, so R/2 < n < R. */
    for (i = 0; i < NL; i++) {
        d = 0x10000UL - (dlimb)n[i] - borrow;
        r2[i] = (limb)(d & LIMB_MASK);
        borrow = 1UL - (d >> 16);
    }

    /* 2^2048 mod n, doubled 2048 more times, is 2^4096 mod n = R^2 mod n. */
    for (i = 0; i < 2048; i++) {
        dbl_mod(r2, n);
        /* Report every 64 doublings: often enough for a smooth bar, rare
         * enough that the callback does not dominate the loop. */
        if ((i & 63) == 63) {
            rsaTick(p, 64);
            if (p && p->aborted) return;
        }
    }
}

/* out = x^65537 mod n. Returns 0 normally, -1 if the callback asked to stop. */
static int mont_pow65537(limb *out, const limb *x, const limb *n, limb n0inv,
                         RsaProgress *p)
{
    limb r2[NL], xm[NL], t[NL], one[NL];
    int i;

    mont_r2(r2, n, p);
    if (p && p->aborted) return -1;

    mont_mul(xm, x, r2, n, n0inv);      /* xm = x*R mod n (Montgomery form) */
    rsaTick(p, WORK_MONTMUL);

    memcpy(t, xm, sizeof(t));
    for (i = 0; i < 16; i++) {
        mont_mul(t, t, t, n, n0inv);    /* t = x^(2^16) * R mod n */
        rsaTick(p, WORK_MONTMUL);
        if (p && p->aborted) return -1;
    }
    mont_mul(t, t, xm, n, n0inv);       /* t = x^65537 * R mod n */
    rsaTick(p, WORK_MONTMUL);

    memset(one, 0, sizeof(one));
    one[0] = 1;
    mont_mul(out, t, one, n, n0inv);    /* leave Montgomery form */
    rsaTick(p, WORK_MONTMUL);

    return (p && p->aborted) ? -1 : 0;
}

/* ---------------------------------------------------------------------------
 * Public entry point
 * ------------------------------------------------------------------------- */
int rsa_verify_sha256(const unsigned char *sig, unsigned int siglen,
                      const unsigned char *hash,
                      const unsigned char *modulus)
{
    return rsa_verify_sha256_cb(sig, siglen, hash, modulus, 0, 0);
}

int rsa_verify_sha256_cb(const unsigned char *sig, unsigned int siglen,
                         const unsigned char *hash,
                         const unsigned char *modulus,
                         RsaProgressCb cb, void *ctx)
{
    limb n[NL], s[NL], m[NL];
    unsigned char em[RSA_BYTES], want[RSA_BYTES];
    RsaProgress prog;
    limb n0inv;
    int i, allzero;

    prog.cb = cb; prog.ctx = ctx; prog.done = 0; prog.aborted = 0;

    if (!sig || !hash || !modulus) return 0;
    if (siglen != RSA_BYTES) return 0;

    /* Guard the assumptions the arithmetic rests on. A real RSA modulus always
     * satisfies both; failing here means rsakeys.cpp is corrupt, not that a
     * signature is merely wrong. */
    if ((modulus[RSA_BYTES - 1] & 1) == 0) return 0;   /* must be odd     */
    if ((modulus[0] & 0x80) == 0) return 0;            /* top bit set     */

    /* Reject s >= n. Big-endian byte order makes memcmp a numeric comparison. */
    if (memcmp(sig, modulus, RSA_BYTES) >= 0) return 0;

    /* Reject the degenerate s = 0 and s = 1, whose powers are themselves. */
    allzero = 1;
    for (i = 0; i < RSA_BYTES - 1; i++) {
        if (sig[i] != 0) { allzero = 0; break; }
    }
    if (allzero && sig[RSA_BYTES - 1] <= 1) return 0;

    be_to_limbs(modulus, n);
    be_to_limbs(sig, s);
    n0inv = mont_n0inv(n[0]);
    /* -1, not 0: a cancelled check must not be reported as a bad signature. */
    if (mont_pow65537(m, s, n, n0inv, &prog) != 0) return -1;
    limbs_to_be(m, em);

    /* Build the block we expect and compare it whole.
     *
     * This is the security-critical choice in the file: constructing the
     * expected encoding and comparing it makes Bleichenbacher's low-exponent
     * forgery structurally impossible, because there is nowhere for an
     * attacker to hide extra bytes. Parsing the block instead - locating the
     * 0x00 separator and reading what follows - is the mistake that made that
     * attack work against real implementations. Never turn this into a parser.
     */
    want[0] = 0x00;
    want[1] = 0x01;
    memset(want + 2, 0xFF, PS_LEN);
    want[2 + PS_LEN] = 0x00;
    memcpy(want + 3 + PS_LEN, SHA256_DIGESTINFO, DIGESTINFO_LEN);
    memcpy(want + 3 + PS_LEN + DIGESTINFO_LEN, hash, HASH_LEN);

    return (memcmp(em, want, RSA_BYTES) == 0) ? 1 : 0;
}
