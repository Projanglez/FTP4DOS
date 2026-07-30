/* =============================================================================
 * httptest.cpp - standalone DOS harness for src/httpget.cpp
 * -----------------------------------------------------------------------------
 * Exercises the HTTP client on its own, before it is wired into the TUI, so a
 * protocol bug cannot hide behind a dialog bug.
 *
 *   httptest mem  <host> <port> <path>              print the body
 *   httptest file <host> <port> <path> <localfile>  download with progress
 *
 * Needs the mTCP stack, so it borrows FtpClient::init_stack() - the same entry
 * point the application uses, which also means this tests the real
 * configuration path (MTCP.CFG buffer sizes and all).
 * ===========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "../src/httpget.h"
#include "../src/ftpcli.h"

/* Watcom reports "heap is corrupted" only at exit, which says nothing about
 * which step broke it. Checking at each stage turns that into a location. */
static void heapAt(const char *where)
{
    int rc = _heapchk();
    const char *s;
    switch (rc) {
    case _HEAPOK:       s = "ok";          break;
    case _HEAPEMPTY:    s = "empty";       break;
    case _HEAPBADBEGIN: s = "BAD BEGIN";   break;
    case _HEAPBADNODE:  s = "BAD NODE";    break;
    case _HEAPBADPTR:   s = "BAD PTR";     break;
    case _HEAPEND:      s = "bad end";     break;
    default:            s = "?";           break;
    }
    printf("[heap %-22s %s]\n", where, s);
    fflush(stdout);
}

static int progress(void *ctx, unsigned long sofar, unsigned long total)
{
    static unsigned long lastPrint = 0;
    (void)ctx;
    if (sofar - lastPrint >= 8192UL || sofar == total) {
        lastPrint = sofar;
        if (total) printf("\r  %lu / %lu bytes (%lu%%)   ", sofar, total,
                          (sofar * 100UL) / total);
        else       printf("\r  %lu bytes   ", sofar);
        fflush(stdout);
    }
    return 0;   /* never cancel: this harness is not interactive */
}

static const char *rcName(int rc)
{
    switch (rc) {
    case HTTP_OK:           return "OK";
    case HTTP_ERR_DNS:      return "DNS";
    case HTTP_ERR_CONNECT:  return "CONNECT";
    case HTTP_ERR_TIMEOUT:  return "TIMEOUT";
    case HTTP_ERR_PROTO:    return "PROTO";
    case HTTP_ERR_STATUS:   return "STATUS";
    case HTTP_ERR_LOCALIO:  return "LOCALIO";
    case HTTP_ERR_ABORT:    return "ABORT";
    case HTTP_ERR_TOOBIG:   return "TOOBIG";
    case HTTP_ERR_TLS:      return "TLS";
    default:                return "?";
    }
}

int main(int argc, char **argv)
{
    int rc;

    if (argc < 5) {
        fprintf(stderr,
                "usage: httptest mem  <host> <port> <path>\n"
                "       httptest file <host> <port> <path> <localfile>\n");
        return 2;
    }

    heapAt("before init_stack");
    if (FtpClient::init_stack() != FTP_OK) {
        fprintf(stderr, "cannot start the mTCP stack (is MTCPCFG set?)\n");
        return 3;
    }
    heapAt("after init_stack");

    if (strcmp(argv[1], "mem") == 0) {
        unsigned char buf[2048];
        int len = 0;
        rc = http_get_mem(argv[2], (unsigned)atoi(argv[3]), argv[4],
                          buf, (int)sizeof(buf), &len);
        printf("rc=%s status=%d len=%d\n", rcName(rc), http_last_status(), len);
        if (rc == HTTP_OK) {
            int i;
            printf("---\n");
            for (i = 0; i < len; i++) putchar(buf[i]);
            printf("---\n");
        } else {
            printf("error: %s\n", http_last_error());
        }
    } else if (strcmp(argv[1], "file") == 0 && argc >= 6) {
        rc = http_get_file(argv[2], (unsigned)atoi(argv[3]), argv[4], argv[5],
                           progress, 0);
        printf("\nrc=%s status=%d\n", rcName(rc), http_last_status());
        if (rc != HTTP_OK) printf("error: %s\n", http_last_error());
    } else {
        fprintf(stderr, "bad arguments\n");
        rc = HTTP_ERR_PROTO;
    }

    heapAt("after transfer");
    FtpClient::shutdown_stack();
    heapAt("after shutdown_stack");
    return (rc == HTTP_OK) ? 0 : 1;
}
