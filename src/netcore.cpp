/*
   FTP4DOS - netcore.cpp
   Shared transport core: the socket-to-disk byte pump, its instrumentation
   and the buffers both are sized by.

   Moved here verbatim from ftpcli.cpp when the auto-updater needed the same
   download path over HTTP. Nothing was rewritten in the process - the stall
   and throughput behaviour encoded in drainToFile() and flushXferBuf() was
   established by measurement on real 386 hardware, and a "cleanup" here is a
   regression waiting to happen. Read the comments before changing anything.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcore.h"

/* mTCP (same order as in the reference client) */
#include "types.h"
#include "utils.h"
#include "timer.h"
#include "packet.h"
#include "arp.h"
#include "udp.h"
#include "dns.h"
#include "tcp.h"
#include "tcpsockm.h"


/* --- Shared state --------------------------------------------------------- */
static uint16_t g_nextLocalPort = 4096;
uint16_t g_tcpBufSize  = DATA_RECV_SIZE;    /* data socket recv buffer */
uint16_t g_fileBufSize = FILE_BUF_DEFAULT;  /* file transfer buffer    */
uint8_t *g_fileBuf     = 0;                 /* allocated in netcore_alloc_buffers */
uint16_t g_diskSlice   = DISK_SLICE_DEFAULT;

/* --- Transfer sampler (diagnostics, off unless configured) ----------------
 * Writes one CSV line per second to the file named by FTP4DOS_XFERLOG in
 * MTCP.CFG. mTCP's own tracing (SET DEBUGGING / SET LOGFILE) reports totals
 * for the whole run, which cannot show WHEN a transfer stalls - and the
 * problem reported from real hardware is a periodic burst/stall pattern, so
 * the shape over time is the evidence. Per-segment TCP tracing would show it
 * but writes a line per packet to the same slow disk and thereby changes the
 * very timing under test; one line per second does not. */
#define XFERLOG_INTERVAL_MS 1000ul
static char  g_xferLogPath[64] = "";
static FILE *g_xferLog  = 0;
unsigned long g_xlRecv;             /* recv() calls that returned data      */
unsigned long g_xlIdle;             /* passes that found nothing to read    */
unsigned long g_xlFlush;            /* buffer flushes (disk writes)         */
static unsigned long g_xlPrev;      /* byte count at the previous sample    */
static clockTicks_t  g_xlStart;
static clockTicks_t  g_xlLast;


uint16_t nextLocalPort(void) {
    uint16_t p = g_nextLocalPort++;
    if (g_nextLocalPort >= 32000) g_nextLocalPort = 4096;
    return p;
}

/* Drive the mTCP stack once (needed in every wait loop). */
void driveStack(void) {
    PACKET_PROCESS_SINGLE;
    Arp::driveArp();
    Tcp::drivePackets();
}

/* Milliseconds elapsed since the given tick value. */
unsigned long elapsedMs(clockTicks_t start) {
    return (unsigned long)Timer_diff(start, TIMER_GET_CURRENT()) * TIMER_TICK_LEN;
}


/* --- Configuration -------------------------------------------------------- */

/* Read one buffer size from MTCP.CFG: 'key' wins over 'fallbackKey' (the
 * key the mTCP FTP client uses, so already-tuned configs work as-is).
 * Out-of-range or missing values keep 'def'. */
static uint16_t cfgBufSize(const char *key, const char *fallbackKey,
                           unsigned long maxv, uint16_t def) {
    char tmp[10];
    if (Utils::getAppValue((char *)key, tmp, sizeof(tmp)) != 0 &&
        Utils::getAppValue((char *)fallbackKey, tmp, sizeof(tmp)) != 0)
        return def;
    unsigned long v = strtoul(tmp, 0, 10);   /* atoi overflows at 32768 */
    if (v < BUF_CFG_MIN || v > maxv) return def;
    return (uint16_t)v;
}

void netcore_read_cfg(void) {
    g_tcpBufSize  = cfgBufSize("FTP4DOS_TCP_BUFFER", "FTP_TCP_BUFFER",
                               TCP_BUF_MAX, DATA_RECV_SIZE);
    g_fileBufSize = cfgBufSize("FTP4DOS_FILE_BUFFER", "FTP_FILE_BUFFER",
                               FILE_BUF_MAX, FILE_BUF_DEFAULT);
    /* Chunk size of a single disk write during a download. Smaller reopens
     * the receive window sooner on a slow drive, larger writes more
     * efficiently; exposed so it can be bisected on real hardware. */
    g_diskSlice   = cfgBufSize("FTP4DOS_DISK_SLICE", "FTP4DOS_DISK_SLICE",
                               DISK_SLICE_MAX, DISK_SLICE_DEFAULT);
    /* Diagnostics: path for the per-second transfer log (empty = disabled).
     * Unlike mTCP's DEBUGGING/LOGFILE tracing this samples on a timer instead
     * of per packet, so it can be left on for a full-size transfer. */
    if (Utils::getAppValue((char *)"FTP4DOS_XFERLOG", g_xferLogPath,
                           sizeof(g_xferLogPath)) != 0)
        g_xferLogPath[0] = '\0';
}

int netcore_alloc_buffers(void) {
    /* File transfer buffer (far heap, not DGROUP). If the configured size
     * cannot be allocated, halve until it fits (never below 2048). */
    if (!g_fileBuf) {
        for (;;) {
            g_fileBuf = (uint8_t *)malloc(g_fileBufSize);
            if (g_fileBuf || g_fileBufSize <= 2048) break;
            g_fileBufSize /= 2;
        }
        if (!g_fileBuf) return -1;
    }
    return 0;
}


/* --- Transfer sampler --------------------------------------------------- */

/* Start sampling one transfer. Does nothing unless FTP4DOS_XFERLOG is set.
 * The file is appended to, so several transfers end up in one log. */
void xferLogOpen(const char *what, const char *name, unsigned long size)
{
    if (g_xferLogPath[0] == '\0') return;
    g_xferLog = fopen(g_xferLogPath, "a");
    g_xlRecv = g_xlIdle = g_xlFlush = g_xlPrev = 0;
    g_xlStart = g_xlLast = TIMER_GET_CURRENT();
    if (!g_xferLog) return;
    fprintf(g_xferLog,
            "\n# %s %s (%lu bytes) tcpbuf=%u filebuf=%u slice=%u\n"
            "# sec,total,delta,recv,idle,flush,buffill\n",
            what, name, size, g_tcpBufSize, g_fileBufSize, g_diskSlice);
    fflush(g_xferLog);
}

/* Emit one sample if the interval has elapsed. Called from the transfer loop,
 * which already polls on a timer, so this costs a tick comparison per pass. */
void xferLogSample(unsigned long total, unsigned bufUsed)
{
    unsigned long ms;
    if (!g_xferLog) return;
    ms = elapsedMs(g_xlLast);
    if (ms < XFERLOG_INTERVAL_MS) return;
    g_xlLast = TIMER_GET_CURRENT();
    fprintf(g_xferLog, "%lu.%02lu,%lu,%lu,%lu,%lu,%lu,%u\n",
            elapsedMs(g_xlStart) / 1000ul, (elapsedMs(g_xlStart) % 1000ul) / 10ul,
            total, total - g_xlPrev,
            g_xlRecv, g_xlIdle, g_xlFlush, bufUsed);
    /* Flush every line: if the transfer hangs hard enough to need a reboot,
     * the log up to that point is what we get to look at. */
    fflush(g_xferLog);
    g_xlPrev  = total;
    g_xlRecv  = g_xlIdle = g_xlFlush = 0;
}

void xferLogClose(unsigned long total, int ioerr)
{
    if (!g_xferLog) return;
    fprintf(g_xferLog, "# done after %lums, %lu bytes, ioerr=%d\n",
            elapsedMs(g_xlStart), total, ioerr);
    fclose(g_xferLog);
    g_xferLog = 0;
}


/* --- The byte pump ------------------------------------------------------- */

/* Write the accumulated buffer to disk in slices, driving the TCP stack
 * between the slices. During one big blocking fwrite (up to 32 KB) nothing
 * services the packet driver: incoming frames overflow its ring, segments
 * are lost and the server backs off into retransmission timeouts - the
 * burst/stall sawtooth observed on real hardware. Slicing keeps ACKs and
 * window updates flowing while the disk works; 4 KB per slice keeps the
 * large-block write advantage (see FILE_BUF_* above).
 * Returns 0 on success, -1 on write error. */
int flushXferBuf(XferBuf *xb, FILE *f) {
    uint16_t off = 0;
    while (off < xb->used) {
        uint16_t n = (uint16_t)(xb->used - off);
        if (n > g_diskSlice) n = g_diskSlice;
        if (fwrite(xb->buf + off, 1, (size_t)n, f) != (size_t)n)
            return -1;
        off += n;
        g_xlFlush++;
        driveStack();
    }
    xb->used = 0;
    return 0;
}

/* Drain the data connection 'ds' as far as possible (RETR download).
 * This keeps the receive window open; otherwise it drops below one MSS or to
 * zero, the server stalls (Silly Window Syndrome) and sends nothing further
 * until its own persist probe fires (server-dependent, often several
 * seconds). mTCP itself re-announces the window immediately once we recv()
 * out of the zero state (TcpSocket::recv -> sendPureAck, mtcp/TCPLIB/TCP.CPP);
 * mTCP's own zero-window probe interval is 1s (TCP_PROBE_INTERVAL,
 * TCPINC/TCP.H) - there is no 5s timer in mTCP. Behavior verified against
 * mTCP 2025-01-10.
 * Received data is accumulated in xb and only written to 'f' once the
 * buffer is full: large writes instead of many per-MSS writes (the
 * latter is the difference between 10 KB/s and full speed on machines
 * with slow disk I/O); flushXferBuf() services the stack between the
 * write slices so the flush itself doesn't starve the packet driver.
 * Returns: >0 bytes received (added to *total), 0 nothing available,
 * -1 write error, -2 connection closed. */
int drainToFile(TcpSocket *ds, FILE *f, XferBuf *xb,
                unsigned long *total) {
    int16_t n;
    int got = 0;
    uint16_t minRead;

    /* Never issue a tiny recv() - flush early instead.
     *
     * mTCP reopens a closed receive window by exactly the number of bytes the
     * application just read (TcpSocket::recv -> sendPureAck when origWin == 0),
     * and it has no receiver-side SWS avoidance of its own: that is left to the
     * application. Asking for the last few hundred bytes of a nearly full
     * buffer therefore pins the window at that size, and the transfer collapses
     * into one small window per round trip.
     *
     * Measured, HTTP download of 298 KB: with 544 bytes of free space the
     * transfer settled at 577 B/s on a 386 - against 250 KB/s for FTP on the
     * same machine. Flushing as soon as less than minRead is free keeps every
     * request at least one segment large. */
    minRead = (uint16_t)(xb->size / 4);
    if (minRead > 2048) minRead = 2048;
    if (minRead == 0)   minRead = 1;

    for (;;) {
        if ((uint16_t)(xb->size - xb->used) < minRead) {
            if (flushXferBuf(xb, f) != 0)
                return -1;
        }
        n = ds->recv(xb->buf + xb->used, (uint16_t)(xb->size - xb->used));
        if (n <= 0) break;
        xb->used += (uint16_t)n;
        *total += (unsigned long)n;
        g_xlRecv++;
        got = 1;
    }
    if (!got) g_xlIdle++;
    if (n < 0) return -2;
    return got;
}

/* Swallow whatever the data connection still holds and throw it away.
 * recv() is what reopens a zero receive window - mTCP re-announces only when
 * the application reads out of the null state - so this is what lets an
 * aborting server flush its pipeline and send its FIN. Without it the close
 * below has nothing to wait for and burns TCP_CLOSE_TIMEOUT.
 * Returns 1 if anything was read. */
int drainDiscard(TcpSocket *ds) {
    int got = 0;
    if (!ds) return 0;
    while (ds->recv(g_fileBuf, g_fileBufSize) > 0) got = 1;
    return got;
}
