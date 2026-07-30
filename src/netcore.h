/* =============================================================================
 * netcore.h - shared transport core for FTP4DOS
 * -----------------------------------------------------------------------------
 * The byte pump that sits between an mTCP socket and a file on disk, plus its
 * instrumentation and the buffers both are sized by. Extracted verbatim from
 * ftpcli.cpp so httpget.cpp (the auto-updater's download path) uses exactly the
 * same code rather than a second, subtly different copy.
 *
 * That sharing is not a tidiness exercise. drainToFile() is what keeps a
 * download from stalling under TCP Silly Window Syndrome, and flushXferBuf()
 * is what keeps the packet driver serviced during disk writes. Both behaviours
 * were arrived at by measurement on real 386 hardware (see the comments at the
 * definitions, and CLAUDE.md). A separate HTTP implementation would have had to
 * rediscover them the hard way.
 *
 * Internal header: unlike ftpcli.h, which deliberately keeps mTCP out of its
 * interface, this one is only included by ftpcli.cpp and httpget.cpp and may
 * name mTCP types directly.
 * ===========================================================================*/
#ifndef NETCORE_H
#define NETCORE_H

#include <stdio.h>

#include "types.h"
#include "timer.h"

/* Only pointers to TcpSocket appear below, so a forward declaration is enough.
 * Pulling in tcpsockm.h here would not work anyway: the mTCP headers have an
 * include-order dependency (tcp.h must come first), and forcing that order on
 * every includer is exactly the fragility worth avoiding. The .cpp files
 * include the full chain themselves. */
class TcpSocket;

/* --- Shared transport constants ------------------------------------------ */
#define DATA_TIMEOUT_MS     60000ul   /* 60s for data transfers               */
/* 5s: a lost first SYN (cold start) fails fast instead of burning 15s before
 * the app-level retry (perform_connect) sends a fresh SYN. mTCP's own SYN
 * retry only kicks in after ~10s - too late for that purpose. */
#define CONNECT_TIMEOUT_MS  5000ul
/* How often a transfer loop polls the progress callback (= the keyboard)
 * when no data is arriving, so ESC still works on a stalled transfer. */
#define CB_POLL_MS           200ul
#define DATA_RECV_SIZE      16384   /* mTCP maximum; large receive window      */

/* File transfer buffer: received data is accumulated here and written to
 * disk in large blocks (small writes are expensive on old machines - see
 * the mTCP FTP client, FTP_FILE_BUFFER). Both sizes are configurable in
 * MTCP.CFG: FTP4DOS_TCP_BUFFER / FTP4DOS_FILE_BUFFER, with the mTCP FTP
 * client's FTP_TCP_BUFFER / FTP_FILE_BUFFER as fallback keys. */
#define FILE_BUF_DEFAULT     8192
#define FILE_BUF_MAX        32768UL
#define TCP_BUF_MAX         16384UL
#define BUF_CFG_MIN           512UL

/* Chunk size of a single disk write; configurable via FTP4DOS_DISK_SLICE so
 * it can be bisected on real hardware (see netcore_read_cfg). */
#define DISK_SLICE_DEFAULT 4096u
#define DISK_SLICE_MAX    16384UL

/* --- Shared state -------------------------------------------------------- */
extern uint16_t g_tcpBufSize;    /* data socket recv buffer */
extern uint16_t g_fileBufSize;   /* file transfer buffer    */
extern uint8_t *g_fileBuf;       /* allocated by netcore_alloc_buffers */
extern uint16_t g_diskSlice;

/* Read the tuning keys from MTCP.CFG. Must be called between
 * Utils::openCfgFile() and Utils::closeCfgFile(). */
void netcore_read_cfg(void);

/* Allocate the file transfer buffer on the far heap, halving the requested
 * size until it fits. Returns 0 on success, -1 if even the minimum fails. */
int netcore_alloc_buffers(void);

uint16_t nextLocalPort(void);

/* Drive the mTCP stack once (needed in every wait loop). */
void driveStack(void);

/* Milliseconds elapsed since the given tick value. */
unsigned long elapsedMs(clockTicks_t start);

/* --- Transfer sampler (diagnostics, off unless FTP4DOS_XFERLOG is set) ---- */

/* The three per-interval counters are written by the transfer loops, including
 * the upload path in ftpcli.cpp's stor(), which reuses them for its own send
 * loop (recv -> accepted sends, idle -> full send window, flush -> disk reads).
 * They are reset by each xferLogSample(). */
extern unsigned long g_xlRecv;
extern unsigned long g_xlIdle;
extern unsigned long g_xlFlush;

void xferLogOpen(const char *what, const char *name, unsigned long size);
void xferLogSample(unsigned long total, unsigned bufUsed);
void xferLogClose(unsigned long total, int ioerr);

/* --- The byte pump ------------------------------------------------------- */

/* Accumulation state for the file transfer buffer. Shared between the
 * readReply() drain hook and the main transfer loop in retr(), so the fill
 * offset stays consistent across both.
 *
 * NOTE (v1.0.1): this was briefly reworked into a ring buffer that refilled
 * from the socket between disk-write slices, the idea being that freed space
 * reopens the TCP receive window mid-flush. Measured in the QEMU test VM it
 * was catastrophic - a 5 MB download went from under 4 s to an ETA of over
 * 4 minutes (~70x slower), because a nearly-full ring hands recv() tiny
 * fragments and each of those costs a pure ACK. The proven linear buffer is
 * back; do not reintroduce the ring without measuring a full download first. */
struct XferBuf {
    uint8_t  *buf;      /* g_fileBuf                          */
    uint16_t  size;     /* g_fileBufSize                      */
    uint16_t  used;     /* accumulated bytes not yet written  */
};

int flushXferBuf(XferBuf *xb, FILE *f);
int drainToFile(TcpSocket *ds, FILE *f, XferBuf *xb, unsigned long *total);
int drainDiscard(TcpSocket *ds);

#endif /* NETCORE_H */
