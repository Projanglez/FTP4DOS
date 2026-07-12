/*
   NCFTP386 - ftpcli.h
   FTP protocol state machine over mTCP (always PASV mode).

   Deliberately WITHOUT mTCP headers: the TcpSocket pointers are wrapped as
   `void*`. This lets ncftp.cpp / rpanel.cpp use this class without needing
   CFG_H and the mTCP includes. Only ftpcli.cpp pulls in the mTCP headers.
*/

#ifndef FTPCLI_H
#define FTPCLI_H


/* --- Return codes ------------------------------------------------------ */
#define FTP_OK             0
#define FTP_ERR_GENERAL   -1
#define FTP_ERR_DNS       -2   /* Host name could not be resolved        */
#define FTP_ERR_CONNECT   -3   /* TCP connect failed                     */
#define FTP_ERR_TIMEOUT   -4   /* Timed out                              */
#define FTP_ERR_AUTH      -5   /* Login rejected                         */
#define FTP_ERR_PROTO     -6   /* Protocol error / connection lost       */
#define FTP_ERR_DATACONN  -7   /* Data connection failed                 */
#define FTP_ERR_LOCALIO   -8   /* Local file error                       */
#define FTP_ERR_SERVER    -9   /* Server replied with 4xx/5xx            */
#define FTP_ERR_ABORT    -10   /* User aborted the operation             */


/* --- States (see CLAUDE.md) --------------------------------------- */
enum FtpState {
    FTP_DISCONNECTED = 0,
    FTP_CONNECTING,
    FTP_BANNER,
    FTP_AUTH_USER,
    FTP_AUTH_PASS,
    FTP_IDLE,
    FTP_LISTING,
    FTP_TRANSFERRING,
    FTP_ERROR,
    FTP_DISCONNECTING
};


/* Callback: called once per raw text line of a LIST output (without CRLF). */
typedef void (*FtpLineCb)(void *ctx, const char *line);

/* Callback: progress during transfers. total==0 => unknown size.
 * Return value: 0 = continue, non-zero = user requested abort (the transfer
 * is cancelled and retr()/stor() return FTP_ERR_ABORT). */
typedef int (*FtpProgressCb)(void *ctx, unsigned long sofar, unsigned long total);


#define FTP_HOST_MAX    64
#define FTP_LINE_MAX   512   /* must hold a full LIST line incl. long names */
#define FTP_ERRMSG_MAX 160


class FtpClient {
public:
    FtpClient();

    /* One-time mTCP stack initialization (parseEnv + initStack).
       MUST be called once before the first connect().
       Returns FTP_OK or an error code. */
    static int  init_stack(void);
    static void shutdown_stack(void);

    /* Drive the mTCP stack for 'ms' milliseconds without sending anything.
     * Used to "warm up" right after init_stack (let the driver/link settle)
     * before the very first connection is established. */
    static void stack_poll(unsigned ms);

    /* Establish the control connection + log in (USER/PASS). */
    int  connect(const char *host, unsigned port,
                 const char *user, const char *pass);
    void disconnect(void);

    int      is_connected(void) const {
        return state >= FTP_IDLE && state < FTP_DISCONNECTING;
    }
    FtpState get_state(void) const { return state; }

    /* --- Keepalive / idle --- */
    /* Send NOOP (keeps the control connection alive against server idle
     * timeouts). If the connection is lost in the process, disconnects
     * cleanly; returns FTP_OK or an error code. */
    int  noop(void);
    /* Drive the stack once and detect a server-side disconnect (without
     * sending anything). Returns 1 = still connected, 0 = just disconnected. */
    int  idle_drive(void);

    /* Fetch a directory listing. Every raw text line -> cb(ctx, line). */
    int  list(const char *path, FtpLineCb cb, void *ctx);

    /* Navigation on the server. */
    int  change_dir(const char *path);          /* CWD          */
    int  parent_dir(void);                       /* CDUP         */
    int  get_cwd(char *buf, int buflen);         /* PWD -> path  */

    /* Transfers (always binary/TYPE I). */
    int  retr(const char *remote, const char *localpath,
              FtpProgressCb cb, void *ctx);
    int  stor(const char *localpath, const char *remote,
              FtpProgressCb cb, void *ctx);

    /* 1 if a file with this path exists on the server (via SIZE).
     * Used for the overwrite prompt during recursive upload. */
    int  remote_file_exists(const char *path);

    /* File operations on the server. */
    int  make_dir(const char *path);             /* MKD          */
    int  remove_dir(const char *path);           /* RMD          */
    int  remove_file(const char *path);          /* DELE         */
    int  rename(const char *from, const char *to); /* RNFR+RNTO  */

    const char *host_name(void) const  { return hostname; }
    const char *last_error(void) const { return errmsg; }
    int         last_code(void) const  { return lastCode; }

    /* Set the text returned by last_error(). For callers that detect an
     * error outside the protocol layer (e.g. dircopy) but report it through
     * the usual last_error() dialog path. */
    void set_error(const char *msg);

private:
    FtpState state;
    void    *ctrl;                  /* TcpSocket* (opaque)            */
    char     hostname[FTP_HOST_MAX];
    int      lastCode;              /* last 3-digit reply code        */
    char     replyText[FTP_LINE_MAX]; /* text of the last reply line  */
    char     errmsg[FTP_ERRMSG_MAX];

    unsigned char pasvAddr[4];      /* parsed from the 227 reply      */
    unsigned      pasvPort;

    /* --- UTF-8 file names (RFC 2640) --- */
    unsigned char serverUtf8;       /* server announced UTF8 in FEAT   */
    unsigned char replySawUtf8;     /* a line of the last reply had "UTF8" */

    /* --- internal helpers (in ftpcli.cpp) --- */
    int  sendCmd(const char *cmd);                  /* appends CRLF     */
    int  sendCmdArg(const char *cmd, const char *arg);
    int  sendRaw(const char *buf, int len);         /* sends everything */
    /* Code or <0. drainCtx (RETR "150" only) = DrainCtx*: drain the data
     * connection while waiting (no mTCP header needed -> void*). */
    int  readReply(void *drainCtx = 0);
    int  openDataConn(void **dataSockOut);          /* PASV + connect   */
    void closeData(void *dataSock);
    int  simpleCmd(const char *cmd, const char *arg); /* send + reply  */
    /* Discard bytes already buffered on the control connection (before a
     * new command is sent: anything still unread is a stale reply). */
    void flushStaleCtrl(void);
    /* Discard control input until 'quietMs' pass without new data (resync
     * after ABOR / timeout, where pending reply counts are ambiguous). */
    void drainReplies(unsigned quietMs);
    void setError(const char *msg);
    void setErrorReply(const char *prefix);
    /* FEAT probe after login: enables UTF-8 name mode (OPTS UTF8 ON). */
    void probeUtf8(void);
    /* Encode a LOCAL-origin name (typed / local panel, DOS codepage) as
     * UTF-8 for the server if it is in UTF-8 mode. Server-origin wire
     * names pass through unchanged. Returns 'name' or 'buf'. */
    const char *encName(const char *name, char *buf, int bufsz);
};


#endif
