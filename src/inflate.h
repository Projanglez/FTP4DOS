/* =============================================================================
 * inflate.h - DEFLATE decompressor (RFC 1951), streaming
 * -----------------------------------------------------------------------------
 * Pull-based: the caller asks for output bytes and supplies compressed input
 * through a callback whenever the decoder runs dry. That suits PNG, where the
 * compressed data is split across any number of IDAT chunks, and it means the
 * whole compressed file never has to be in memory.
 *
 * Memory: one 32 KB window plus the Huffman tables, all on the far heap.
 *
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#ifndef INFLATE_H
#define INFLATE_H

#define INFL_OK        0
#define INFL_ERR_DATA -1    /* malformed stream            */
#define INFL_ERR_MEM  -2    /* out of memory               */
#define INFL_ERR_IO   -3    /* the input callback failed   */

/* Refill callback: put up to 'max' compressed bytes into 'buf' and return how
 * many were written; 0 means the input is finished. */
typedef int (*InflReadFn)(void *user, unsigned char far *buf, int max);

struct InflState;

/* Create/destroy. Returns 0 on failure. */
InflState *infl_create(InflReadFn rd, void *user);
void       infl_destroy(InflState *s);

/* Pull exactly 'len' decompressed bytes into 'out'. Returns INFL_OK, a
 * negative INFL_ERR_*, or 1 when the stream ended before 'len' bytes were
 * available (the remainder of 'out' is untouched). */
int infl_read(InflState *s, unsigned char far *out, unsigned len);

#endif /* INFLATE_H */
