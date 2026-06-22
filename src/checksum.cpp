/* =============================================================================
 * checksum.cpp - CRC32 and MD5 file checksums for FTP4DOS
 * -----------------------------------------------------------------------------
 * CRC32: standard IEEE polynomial 0xEDB88320, table-driven.
 * MD5  : classic RFC 1321 implementation.
 * Both are fed from a single fread() loop so the file is read only once.
 *
 * Note on integer width: under Open Watcom 16-bit DOS, 'unsigned long' is
 * exactly 32 bits, so arithmetic wraps mod 2^32 automatically (as both
 * algorithms require). No floating point, no inline assembly.
 * ===========================================================================*/
#include <stdio.h>
#include <string.h>

#include "checksum.h"

/* ---------------------------------------------------------------------------
 * CRC32 (IEEE 802.3, reflected)
 * ------------------------------------------------------------------------- */
static unsigned long crc_table[256];
static int           crc_table_ready = 0;

static void crc32_build_table(void)
{
    unsigned long c;
    int n, k;
    for (n = 0; n < 256; n++) {
        c = (unsigned long)n;
        for (k = 0; k < 8; k++)
            c = (c & 1UL) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_table_ready = 1;
}

static unsigned long crc32_update(unsigned long crc,
                                  const unsigned char *p, unsigned int len)
{
    while (len--)
        crc = crc_table[(unsigned char)((crc ^ *p++) & 0xFFUL)] ^ (crc >> 8);
    return crc;
}

/* ---------------------------------------------------------------------------
 * MD5 (RFC 1321)
 * ------------------------------------------------------------------------- */
struct Md5Ctx {
    unsigned long a, b, c, d;     /* state                                   */
    unsigned long lenlo, lenhi;   /* message length in BITS (64-bit, lo/hi)  */
    unsigned char buf[64];        /* partial 64-byte block                   */
    unsigned int  buflen;         /* bytes currently buffered (0..63)        */
};

/* Per-round left-rotate constants. */
static const unsigned char MD5_S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

/* Precomputed sine-based constants K[i] = floor(2^32 * abs(sin(i+1))). */
static const unsigned long MD5_K[64] = {
    0xd76aa478UL,0xe8c7b756UL,0x242070dbUL,0xc1bdceeeUL,
    0xf57c0fafUL,0x4787c62aUL,0xa8304613UL,0xfd469501UL,
    0x698098d8UL,0x8b44f7afUL,0xffff5bb1UL,0x895cd7beUL,
    0x6b901122UL,0xfd987193UL,0xa679438eUL,0x49b40821UL,
    0xf61e2562UL,0xc040b340UL,0x265e5a51UL,0xe9b6c7aaUL,
    0xd62f105dUL,0x02441453UL,0xd8a1e681UL,0xe7d3fbc8UL,
    0x21e1cde6UL,0xc33707d6UL,0xf4d50d87UL,0x455a14edUL,
    0xa9e3e905UL,0xfcefa3f8UL,0x676f02d9UL,0x8d2a4c8aUL,
    0xfffa3942UL,0x8771f681UL,0x6d9d6122UL,0xfde5380cUL,
    0xa4beea44UL,0x4bdecfa9UL,0xf6bb4b60UL,0xbebfbc70UL,
    0x289b7ec6UL,0xeaa127faUL,0xd4ef3085UL,0x04881d05UL,
    0xd9d4d039UL,0xe6db99e5UL,0x1fa27cf8UL,0xc4ac5665UL,
    0xf4292244UL,0x432aff97UL,0xab9423a7UL,0xfc93a039UL,
    0x655b59c3UL,0x8f0ccc92UL,0xffeff47dUL,0x85845dd1UL,
    0x6fa87e4fUL,0xfe2ce6e0UL,0xa3014314UL,0x4e0811a1UL,
    0xf7537e82UL,0xbd3af235UL,0x2ad7d2bbUL,0xeb86d391UL
};

#define MD5_ROT(x,c) (((x) << (c)) | (((x) & 0xFFFFFFFFUL) >> (32 - (c))))

static void md5_init(struct Md5Ctx *ctx)
{
    ctx->a = 0x67452301UL;
    ctx->b = 0xefcdab89UL;
    ctx->c = 0x98badcfeUL;
    ctx->d = 0x10325476UL;
    ctx->lenlo = ctx->lenhi = 0;
    ctx->buflen = 0;
}

/* Process one full 64-byte block from ctx->buf. */
static void md5_block(struct Md5Ctx *ctx)
{
    unsigned long m[16];
    unsigned long a = ctx->a, b = ctx->b, c = ctx->c, d = ctx->d;
    unsigned long f, tmp;
    int i, g;

    for (i = 0; i < 16; i++)
        m[i] =  (unsigned long)ctx->buf[i*4]
             | ((unsigned long)ctx->buf[i*4+1] << 8)
             | ((unsigned long)ctx->buf[i*4+2] << 16)
             | ((unsigned long)ctx->buf[i*4+3] << 24);

    for (i = 0; i < 64; i++) {
        if (i < 16)       { f = (b & c) | (~b & d);       g = i; }
        else if (i < 32)  { f = (d & b) | (~d & c);       g = (5*i + 1) & 15; }
        else if (i < 48)  { f = b ^ c ^ d;                g = (3*i + 5) & 15; }
        else              { f = c ^ (b | (~d & 0xFFFFFFFFUL)); g = (7*i) & 15; }

        tmp = d;
        d = c;
        c = b;
        f = (f + a + MD5_K[i] + m[g]) & 0xFFFFFFFFUL;
        b = (b + MD5_ROT(f, MD5_S[i])) & 0xFFFFFFFFUL;
        a = tmp;
    }

    ctx->a = (ctx->a + a) & 0xFFFFFFFFUL;
    ctx->b = (ctx->b + b) & 0xFFFFFFFFUL;
    ctx->c = (ctx->c + c) & 0xFFFFFFFFUL;
    ctx->d = (ctx->d + d) & 0xFFFFFFFFUL;
}

static void md5_update(struct Md5Ctx *ctx, const unsigned char *p, unsigned int len)
{
    /* Add len*8 bits to the 64-bit message length (len fits in the read
     * buffer, so len<<3 never overflows a single unsigned long). */
    unsigned long inc = (unsigned long)len << 3;
    unsigned long old = ctx->lenlo;
    ctx->lenlo = (old + inc) & 0xFFFFFFFFUL;
    if (ctx->lenlo < old) ctx->lenhi++;

    while (len > 0) {
        unsigned int space = 64 - ctx->buflen;
        unsigned int take  = (len < space) ? len : space;
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p   += take;
        len -= take;
        if (ctx->buflen == 64) {
            md5_block(ctx);
            ctx->buflen = 0;
        }
    }
}

static void md5_final(struct Md5Ctx *ctx, unsigned char out[16])
{
    unsigned char lenbytes[8];
    unsigned char pad = 0x80;
    unsigned char zero = 0x00;
    int i;

    /* Snapshot the bit length before padding changes it. */
    unsigned long lo = ctx->lenlo, hi = ctx->lenhi;
    for (i = 0; i < 4; i++) lenbytes[i]   = (unsigned char)((lo >> (8*i)) & 0xFF);
    for (i = 0; i < 4; i++) lenbytes[4+i] = (unsigned char)((hi >> (8*i)) & 0xFF);

    md5_update(ctx, &pad, 1);
    while (ctx->buflen != 56)
        md5_update(ctx, &zero, 1);
    md5_update(ctx, lenbytes, 8);   /* now buflen hits 64 -> final block */

    for (i = 0; i < 4; i++) out[i]    = (unsigned char)((ctx->a >> (8*i)) & 0xFF);
    for (i = 0; i < 4; i++) out[4+i]  = (unsigned char)((ctx->b >> (8*i)) & 0xFF);
    for (i = 0; i < 4; i++) out[8+i]  = (unsigned char)((ctx->c >> (8*i)) & 0xFF);
    for (i = 0; i < 4; i++) out[12+i] = (unsigned char)((ctx->d >> (8*i)) & 0xFF);
}

/* ---------------------------------------------------------------------------
 * Public: compute both sums in a single read pass
 * ------------------------------------------------------------------------- */
int checksum_file(const char *path, char *crc_out, char *md5_out)
{
    static unsigned char buf[4096];   /* static: large model, keep off stack */
    FILE *f;
    struct Md5Ctx md5;
    unsigned long crc = 0xFFFFFFFFUL;
    unsigned char digest[16];
    size_t n;
    int i;

    if (!crc_table_ready) crc32_build_table();
    md5_init(&md5);

    f = fopen(path, "rb");
    if (f == 0) return -1;

    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        crc = crc32_update(crc, buf, (unsigned int)n);
        md5_update(&md5, buf, (unsigned int)n);
    }
    if (ferror(f)) { fclose(f); return -1; }
    fclose(f);

    crc ^= 0xFFFFFFFFUL;
    md5_final(&md5, digest);

    sprintf(crc_out, "%08lx", crc & 0xFFFFFFFFUL);
    for (i = 0; i < 16; i++)
        sprintf(md5_out + i*2, "%02x", digest[i]);
    md5_out[32] = '\0';
    return 0;
}
