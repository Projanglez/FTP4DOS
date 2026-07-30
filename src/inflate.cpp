/* =============================================================================
 * inflate.cpp - DEFLATE decompressor (RFC 1951)
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 *
 * Canonical Huffman decoding by the count/offset method from RFC 1951 section
 * 3.2.2: rather than building a lookup table, codes are walked one bit at a
 * time against the per-length counts. That is slower per symbol than a table
 * but needs only two small arrays per tree, which is the right trade here -
 * DGROUP is nearly full and the far heap is shared with a 64 KB framebuffer.
 *
 * Output goes through a 32 KB circular window, because a DEFLATE back
 * reference can point up to 32768 bytes into what was already produced.
 * ===========================================================================*/
#include <stdlib.h>
#include <string.h>

#include "inflate.h"

#define WINSZ     32768u
#define WINMASK   (WINSZ - 1)
#define INBUFSZ   2048
#define MAXBITS   15
#define MAXSYMS   288

struct Huff {
    unsigned short count[MAXBITS + 1];
    unsigned short sym[MAXSYMS];
};

struct InflState {
    InflReadFn rd;
    void      *user;

    unsigned char far *in;      /* compressed input buffer */
    int   inLen, inPos;
    int   inEof;

    unsigned long bitBuf;
    int   bitCnt;

    unsigned char far *win;     /* 32 KB circular output window */
    unsigned      winPos;       /* next write position */
    unsigned      avail;        /* bytes produced but not yet handed out */
    unsigned      readPos;      /* next read position */

    int   done;                 /* final block consumed and drained */
    int   lastBlock;            /* current block carries the BFINAL bit */
    int   inBlock;              /* inside a block */
    int   blockType;
    unsigned storedLeft;

    Huff far *lit;
    Huff far *dist;
    int   haveTrees;
};

/* --- input ---------------------------------------------------------------- */

static int fill(InflState *s)
{
    if (s->inEof) return 0;
    s->inLen = s->rd(s->user, s->in, INBUFSZ);
    s->inPos = 0;
    if (s->inLen <= 0) { s->inEof = 1; s->inLen = 0; return 0; }
    return s->inLen;
}

static int getByte(InflState *s)
{
    if (s->inPos >= s->inLen && fill(s) == 0) return -1;
    return s->in[s->inPos++];
}

static int needBits(InflState *s, int n)
{
    while (s->bitCnt < n) {
        int b = getByte(s);
        if (b < 0) return -1;
        s->bitBuf |= (unsigned long)(unsigned char)b << s->bitCnt;
        s->bitCnt += 8;
    }
    return 0;
}

static int getBits(InflState *s, int n)
{
    int v;
    if (n == 0) return 0;
    if (needBits(s, n) != 0) return -1;
    v = (int)(s->bitBuf & ((1UL << n) - 1));
    s->bitBuf >>= n;
    s->bitCnt -= n;
    return v;
}

/* --- Huffman -------------------------------------------------------------- */

/* Build the canonical code from a list of code lengths. */
static int buildHuff(Huff far *h, const unsigned char far *lens, int n)
{
    int i, len;
    unsigned short offs[MAXBITS + 1];

    for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;

    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++)
        offs[len + 1] = (unsigned short)(offs[len] + h->count[len]);

    for (i = 0; i < n; i++)
        if (lens[i]) h->sym[offs[lens[i]]++] = (unsigned short)i;

    return 0;
}

/* Walk the tree one bit at a time (RFC 1951, 3.2.2). */
static int decodeSym(InflState *s, const Huff far *h)
{
    int code = 0, first = 0, index = 0, len;

    for (len = 1; len <= MAXBITS; len++) {
        int b = getBits(s, 1);
        if (b < 0) return -1;
        code |= b;
        {
            int count = h->count[len];
            if (code - first < count) return h->sym[index + (code - first)];
            index += count;
            first  = (first + count) << 1;
            code <<= 1;
        }
    }
    return -1;
}

static const unsigned short lenBase[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
    67,83,99,115,131,163,195,227,258 };
static const unsigned char lenExtra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const unsigned short distBase[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const unsigned char distExtra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

/* The fixed trees are defined by RFC 1951, 3.2.6. */
static void fixedTrees(InflState *s)
{
    unsigned char tmp[MAXSYMS];
    int i;

    for (i = 0;   i < 144; i++) tmp[i] = 8;
    for (;        i < 256; i++) tmp[i] = 9;
    for (;        i < 280; i++) tmp[i] = 7;
    for (;        i < 288; i++) tmp[i] = 8;
    buildHuff(s->lit, tmp, 288);
    for (i = 0; i < 30; i++) tmp[i] = 5;
    buildHuff(s->dist, tmp, 30);
}

static int dynamicTrees(InflState *s)
{
    static const unsigned char ord[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    unsigned char lens[MAXSYMS + 32];
    Huff far *code;
    int hlit, hdist, hclen, i, n;

    hlit  = getBits(s, 5); if (hlit  < 0) return INFL_ERR_DATA; hlit  += 257;
    hdist = getBits(s, 5); if (hdist < 0) return INFL_ERR_DATA; hdist += 1;
    hclen = getBits(s, 4); if (hclen < 0) return INFL_ERR_DATA; hclen += 4;
    if (hlit > 286 || hdist > 30) return INFL_ERR_DATA;

    code = (Huff far *)malloc(sizeof(Huff));
    if (!code) return INFL_ERR_MEM;

    memset(lens, 0, 19);
    for (i = 0; i < hclen; i++) {
        int v = getBits(s, 3);
        if (v < 0) { free(code); return INFL_ERR_DATA; }
        lens[ord[i]] = (unsigned char)v;
    }
    buildHuff(code, lens, 19);

    n = 0;
    while (n < hlit + hdist) {
        int sym = decodeSym(s, code);
        if (sym < 0) { free(code); return INFL_ERR_DATA; }
        if (sym < 16) {
            lens[n++] = (unsigned char)sym;
        } else if (sym == 16) {
            int r, prev;
            if (n == 0) { free(code); return INFL_ERR_DATA; }
            prev = lens[n - 1];
            r = getBits(s, 2); if (r < 0) { free(code); return INFL_ERR_DATA; }
            for (r += 3; r > 0 && n < hlit + hdist; r--) lens[n++] = (unsigned char)prev;
        } else if (sym == 17) {
            int r = getBits(s, 3);
            if (r < 0) { free(code); return INFL_ERR_DATA; }
            for (r += 3; r > 0 && n < hlit + hdist; r--) lens[n++] = 0;
        } else {
            int r = getBits(s, 7);
            if (r < 0) { free(code); return INFL_ERR_DATA; }
            for (r += 11; r > 0 && n < hlit + hdist; r--) lens[n++] = 0;
        }
    }
    free(code);

    buildHuff(s->lit,  lens, hlit);
    buildHuff(s->dist, lens + hlit, hdist);
    return INFL_OK;
}

/* --- window --------------------------------------------------------------- */

static void put(InflState *s, unsigned char c)
{
    s->win[s->winPos] = c;
    s->winPos = (s->winPos + 1) & WINMASK;
    s->avail++;
}

/* Produce more output. Returns INFL_OK, 1 at end of stream, or an error. */
static int produce(InflState *s)
{
    if (s->done) return 1;

    if (!s->inBlock) {
        int last = getBits(s, 1);
        int type;
        if (last < 0) return 1;
        type = getBits(s, 2);
        if (type < 0) return INFL_ERR_DATA;
        s->blockType = type;
        s->inBlock   = 1;
        s->haveTrees = 0;
        s->lastBlock = last;

        if (type == 0) {
            unsigned len, nlen;
            int a, b, c, d;
            s->bitBuf = 0; s->bitCnt = 0;          /* stored blocks are byte-aligned */
            a = getByte(s); b = getByte(s); c = getByte(s); d = getByte(s);
            if (d < 0) return INFL_ERR_DATA;
            len  = (unsigned)a | ((unsigned)b << 8);
            nlen = (unsigned)c | ((unsigned)d << 8);
            if ((len ^ 0xFFFFu) != nlen) return INFL_ERR_DATA;
            s->storedLeft = len;
        } else if (type == 1) {
            fixedTrees(s);
            s->haveTrees = 1;
        } else if (type == 2) {
            int rc = dynamicTrees(s);
            if (rc != INFL_OK) return rc;
            s->haveTrees = 1;
        } else {
            return INFL_ERR_DATA;
        }
    }

    if (s->blockType == 0) {
        while (s->storedLeft > 0 && s->avail < WINSZ / 2) {
            int b = getByte(s);
            if (b < 0) return INFL_ERR_DATA;
            put(s, (unsigned char)b);
            s->storedLeft--;
        }
        if (s->storedLeft == 0) {
            s->inBlock = 0;
            if (s->lastBlock) { s->done = 1; return 1; }
        }
        return INFL_OK;
    }

    while (s->avail < WINSZ / 2) {
        int sym = decodeSym(s, s->lit);
        if (sym < 0) return INFL_ERR_DATA;
        if (sym < 256) {
            put(s, (unsigned char)sym);
        } else if (sym == 256) {
            s->inBlock = 0;
            if (s->lastBlock) { s->done = 1; return 1; }
            return INFL_OK;
        } else {
            int   li = sym - 257;
            int   dsym, e;
            unsigned len, dist, from;
            if (li >= 29) return INFL_ERR_DATA;
            e = getBits(s, lenExtra[li]);
            if (e < 0) return INFL_ERR_DATA;
            len = lenBase[li] + (unsigned)e;

            dsym = decodeSym(s, s->dist);
            if (dsym < 0 || dsym >= 30) return INFL_ERR_DATA;
            e = getBits(s, distExtra[dsym]);
            if (e < 0) return INFL_ERR_DATA;
            dist = distBase[dsym] + (unsigned)e;
            if (dist > WINSZ) return INFL_ERR_DATA;

            from = (s->winPos - dist) & WINMASK;
            while (len-- > 0) {
                put(s, s->win[from]);
                from = (from + 1) & WINMASK;
            }
        }
    }
    return INFL_OK;
}

/* --- public --------------------------------------------------------------- */

InflState *infl_create(InflReadFn rd, void *user)
{
    InflState *s = (InflState *)malloc(sizeof(InflState));
    if (!s) return 0;
    memset(s, 0, sizeof(InflState));
    s->rd   = rd;
    s->user = user;
    s->in   = (unsigned char far *)malloc(INBUFSZ);
    s->win  = (unsigned char far *)malloc(WINSZ);
    s->lit  = (Huff far *)malloc(sizeof(Huff));
    s->dist = (Huff far *)malloc(sizeof(Huff));
    if (!s->in || !s->win || !s->lit || !s->dist) {
        infl_destroy(s);
        return 0;
    }
    return s;
}

void infl_destroy(InflState *s)
{
    if (!s) return;
    if (s->in)   free(s->in);
    if (s->win)  free(s->win);
    if (s->lit)  free(s->lit);
    if (s->dist) free(s->dist);
    free(s);
}

int infl_read(InflState *s, unsigned char far *out, unsigned len)
{
    unsigned n = 0;

    while (n < len) {
        if (s->avail == 0) {
            int rc = produce(s);
            if (rc < 0) return rc;
            if (rc == 1 && s->avail == 0) return 1;   /* stream exhausted */
        }
        while (n < len && s->avail > 0) {
            out[n++] = s->win[s->readPos];
            s->readPos = (s->readPos + 1) & WINMASK;
            s->avail--;
        }
    }
    return INFL_OK;
}
