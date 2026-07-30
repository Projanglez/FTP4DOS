/* =============================================================================
 * httpget.h - minimal HTTP/1.0 GET for FTP4DOS
 * -----------------------------------------------------------------------------
 * Just enough HTTP to fetch the update manifest, its signature and a new
 * executable. Not a general-purpose client: no POST, no authentication, no
 * cookies, no keep-alive, no compression, no TLS.
 *
 * No TLS is the constraint everything else follows from - mTCP has none, so the
 * update channel is plain HTTP and authenticity comes from the RSA signature
 * over the manifest (see rsaverify.h), never from the transport. A redirect to
 * an https:// URL is therefore a hard error rather than something to follow.
 *
 * The transfer loop deliberately reuses netcore.h: drainToFile() is what keeps
 * a download from stalling under TCP Silly Window Syndrome, and that behaviour
 * was established by measurement on real 386 hardware. A separate
 * implementation here would have had to rediscover it.
 *
 * Reference for the protocol handling: mtcp/src/APPS/HTGET/HTGET.CPP.
 * ===========================================================================*/
#ifndef HTTPGET_H
#define HTTPGET_H

#define HTTP_OK            0
#define HTTP_ERR_DNS      -1
#define HTTP_ERR_CONNECT  -2
#define HTTP_ERR_TIMEOUT  -3
#define HTTP_ERR_PROTO    -4   /* malformed reply, or chunked encoding      */
#define HTTP_ERR_STATUS   -5   /* reply was not 200 (see http_last_status)  */
#define HTTP_ERR_LOCALIO  -6
#define HTTP_ERR_ABORT    -7   /* the progress callback asked to stop       */
#define HTTP_ERR_TOOBIG   -8   /* body larger than the caller's buffer      */
#define HTTP_ERR_TLS      -9   /* redirected to https, which we cannot do   */

#define HTTP_HOST_MAX  80
#define HTTP_PATH_MAX 128

/* Same shape as FtpProgressCb (ftpcli.h) so ncftp.cpp's copy_progress() can be
 * handed straight in. Returning non-zero aborts the transfer. 'total' is 0 when
 * the server did not state a Content-Length. */
typedef int (*HttpProgressCb)(void *ctx, unsigned long sofar,
                              unsigned long total);

/* Fetch into memory. Used for the small files - manifest and signature.
 * 'outlen' receives the number of bytes stored. Returns HTTP_OK or an error. */
int http_get_mem(const char *host, unsigned port, const char *path,
                 unsigned char *buf, int buflen, int *outlen);

/* Fetch to a file, with progress and cancellation. Used for the executable. */
int http_get_file(const char *host, unsigned port, const char *path,
                  const char *localpath, HttpProgressCb cb, void *ctx);

/* Human-readable detail for the last failure, and the HTTP status code if one
 * was received (0 otherwise). */
const char *http_last_error(void);
int         http_last_status(void);

#endif /* HTTPGET_H */
