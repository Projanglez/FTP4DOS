/*
   FTP4DOS - httpget.cpp
   Minimal HTTP/1.0 GET over mTCP, for the signed auto-updater.

   Only what the updater needs: resolve, connect, send a request, read the
   headers, read the body. The transfer loop is the one from ftpcli.cpp's
   retr(), because it shares netcore's drainToFile() - see netcore.h for why
   that matters (Silly Window Syndrome on real hardware).

   HTTP/1.0 is requested deliberately. It obliges the server to close the
   connection when the body ends, which makes the "no Content-Length" case
   well-defined, and it rules out chunked transfer-encoding, which is not legal
   in an HTTP/1.0 reply. That removes the single largest chunk of parsing this
   file would otherwise need.

   Reference: mtcp/src/APPS/HTGET/HTGET.CPP (not linkable - file-scope globals
   plus its own main()).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "httpget.h"
#include "netcore.h"
#include "lfn.h"      /* lfn_fopen/lfn_remove: local names may be long */
#include "i18n.h"

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

/* After all system/mTCP headers: short umlaut macros for German text. */
#include "umlaut.h"


/* DNS_TIMEOUT comes from ncftp.cfg (10 s), the same one ftpcli.cpp uses. */
#define HDR_TIMEOUT        20000ul   /* status line + headers must arrive     */
#define HDR_LINE_MAX         256
#define HDR_TOTAL_MAX       8192     /* refuse absurd header blocks           */
#define MAX_REDIRECTS          2

static char g_err[128] = "";
static int  g_status   = 0;

const char *http_last_error(void)  { return g_err; }
int         http_last_status(void) { return g_status; }

static void setErr(const char *msg)
{
    strncpy(g_err, msg, sizeof(g_err) - 1);
    g_err[sizeof(g_err) - 1] = '\0';
}


/* --- Connection setup ---------------------------------------------------- */

static int resolveHost(const char *host, IpAddr_t addr)
{
    int8_t drc = Dns::resolve(host, addr, 1);
    if (drc < 0) {
        setErr(L("Host name too long or invalid", "Hostname zu lang oder ung" ue "ltig"));
        return HTTP_ERR_DNS;
    }
    if (drc != 0) {
        clockTicks_t start = TIMER_GET_CURRENT();
        while (Dns::isQueryPending()) {
            PACKET_PROCESS_SINGLE;
            Arp::driveArp();
            Dns::drivePendingQuery();
            if (elapsedMs(start) > DNS_TIMEOUT) {
                setErr(L("DNS timeout", "DNS-Zeit" ue "berschreitung"));
                return HTTP_ERR_DNS;
            }
        }
        drc = Dns::resolve(host, addr, 0);
        if (drc != 0) {
            setErr(L("Host not found", "Host nicht gefunden"));
            return HTTP_ERR_DNS;
        }
    }
    return HTTP_OK;
}

static int openConn(const char *host, unsigned port, TcpSocket **out)
{
    IpAddr_t addr;
    TcpSocket *s;
    int rc;

    *out = 0;
    rc = resolveHost(host, addr);
    if (rc != HTTP_OK) return rc;

    s = TcpSocketMgr::getSocket();
    if (!s) { setErr(L("No free socket", "Kein freier Socket")); return HTTP_ERR_CONNECT; }
    /* Size this like FTP's DATA connection, not like its control connection:
     * the whole download comes through this one socket. Sizing it at 4 KB -
     * "it is only a control channel, the body goes to g_fileBuf" - was a
     * mistake that cost roughly two orders of magnitude of throughput. */
    if (s->setRecvBuffer(g_tcpBufSize)) {
        TcpSocketMgr::freeSocket(s);
        setErr(L("Out of memory", "Zu wenig Speicher"));
        return HTTP_ERR_CONNECT;
    }
    if (s->connect(nextLocalPort(), addr, (uint16_t)port, CONNECT_TIMEOUT_MS) != 0) {
        s->close();
        TcpSocketMgr::freeSocket(s);
        setErr(L("Cannot reach the update server", "Update-Server nicht erreichbar"));
        return HTTP_ERR_CONNECT;
    }
    *out = s;
    return HTTP_OK;
}

static void closeConn(TcpSocket *s)
{
    if (!s) return;
    s->close();
    TcpSocketMgr::freeSocket(s);
}

static int sendAll(TcpSocket *s, const char *buf, int len)
{
    int off = 0;
    clockTicks_t start = TIMER_GET_CURRENT();
    while (off < len) {
        int16_t n;
        driveStack();
        n = s->send((uint8_t *)(buf + off), (uint16_t)(len - off));
        if (n > 0) { off += n; start = TIMER_GET_CURRENT(); }
        else if (n < 0) return HTTP_ERR_CONNECT;
        else if (elapsedMs(start) > DATA_TIMEOUT_MS) return HTTP_ERR_TIMEOUT;
    }
    return HTTP_OK;
}

static int sendRequest(TcpSocket *s, const char *host, const char *path)
{
    char req[HTTP_PATH_MAX + HTTP_HOST_MAX + 160];

    /* Identity encoding and a closing connection, stated explicitly: this is a
     * 16-bit real-mode client with no gzip and no keep-alive. Asking for
     * exactly what we can handle is cheaper than coping with what we cannot. */
    sprintf(req,
            "GET %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "User-Agent: FTP4DOS\r\n"
            "Accept: */*\r\n"
            "Accept-Encoding: identity\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, host);
    return sendAll(s, req, (int)strlen(req));
}


/* --- Header parsing ------------------------------------------------------ */

/* Reading the header block one byte at a time is what the first version did,
 * on the reasoning that it cannot over-read into the body. It also destroys
 * throughput, and the mechanism is worth spelling out because it is not
 * obvious:
 *
 * mTCP re-announces a closed receive window by exactly the number of bytes the
 * application just read (TcpSocket::recv -> sendPureAck when origWin == 0), and
 * leaves receiver-side SWS avoidance to the application. Once the reply fills
 * the 16 KB socket buffer - which it does immediately, headers and the start of
 * the body arriving together - every single-byte read re-opens the window by
 * ONE BYTE. Measured on a 386: 577 B/s, against 250 KB/s for FTP on the same
 * machine.
 *
 * So read in blocks and keep whatever was over-read: those bytes are simply
 * the first bytes of the body, and the body readers below start from them. */
#define HDR_BUF_SIZE 1024   /* stays <= the smallest possible g_fileBufSize */

struct HttpBuf {
    unsigned char data[HDR_BUF_SIZE];
    uint16_t len;   /* valid bytes                */
    uint16_t pos;   /* next unread byte           */
};

/* Pull more bytes from the socket. 1 = added, 0 = nothing available,
 * -1 = connection closed. */
static int bufFill(HttpBuf *b, TcpSocket *s)
{
    int16_t n;

    /* Reclaim the consumed prefix so the read below stays large. */
    if (b->pos > 0) {
        if (b->len > b->pos)
            memmove(b->data, b->data + b->pos, (size_t)(b->len - b->pos));
        b->len = (uint16_t)(b->len - b->pos);
        b->pos = 0;
    }
    if (b->len >= HDR_BUF_SIZE) return 0;

    n = s->recv(b->data + b->len, (uint16_t)(HDR_BUF_SIZE - b->len));
    if (n > 0) { b->len = (uint16_t)(b->len + n); return 1; }
    return (n < 0) ? -1 : 0;
}

/* Read one CRLF-terminated line out of the buffer, refilling as needed. */
static int readLine(TcpSocket *s, HttpBuf *b, char *line, int maxlen,
                    clockTicks_t deadline)
{
    int len = 0;
    for (;;) {
        while (b->pos < b->len) {
            unsigned char ch = b->data[b->pos++];
            if (ch == '\n') { line[len] = '\0'; return len; }
            if (ch != '\r' && len < maxlen - 1) line[len++] = (char)ch;
        }

        driveStack();
        {
            int r = bufFill(b, s);
            if (r < 0) { line[len] = '\0'; return (len > 0) ? len : -1; }
            if (r == 0) {
                if (s->isRemoteClosed() && !s->recvDataWaiting()) {
                    line[len] = '\0';
                    return (len > 0) ? len : -1;
                }
                if (elapsedMs(deadline) > HDR_TIMEOUT) return -2;
            }
        }
    }
}

struct HttpHeaders {
    int           status;
    unsigned long length;
    int           hasLength;
    int           chunked;
    char          location[HTTP_PATH_MAX + HTTP_HOST_MAX + 8];
};

static int readHeaders(TcpSocket *s, HttpBuf *b, HttpHeaders *h)
{
    char line[HDR_LINE_MAX];
    clockTicks_t start = TIMER_GET_CURRENT();
    int total = 0, n;

    h->status = 0; h->length = 0; h->hasLength = 0; h->chunked = 0;
    h->location[0] = '\0';

    n = readLine(s, b, line, sizeof(line), start);
    if (n == -2) { setErr(L("Server did not reply in time", "Server antwortet nicht rechtzeitig")); return HTTP_ERR_TIMEOUT; }
    if (n < 0)   { setErr(L("Connection closed by server", "Verbindung vom Server geschlossen")); return HTTP_ERR_PROTO; }

    /* "HTTP/1.x NNN reason" - a 1.1 reply to our 1.0 request is normal. */
    if (strncmp(line, "HTTP/", 5) != 0) {
        setErr(L("Not an HTTP reply", "Keine HTTP-Antwort"));
        return HTTP_ERR_PROTO;
    }
    {
        const char *p = strchr(line, ' ');
        if (!p) { setErr(L("Malformed HTTP status line", "Fehlerhafte HTTP-Statuszeile")); return HTTP_ERR_PROTO; }
        h->status = atoi(p + 1);
    }

    for (;;) {
        n = readLine(s, b, line, sizeof(line), start);
        if (n == -2) { setErr(L("Server did not reply in time", "Server antwortet nicht rechtzeitig")); return HTTP_ERR_TIMEOUT; }
        if (n < 0)   { setErr(L("Connection closed by server", "Verbindung vom Server geschlossen")); return HTTP_ERR_PROTO; }
        if (n == 0)  break;                    /* blank line: headers done */

        total += n;
        if (total > HDR_TOTAL_MAX) {
            setErr(L("Header block too large", "Header-Block zu gro" ss));
            return HTTP_ERR_PROTO;
        }

        if (strnicmp(line, "Content-Length:", 15) == 0) {
            h->length = strtoul(line + 15, 0, 10);
            h->hasLength = 1;
        } else if (strnicmp(line, "Transfer-Encoding:", 18) == 0) {
            /* Not legal in an HTTP/1.0 reply, so this only happens if the
             * server ignored our request version. Refuse rather than carry a
             * chunked decoder we would never otherwise exercise. */
            if (strstr(line, "chunked") || strstr(line, "Chunked"))
                h->chunked = 1;
        } else if (strnicmp(line, "Location:", 9) == 0) {
            const char *p = line + 9;
            while (*p == ' ' || *p == '\t') p++;
            strncpy(h->location, p, sizeof(h->location) - 1);
            h->location[sizeof(h->location) - 1] = '\0';
        }
    }
    return HTTP_OK;
}

/* Split "http://host[:port]/path" (or a bare "/path") for redirect handling.
 * Returns HTTP_ERR_TLS for https:// - we cannot follow it, and saying so
 * plainly is more useful than a generic protocol error. */
static int parseUrl(const char *url, char *host, int hostmax,
                    unsigned *port, char *path, int pathmax)
{
    const char *p = url;
    const char *slash;
    int n;

    if (strnicmp(p, "https://", 8) == 0) {
        setErr(L("Update server redirected to HTTPS, which DOS cannot do",
                 "Update-Server leitet auf HTTPS um - unter DOS nicht m" oe "glich"));
        return HTTP_ERR_TLS;
    }

    if (strnicmp(p, "http://", 7) == 0) {
        p += 7;
        slash = strchr(p, '/');
        n = slash ? (int)(slash - p) : (int)strlen(p);
        if (n >= hostmax) { setErr(L("Redirect host too long", "Redirect-Host zu lang")); return HTTP_ERR_PROTO; }
        memcpy(host, p, n);
        host[n] = '\0';
        {
            char *colon = strchr(host, ':');
            if (colon) { *colon = '\0'; *port = (unsigned)atoi(colon + 1); }
        }
        if (slash) {
            if ((int)strlen(slash) >= pathmax) { setErr(L("Redirect path too long", "Redirect-Pfad zu lang")); return HTTP_ERR_PROTO; }
            strcpy(path, slash);
        } else {
            strcpy(path, "/");
        }
        return HTTP_OK;
    }

    /* Relative redirect: same host and port, new path. */
    if (*p != '/') { setErr(L("Unsupported redirect target", "Nicht unterst" ue "tztes Redirect-Ziel")); return HTTP_ERR_PROTO; }
    if ((int)strlen(p) >= pathmax) { setErr(L("Redirect path too long", "Redirect-Pfad zu lang")); return HTTP_ERR_PROTO; }
    strcpy(path, p);
    return HTTP_OK;
}


/* --- Body readers -------------------------------------------------------- */

static int bodyToMem(TcpSocket *s, const HttpHeaders *h, HttpBuf *b,
                     unsigned char *buf, int buflen, int *outlen)
{
    unsigned long total = 0;
    clockTicks_t start = TIMER_GET_CURRENT();

    *outlen = 0;

    /* Whatever the header reader over-read is the start of the body. */
    if (b->pos < b->len) {
        uint16_t left = (uint16_t)(b->len - b->pos);
        if ((long)left > (long)buflen) {
            setErr(L("Reply larger than expected", "Antwort gr" oe ss "er als erwartet"));
            return HTTP_ERR_TOOBIG;
        }
        memcpy(buf, b->data + b->pos, left);
        b->pos = b->len;
        total = left;
        if (h->hasLength && total >= h->length) { *outlen = (int)total; return HTTP_OK; }
    }
    for (;;) {
        int16_t n;
        driveStack();

        if ((int)total >= buflen) {
            /* One more byte would overflow; check whether more is coming. */
            uint8_t probe;
            n = s->recv(&probe, 1);
            if (n == 1) { setErr(L("Reply larger than expected", "Antwort gr" oe ss "er als erwartet")); return HTTP_ERR_TOOBIG; }
            if (s->isRemoteClosed() && !s->recvDataWaiting()) break;
            if (elapsedMs(start) > DATA_TIMEOUT_MS) { setErr(L("Transfer timed out", "Zeit" ue "berschreitung bei der " ue "bertragung")); return HTTP_ERR_TIMEOUT; }
            continue;
        }

        n = s->recv(buf + total, (uint16_t)(buflen - (int)total));
        if (n > 0) {
            total += (unsigned long)n;
            start = TIMER_GET_CURRENT();
            if (h->hasLength && total >= h->length) break;
            continue;
        }
        if (n < 0) break;                       /* closed */
        if (s->isRemoteClosed() && !s->recvDataWaiting()) break;
        if (elapsedMs(start) > DATA_TIMEOUT_MS) {
            setErr(L("Transfer timed out", "Zeit" ue "berschreitung bei der " ue "bertragung"));
            return HTTP_ERR_TIMEOUT;
        }
    }

    if (h->hasLength && total != h->length) {
        setErr(L("Truncated reply", "Unvollst" ae "ndige Antwort"));
        return HTTP_ERR_PROTO;
    }
    *outlen = (int)total;
    return HTTP_OK;
}

static int bodyToFile(TcpSocket *s, const HttpHeaders *h, HttpBuf *b,
                      const char *localpath, HttpProgressCb cb, void *ctx)
{
    FILE *f;
    XferBuf xb;
    unsigned long total = 0;
    clockTicks_t start, lastCb;
    int rc = HTTP_OK;

    f = lfn_fopen(localpath, "wb");
    if (!f) { setErr(L("Cannot create the local file", "Lokale Datei kann nicht angelegt werden")); return HTTP_ERR_LOCALIO; }

    xb.buf = g_fileBuf; xb.size = g_fileBufSize; xb.used = 0;

    /* Whatever the header reader over-read is the start of the body. The
     * header buffer is never larger than the smallest permitted file buffer,
     * so this always fits. */
    if (b->pos < b->len) {
        uint16_t left = (uint16_t)(b->len - b->pos);
        if (left > xb.size) left = xb.size;
        memcpy(xb.buf, b->data + b->pos, left);
        xb.used = left;
        b->pos = (uint16_t)(b->pos + left);
        total = left;
    }
    start = lastCb = TIMER_GET_CURRENT();
    xferLogOpen("HTTP", localpath, h->hasLength ? h->length : 0);

    /* Same shape as ftpcli.cpp's retr() loop, and for the same reasons: drain
     * the socket completely every pass so the receive window never collapses,
     * and poll the callback on a timer so ESC still works on a stalled
     * transfer. */
    for (;;) {
        int dr;
        driveStack();

        dr = drainToFile(s, f, &xb, &total);
        if (dr == -1) { rc = HTTP_ERR_LOCALIO; setErr(L("Write error - disk full?", "Schreibfehler - Platte voll?")); break; }
        if (dr > 0) start = TIMER_GET_CURRENT();

        xferLogSample(total, xb.used);

        if (dr > 0 || elapsedMs(lastCb) >= CB_POLL_MS) {
            lastCb = TIMER_GET_CURRENT();
            if (cb && cb(ctx, total, h->hasLength ? h->length : 0)) {
                rc = HTTP_ERR_ABORT;
                break;
            }
        }

        if (dr == -2) break;                                   /* closed */
        if (s->isRemoteClosed() && !s->recvDataWaiting()) break;
        if (h->hasLength && total >= h->length) break;
        if (elapsedMs(start) > DATA_TIMEOUT_MS) {
            rc = HTTP_ERR_TIMEOUT;
            setErr(L("Transfer timed out", "Zeit" ue "berschreitung bei der " ue "bertragung"));
            break;
        }
    }

    if (rc == HTTP_OK && flushXferBuf(&xb, f) != 0) {
        rc = HTTP_ERR_LOCALIO;
        setErr(L("Write error - disk full?", "Schreibfehler - Platte voll?"));
    }

    xferLogClose(total, rc);
    fclose(f);

    /* A short body is a failed download, not a small one. Without the
     * Content-Length check a truncated transfer would reach the signature
     * stage and fail there with a far more confusing message. */
    if (rc == HTTP_OK && h->hasLength && total != h->length) {
        rc = HTTP_ERR_PROTO;
        setErr(L("Truncated download", "Unvollst" ae "ndiger Download"));
    }
    if (rc == HTTP_OK && cb) cb(ctx, total, h->hasLength ? h->length : total);

    if (rc != HTTP_OK) lfn_remove(localpath);
    return rc;
}


/* --- Public entry points ------------------------------------------------- */

/* Shared driver: connect, request, read headers, follow up to MAX_REDIRECTS
 * same-scheme redirects, then hand the body to one of the readers above. */
static int httpFetch(const char *host, unsigned port, const char *path,
                     unsigned char *membuf, int memlen, int *outlen,
                     const char *localpath, HttpProgressCb cb, void *ctx)
{
    char curHost[HTTP_HOST_MAX], curPath[HTTP_PATH_MAX];
    unsigned curPort = port;
    int hops;

    g_err[0] = '\0';
    g_status = 0;

    if (strlen(host) >= sizeof(curHost)) { setErr(L("Host name too long", "Hostname zu lang")); return HTTP_ERR_DNS; }
    if (strlen(path) >= sizeof(curPath)) { setErr(L("Path too long", "Pfad zu lang")); return HTTP_ERR_PROTO; }
    strcpy(curHost, host);
    strcpy(curPath, path);

    for (hops = 0; ; hops++) {
        TcpSocket *s = 0;
        HttpHeaders h;
        HttpBuf hb;
        int rc;

        hb.len = 0;
        hb.pos = 0;

        rc = openConn(curHost, curPort, &s);
        if (rc != HTTP_OK) return rc;

        rc = sendRequest(s, curHost, curPath);
        if (rc != HTTP_OK) {
            closeConn(s);
            setErr(L("Could not send the request", "Anfrage konnte nicht gesendet werden"));
            return rc;
        }

        rc = readHeaders(s, &hb, &h);
        g_status = h.status;
        if (rc != HTTP_OK) { closeConn(s); return rc; }

        if (h.chunked) {
            closeConn(s);
            setErr(L("Server used chunked encoding", "Server verwendet Chunked-Encoding"));
            return HTTP_ERR_PROTO;
        }

        if (h.status == 301 || h.status == 302 || h.status == 303 ||
            h.status == 307 || h.status == 308) {
            closeConn(s);
            if (hops >= MAX_REDIRECTS) { setErr(L("Too many redirects", "Zu viele Weiterleitungen")); return HTTP_ERR_PROTO; }
            if (!h.location[0])        { setErr(L("Redirect without a target", "Weiterleitung ohne Ziel")); return HTTP_ERR_PROTO; }
            rc = parseUrl(h.location, curHost, sizeof(curHost), &curPort,
                          curPath, sizeof(curPath));
            if (rc != HTTP_OK) return rc;
            continue;
        }

        if (h.status != 200) {
            closeConn(s);
            /* 404 is the everyday case - the server is reachable but carries
             * no published update yet - so say that rather than leaving the
             * user to guess what was not found. */
            if (h.status == 404)
                setErr(L("The update server has no update information (404).\n"
                         "Probably no release has been published yet.",
                         "Der Update-Server h" ae "lt keine Update-Informationen bereit (404).\n"
                         "Vermutlich wurde noch kein Release ver" oe "ffentlicht."));
            else
                sprintf(g_err, L("The update server answered with HTTP %d.",
                                 "Der Update-Server antwortete mit HTTP %d."), h.status);
            return HTTP_ERR_STATUS;
        }

        if (localpath) rc = bodyToFile(s, &h, &hb, localpath, cb, ctx);
        else           rc = bodyToMem(s, &h, &hb, membuf, memlen, outlen);

        closeConn(s);
        return rc;
    }
}

int http_get_mem(const char *host, unsigned port, const char *path,
                 unsigned char *buf, int buflen, int *outlen)
{
    if (outlen) *outlen = 0;
    return httpFetch(host, port, path, buf, buflen, outlen, 0, 0, 0);
}

int http_get_file(const char *host, unsigned port, const char *path,
                  const char *localpath, HttpProgressCb cb, void *ctx)
{
    return httpFetch(host, port, path, 0, 0, 0, localpath, cb, ctx);
}
