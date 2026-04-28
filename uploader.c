/* Background uploader.
 *
 * `ps_uploader_enqueue()` is called from the screenshot-encode hook
 * with a pointer to the just-encoded PNG. It must return quickly —
 * we don't want SceShell's encode path to block on network I/O. The
 * buffer is copied into a fresh SceKernelAllocMemBlock and handed to
 * a dedicated worker thread via a bounded ring + semaphore; the
 * worker does the HTTP POST.
 *
 * Upload format (sys-screenuploader):
 *   POST <upload_url with {filename} expanded>
 *   Content-Type: image/png
 *   <raw PNG body>
 *
 * The dispatch queue is bounded (PS_UPLOAD_QUEUE); if it's full, the
 * new screenshot is dropped with a log line — better than blocking
 * the Vita UI. In practice the queue never fills because screenshots
 * are a human-initiated event, not a stream. */

#include "pngshot.h"

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/net/http.h>
#include <psp2/libssl.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/message_dialog.h>
#include <psp2/common_dialog.h>


/* ------------------------------------------------------------------ */
/* Dedicated MemBlock allocator for upload bodies. A Vita screenshot  */
/* is typically ~1 MB of PNG — too large to squeeze through ScePaf's  */
/* small pool reliably.                                               */
/* ------------------------------------------------------------------ */
static void *big_alloc(size_t size, SceUID *out_blk) {
    size_t a = (size + 4095u) & ~4095u;
    SceUID blk = sceKernelAllocMemBlock("pngshotssu",
                                        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                        a, NULL);
    if (blk < 0) {
        vlog("big_alloc %u failed 0x%08X", (unsigned)a, blk);
        *out_blk = -1;
        return NULL;
    }
    void *p = NULL;
    sceKernelGetMemBlockBase(blk, &p);
    if (!p) { sceKernelFreeMemBlock(blk); *out_blk = -1; return NULL; }
    *out_blk = blk;
    return p;
}
static void big_free(SceUID blk) { if (blk >= 0) sceKernelFreeMemBlock(blk); }

/* ------------------------------------------------------------------ */
/* User-feedback popup — implemented as SceMsgDialog.                 */
/*                                                                    */
/* The "right" mechanism would be sceNotificationUtilSendNotification  */
/* (top-right trophy-style bubble), but Photos isn't on Sony's allow-  */
/* list for that API: the call returns 0x80106301 INTERNAL no matter  */
/* what init dance we do, and skipping init crashes the process.      */
/*                                                                    */
/* MsgDialog is a fallback that any foreground app can use, including */
/* Photos — it's the same widget Photos uses for its own "Are you     */
/* sure you want to delete?" prompts. It's modal (~1 s of UI block    */
/* while user dismisses with X), but visible feedback for an upload   */
/* that the user explicitly initiated via the Share menu is exactly   */
/* what the task calls for.                                           */
/*                                                                    */
/* Render order:                                                      */
/*   sceMsgDialogInit(USER_MSG, OK button, no extras) — pushes the    */
/*   dialog onto Photos' commonDialog stack. Photos calls             */
/*   sceCommonDialogUpdate() every frame, which actually renders our  */
/*   dialog without us having to touch GXM.                           */
/*                                                                    */
/* We then poll sceMsgDialogGetStatus until FINISHED (user hit X) or  */
/* a 30-second safety timeout, then sceMsgDialogTerm to release it.   */
/*                                                                    */
/* MsgDialog is single-instance per process, so we serialize calls    */
/* on a tiny mutex — two back-to-back uploads' toasts won't stomp     */
/* each other. */
static int    g_dlg_inited = 0;
static SceUID g_dlg_mtx    = -1;

static void notify_ensure(void) {
    if (g_dlg_inited) return;
    g_dlg_mtx = sceKernelCreateMutex("pngshotssu_dlg", 0, 0, NULL);
    g_dlg_inited = 1;
}

void ps_notify(const char *text) {
    if (!text) return;
    notify_ensure();
    if (g_dlg_mtx < 0) return;

    sceKernelLockMutex(g_dlg_mtx, 1, NULL);

    SceMsgDialogUserMessageParam user;
    ps_memset(&user, 0, sizeof(user));
    user.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    user.msg        = (const SceChar8 *)text;

    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode         = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &user;

    int rc = sceMsgDialogInit(&param);
    if (rc < 0) {
        /* If MsgDialog is already up (another in-app prompt) this
         * returns 0x80100A01. Not fatal — log and bail. */
        vlog("notify: MsgDialogInit 0x%08X", rc);
        sceKernelUnlockMutex(g_dlg_mtx, 1);
        return;
    }

    /* Wait for the user to dismiss, with a hard cap so we never
     * leak the dialog if Photos somehow stops calling
     * sceCommonDialogUpdate. 30 s @ 30 ms = 1000 polls. */
    for (int i = 0; i < 1000; i++) {
        SceCommonDialogStatus st = sceMsgDialogGetStatus();
        if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) break;
        sceKernelDelayThread(30 * 1000);
    }
    /* Per the SDK contract: a dialog whose status hit FINISHED must
     * be transitioned to NONE before the next sceMsgDialogInit, or
     * we get SCE_ERROR_ERRNO_EBUSY (0x80020401). The "Close →
     * frame-tick → Term" sequence used by Sony's own samples works
     * reliably: Close moves FINISHED → CLOSED, the next
     * sceCommonDialogUpdate (driven by Photos) drains it, Term
     * finally returns the slot to NONE. We then poll once more to
     * confirm so the next caller sees a clean state. */
    sceMsgDialogClose();
    /* Give Photos' draw loop a couple of frames to actually flush. */
    for (int i = 0; i < 200; i++) {
        if (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_RUNNING)
            break;
        sceKernelDelayThread(16 * 1000);   /* ~one frame at 60 Hz */
    }
    sceMsgDialogTerm();
    /* And wait for the slot to actually go NONE. Belt-and-braces. */
    for (int i = 0; i < 200; i++) {
        if (sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_NONE)
            break;
        sceKernelDelayThread(16 * 1000);
    }

    sceKernelUnlockMutex(g_dlg_mtx, 1);
}

/* ------------------------------------------------------------------ */
/* Progress dialog                                                    */
/*                                                                    */
/* SceMsgDialog `PROGRESS_BAR` mode with the only legal barType       */
/* (PERCENTAGE = 0) gives us a no-button modal with a custom title    */
/* and a percent bar underneath. We never advance the bar — leaving   */
/* it at 0% with a "Uploading screenshot..." title is the closest     */
/* user-mode equivalent of an indeterminate "please wait" widget.     */
/* (Sony's SCE_MSG_DIALOG_SYSMSG_TYPE_WAIT is the proper indeter-     */
/* minate spinner, but its message text is fixed to "Please wait."    */
/* and not customisable.)                                             */
/*                                                                    */
/* Locking: shares g_dlg_mtx with ps_notify so a final-result dialog */
/* can't race the still-up progress dialog. ps_progress_show acquires */
/* the lock and *holds it* until ps_progress_hide releases it.        */
/* ------------------------------------------------------------------ */

static int s_progress_up = 0;

void ps_progress_show(const char *text) {
    if (!text) return;
    notify_ensure();
    if (g_dlg_mtx < 0) return;

    sceKernelLockMutex(g_dlg_mtx, 1, NULL);
    /* Belt-and-braces: if a previous progress dialog is somehow still
     * marked active (e.g. caller forgot ps_progress_hide), drop it
     * before raising a new one. */
    if (s_progress_up) {
        sceMsgDialogClose();
        for (int i = 0; i < 200; i++) {
            if (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_RUNNING) break;
            sceKernelDelayThread(16 * 1000);
        }
        sceMsgDialogTerm();
        for (int i = 0; i < 200; i++) {
            if (sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_NONE) break;
            sceKernelDelayThread(16 * 1000);
        }
        s_progress_up = 0;
    }

    SceMsgDialogProgressBarParam pb;
    ps_memset(&pb, 0, sizeof(pb));
    /* The only valid `barType` is PERCENTAGE (0); other values fail
     * with SCE_MSG_DIALOG_ERROR_PARAM (22). We never call
     * sceMsgDialogProgressBarSetValue, so the bar stays at 0% — the
     * dialog's title (`msg`) is what the user actually reads, and
     * the static bar-at-0 is acceptable for an "in-progress" hint
     * since the upload itself is opaque (sceHttpSendRequest is
     * blocking and emits no progress callbacks). */
    pb.barType = SCE_MSG_DIALOG_PROGRESSBAR_TYPE_PERCENTAGE;
    pb.msg     = (const SceChar8 *)text;

    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode             = SCE_MSG_DIALOG_MODE_PROGRESS_BAR;
    param.progBarParam     = &pb;

    int rc = sceMsgDialogInit(&param);
    if (rc < 0) {
        vlog("progress: MsgDialogInit 0x%08X", rc);
        sceKernelUnlockMutex(g_dlg_mtx, 1);
        return;
    }
    s_progress_up = 1;
    /* Mutex stays locked until ps_progress_hide. */
}

void ps_progress_hide(void) {
    if (g_dlg_mtx < 0) return;
    if (!s_progress_up) {
        /* Nothing to do, but make sure we don't leak the lock if the
         * caller pairs hide() with a show() that was bypassed (e.g.
         * MsgDialogInit failed). */
        return;
    }

    sceMsgDialogClose();
    for (int i = 0; i < 200; i++) {
        if (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_RUNNING) break;
        sceKernelDelayThread(16 * 1000);
    }
    sceMsgDialogTerm();
    for (int i = 0; i < 200; i++) {
        if (sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_NONE) break;
        sceKernelDelayThread(16 * 1000);
    }
    s_progress_up = 0;
    sceKernelUnlockMutex(g_dlg_mtx, 1);
}

/* ------------------------------------------------------------------ */
/* Queue                                                              */
/* ------------------------------------------------------------------ */

/* A queued upload comes in one of two flavours:
 *
 *   in-RAM (Photos share): the caller already has the PNG buffered
 *     and we copy it into our own MemBlock at enqueue time. `path`
 *     is empty.
 *
 *   on-disk (SceShell encode hook / Photos share): the caller hands
 *     us a path it already owns. `body`/`blk` stay unset; the worker
 *     streams the file directly off disk through a 32 KB scratch
 *     buffer, so we never need a contiguous body allocation at all.
 *     The file is left in place — every current caller's path is
 *     either SceShell's persistent capture.png or the user's gallery
 *     photo, neither of which we should be deleting.
 */
typedef struct {
    SceUID      blk;       /* MemBlock holding the body, -1 for file mode */
    void       *body;      /* base pointer inside blk, NULL for file mode */
    int         len;
    int         notify;    /* if non-zero: pop a toast on success/failure */
    char        path[128]; /* staging file, empty string for in-RAM mode */
    SceDateTime stamp;     /* capture time, for filename synthesis */
} job_t;



static SceUID g_pipe_mtx = -1;     /* protects ring */
static SceUID g_pipe_sem = -1;     /* counts queued jobs */
static job_t  g_ring[PS_UPLOAD_QUEUE];
static int    g_ring_head  = 0;
static int    g_ring_tail  = 0;
static int    g_ring_count = 0;

static SceUID g_thid    = -1;
static int    g_running = 0;

/* ------------------------------------------------------------------ */
/* HTTP init                                                          */
/* ------------------------------------------------------------------ */
static int g_http_inited = 0;

/* Accept-all SSL callback, used when the user chose to point us at an
 * https:// upload_url. We *cannot* validate modern Let's Encrypt /
 * ISRG Root X1 chains — the Vita's CA bundle is older than that root.
 * This is safe only because the user explicitly picked their own
 * endpoint; no sensitive credentials are in-flight (the body is just
 * their own screenshot). */
static int https_accept_all(unsigned int verifyEsrr, SceSslCert * const *sslCert,
                            int certNum, void *userArg) {
    (void)verifyEsrr; (void)sslCert; (void)certNum; (void)userArg;
    return 0;
}

static int http_ensure(void) {
    if (g_http_inited) return 0;

    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTP);
    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);
    sceSysmoduleLoadModule(SCE_SYSMODULE_SSL);

    /* SceShell has typically already done sceHttpInit for its own
     * networking. Trying again returns SCE_HTTP_ERROR_ALREADY_INITED
     * (0x80431020) — treat as success. */
    int r = sceHttpInit(1 * 1024 * 1024);
    if (r < 0 && r != (int)0x80431020) {
        vlog("sceHttpInit failed: 0x%08X", r);
        return -1;
    }

    /* Disable every TLS sanity check globally. Some flags are
     * unsupported on older firmwares (returns 0x8043506B) — not
     * fatal, we still install the per-template accept-all callback. */
    int f = SCE_HTTPS_FLAG_SERVER_VERIFY
          | SCE_HTTPS_FLAG_CLIENT_VERIFY
          | SCE_HTTPS_FLAG_CN_CHECK
          | SCE_HTTPS_FLAG_NOT_AFTER_CHECK
          | SCE_HTTPS_FLAG_NOT_BEFORE_CHECK
          | SCE_HTTPS_FLAG_KNOWN_CA_CHECK;
    int dr = sceHttpsDisableOption((unsigned int)f);
    if (dr < 0) vlog("sceHttpsDisableOption 0x%08X (ignored)", dr);
    g_http_inited = 1;
    return 0;
}


/* ------------------------------------------------------------------ */
/* sys-screenuploader filename synthesis                               */
/*   ^(\d{16})-([0-9A-F]{32})\.png$                                    */
/*    ↑ YYYYMMDDhhmmssXX       ↑ arbitrary 32 hex chars; the server    */
/*                               uses this to look up a title row.    */
/*                               32 zeros is fine; server falls back   */
/*                               to "[Unknown application]".          */
/* ------------------------------------------------------------------ */
static void synth_filename(const SceDateTime *t, char out[64]) {
    ps_snprintf(out, 64,
                "%04u%02u%02u%02u%02u%02u00-"
                "00000000000000000000000000000000.png",
                t->year, t->month, t->day,
                t->hour, t->minute, t->second);
}

/* ------------------------------------------------------------------ */
/* URL templating: replace {filename} once.                           */
/* ------------------------------------------------------------------ */
static char *ps_strstr(const char *hay, const char *needle) {
    size_t nl = ps_strlen(needle);
    if (!nl) return (char *)hay;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] == needle[i]) i++;
        if (i == nl) return (char *)p;
    }
    return 0;
}

static void url_with_filename(const char *tmpl, const char *fname,
                              char *out, int out_cap) {
    const char *key = "{filename}";
    char *found = ps_strstr(tmpl, key);
    if (!found) {
        size_t n = ps_strlen(tmpl);
        if ((int)n >= out_cap) n = out_cap - 1;
        ps_memcpy(out, tmpl, n);
        out[n] = '\0';
        return;
    }
    int prefix = (int)(found - tmpl);
    if (prefix >= out_cap) prefix = out_cap - 1;
    ps_memcpy(out, tmpl, prefix);
    ps_snprintf(out + prefix, out_cap - prefix,
                "%s%s", fname, found + ps_strlen(key));
}

/* ------------------------------------------------------------------ */
/* Streaming HTTP POST over a raw socket                              */
/*                                                                    */
/* sceHttp wants the whole request body in one contiguous buffer,    */
/* which is unworkable from inside the Photos process — its          */
/* `ddrmain: NPXS10004` partition is so small that a single 1 MB    */
/* allocation crashes the second upload. We bypass sceHttp entirely  */
/* for file-mode jobs and write a minimal HTTP/1.1 POST onto a raw   */
/* sceNetSocket, looping the body through a fixed 32 KB scratch      */
/* buffer pulled from disk. Total RAM = ~32 KB regardless of file    */
/* size.                                                             */
/*                                                                    */
/* This deliberately does *not* support https://. TLS would require  */
/* mbedTLS-class crypto we don't have a streaming primitive for, and */
/* the user's config already prefers plain http (the Vita CA bundle  */
/* predates Let's Encrypt anyway).                                   */
/*                                                                    */
/* Same return-code contract as send_buffer():                       */
/*    0 : ok                                                          */
/*   -1 : permanent failure                                          */
/*   -2 : transient failure                                          */
/* ------------------------------------------------------------------ */

/* Tiny URL splitter — handles only http://host[:port][/path]. */
static int parse_http_url(const char *url,
                          char *host, int host_cap,
                          char *path, int path_cap,
                          int *port_out) {
    const char *p = url;
    if (sceClibStrncmp(p, "http://", 7) != 0) return -1;
    p += 7;
    /* host (until ':' / '/' / end) */
    int hi = 0;
    while (*p && *p != ':' && *p != '/' && hi < host_cap - 1) host[hi++] = *p++;
    host[hi] = '\0';
    if (hi == 0) return -1;
    /* port */
    int port = 80;
    if (*p == ':') {
        p++;
        port = 0;
        while (*p >= '0' && *p <= '9') { port = port * 10 + (*p - '0'); p++; }
        if (port <= 0 || port > 65535) return -1;
    }
    *port_out = port;
    /* path */
    if (*p == '\0') {
        path[0] = '/';
        path[1] = '\0';
    } else {
        int pi = 0;
        while (*p && pi < path_cap - 1) path[pi++] = *p++;
        path[pi] = '\0';
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* System HTTP proxy lookup                                            */
/*                                                                    */
/* The Vita's connection profile (Settings → Network) lets the user   */
/* configure an HTTP proxy. sceHttp honours that automatically;       */
/* since we bypass sceHttp on the streaming path, we have to read     */
/* the same NetCtl info ourselves and tunnel through it. We treat     */
/* http_proxy_config != 0 as "proxy enabled" (the value is 1 = auto / */
/* 2 = manual on retail; both surface usable host:port tuples).       */
/*                                                                    */
/* When a proxy is in effect, we still TCP-connect to the proxy       */
/* host:port (instead of the upstream origin) and write a so-called   */
/* "absolute-form" request line ("POST http://origin/path HTTP/1.1"  */
/* with the proxy's Host header) — this is the classic forward-proxy */
/* convention and what mitmproxy / Squid / etc. expect.               */
/* ------------------------------------------------------------------ */
typedef struct {
    int  in_use;
    char host[256];
    int  port;
} ps_proxy_t;

static int proxy_lookup(ps_proxy_t *out) {
    out->in_use = 0;
    out->host[0] = '\0';
    out->port = 0;

    SceNetCtlInfo info;
    ps_memset(&info, 0, sizeof(info));
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_HTTP_PROXY_CONFIG, &info) < 0)
        return 0;
    if (info.http_proxy_config == 0) return 0;   /* proxy disabled */

    ps_memset(&info, 0, sizeof(info));
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_HTTP_PROXY_SERVER, &info) < 0)
        return 0;
    if (info.http_proxy_server[0] == '\0') return 0;

    size_t n = ps_strlen(info.http_proxy_server);
    if (n >= sizeof(out->host)) n = sizeof(out->host) - 1;
    ps_memcpy(out->host, info.http_proxy_server, n);
    out->host[n] = '\0';

    ps_memset(&info, 0, sizeof(info));
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_HTTP_PROXY_PORT, &info) < 0)
        return 0;
    out->port = (int)info.http_proxy_port;
    if (out->port <= 0 || out->port > 65535) out->port = 8080; /* sane default */

    out->in_use = 1;
    vlog("proxy: using %s:%d", out->host, out->port);
    return 1;
}

/* Resolve a hostname or accept a literal IPv4 string. Returns 0 on
 * success and fills *out. */
static int resolve_host(const char *host, SceNetInAddr *out) {

    /* Try literal IP first. */
    if (sceNetInetPton(SCE_NET_AF_INET, host, out) > 0) return 0;
    /* Otherwise resolver. Pre-allocate a small workspace; ScePaf is
     * fine here because the resolver itself is tiny. */
    int rid = sceNetResolverCreate("pngshotssu_res", NULL, 0);
    if (rid < 0) {
        vlog("resolver create 0x%08X", rid);
        return -1;
    }
    int rc = sceNetResolverStartNtoa(rid, host, out, 0, 0, 0);
    sceNetResolverDestroy(rid);
    if (rc < 0) {
        vlog("resolver ntoa(%s) 0x%08X", host, rc);
        return -1;
    }
    return 0;
}

/* Robust send: keeps calling sceNetSend until everything's out the
 * socket or we hit an error. Handles EAGAIN/EWOULDBLOCK by yielding
 * briefly and retrying — even though we set SNDTIMEO, sceNet has
 * been observed returning SCE_NET_ERROR_EAGAIN (0x80410123) on the
 * Vita's small kernel send buffer when the remote is slow to ACK,
 * so we treat it as "back-pressure, try again" rather than fatal.
 * Total wait per call is bounded by the SNDTIMEO we set when the
 * socket was created (30 s); past that we give up. */
static int net_send_all(int sock, const void *buf, int len) {
    int sent = 0;
    int retries = 0;
    while (sent < len) {
        int n = sceNetSend(sock, (const char *)buf + sent, len - sent, 0);
        if (n > 0) {
            sent += n;
            retries = 0;
            continue;
        }
        /* Errno-style payload lives in the low byte of the SCE_NET
         * error code. 0x80410123 → errno 0x23 = EAGAIN. The kernel
         * send buffer is full; the right move is to wait for ACKs
         * to drain and retry. */
        unsigned err = (unsigned)n;
        int errno_ = err & 0xFFu;
        if (n < 0 && (errno_ == 0x23 /* EAGAIN/EWOULDBLOCK */ ||
                      errno_ == 0x04 /* EINTR */)) {
            /* Cap total backoff at ~30 s (300 * 100 ms) so a peer
             * that stops reading can't pin the worker thread. */
            if (retries++ > 300) {
                vlog("sceNetSend stalled after %d retries at %d/%d",
                     retries, sent, len);
                return -1;
            }
            sceKernelDelayThread(100 * 1000);
            continue;
        }
        vlog("sceNetSend %d/%d -> 0x%08X", sent, len, (unsigned)n);
        return -1;
    }
    return 0;
}


#define PS_STREAM_CHUNK   (32 * 1024)

/* Read the file in PS_STREAM_CHUNK pieces and ship each one over the
 * socket. We allocate the chunk buffer with sce_paf_malloc — it's
 * small enough (32 KB) to fit even the tightest partition and there's
 * no upside to reusing a static buffer when we expect at most a few
 * uploads in flight. */
static int net_send_file(int sock, const char *path, int len) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        vlog("net_send_file: open %s 0x%08X", path, fd);
        return -1;
    }
    /* Stack-resident scratch buffer. The uploader thread is started
     * with a 0x20000 (128 KB) stack — 32 KB is well within budget,
     * and keeping the buffer on-stack means we never touch ScePaf
     * (which is pathologically tight inside the Photos process). */
    static char chunk[PS_STREAM_CHUNK];
    int sent = 0;
    int rc = 0;
    while (sent < len) {
        int want = len - sent;
        if (want > PS_STREAM_CHUNK) want = PS_STREAM_CHUNK;
        int got = sceIoRead(fd, chunk, want);
        if (got <= 0) { vlog("net_send_file: read short at %d", sent); rc = -1; break; }
        if (net_send_all(sock, chunk, got) < 0) { rc = -1; break; }
        sent += got;
    }
    sceIoClose(fd);
    return rc;
}


/* Read the response status line, then walk through the headers,
 * and finally capture the first chunk of the body into `body_buf`
 * (NUL-terminated, truncated to body_cap-1). The body snippet is
 * what we surface to the user and the log when the server returns
 * a non-2xx status — sys-screenuploader's API typically returns
 * a plain-text reason like "rate limit exceeded" or "image too
 * big", and exposing that is far more useful than a bare status
 * code.
 *
 *   status_out:    HTTP status code (e.g. 200, 413, 502)
 *   body_buf/cap:  destination for first body bytes; cap >= 1
 *
 * Returns 0 on a parseable HTTP response (any status), or -1 if
 * the response is malformed / connection broke before status.
 *
 * We pull bytes one at a time through sceNetRecv for the line-
 * delimited prefix (status + headers); switching to bulk reads
 * for the body. The status-line loop is fine to keep byte-by-byte
 * — Vita has a TCP-level receive buffer in front of us, so this
 * isn't actually one syscall per byte over the wire. */
static int read_http_response(int sock, int *status_out,
                              char *body_buf, int body_cap) {
    if (body_cap > 0) body_buf[0] = '\0';

    /* --- status line -------------------------------------------- */
    char line[256];
    int total = 0;
    while (total < (int)sizeof(line) - 1) {
        int n = sceNetRecv(sock, line + total, 1, 0);
        if (n <= 0) {
            vlog("read_http_response: recv %d at status-line byte %d",
                 n, total);
            return -1;
        }
        total += n;
        if (total >= 2 && line[total - 2] == '\r' && line[total - 1] == '\n')
            break;
    }
    line[total] = '\0';

    /* "HTTP/1.x SSS Reason\r\n" — locate the status code. */
    int sp = 0;
    while (line[sp] && line[sp] != ' ') sp++;
    if (!line[sp]) return -1;
    int code = 0;
    sp++;
    while (line[sp] >= '0' && line[sp] <= '9') {
        code = code * 10 + (line[sp] - '0');
        sp++;
    }
    if (code <= 0) return -1;
    *status_out = code;

    /* --- headers ------------------------------------------------ */
    /* Walk header lines until we hit a blank line. Track
     * Content-Length so we know how much body to pull (capped at
     * body_cap-1; we drop the rest on the floor). Other headers are
     * ignored; we already asked for "Connection: close" so we don't
     * need to honour Transfer-Encoding either. */
    int content_length = -1;
    for (;;) {
        total = 0;
        while (total < (int)sizeof(line) - 1) {
            int n = sceNetRecv(sock, line + total, 1, 0);
            if (n <= 0) {
                /* No more data — this is fine for very short error
                 * responses that hang up immediately. We just won't
                 * have a body to show. */
                return 0;
            }
            total += n;
            if (total >= 2 && line[total - 2] == '\r' && line[total - 1] == '\n')
                break;
        }
        line[total] = '\0';

        /* End-of-headers marker (a bare CRLF). */
        if (total <= 2) break;

        /* Case-insensitive match on "Content-Length:". */
        const char *cl = "content-length:";
        int i = 0;
        while (cl[i] && line[i]) {
            char a = line[i]; if (a >= 'A' && a <= 'Z') a += 32;
            if (a != cl[i]) break;
            i++;
        }
        if (!cl[i]) {
            /* Skip whitespace, parse decimal. */
            int p = i;
            while (line[p] == ' ' || line[p] == '\t') p++;
            int v = 0;
            while (line[p] >= '0' && line[p] <= '9') {
                v = v * 10 + (line[p] - '0');
                p++;
            }
            content_length = v;
        }
    }

    /* --- body snippet ------------------------------------------- */
    if (body_cap <= 1) return 0;
    int want = content_length >= 0 ? content_length : (body_cap - 1);
    if (want > body_cap - 1) want = body_cap - 1;
    int got = 0;
    while (got < want) {
        int n = sceNetRecv(sock, body_buf + got, want - got, 0);
        if (n <= 0) break;
        got += n;
    }
    body_buf[got] = '\0';

    /* Strip trailing whitespace / CRLF for cleaner log + UI. */
    while (got > 0 && (body_buf[got - 1] == '\r' || body_buf[got - 1] == '\n' ||
                       body_buf[got - 1] == ' '  || body_buf[got - 1] == '\t')) {
        body_buf[--got] = '\0';
    }
    return 0;
}


/* ------------------------------------------------------------------ */
/* JSON status check                                                   */
/*                                                                    */
/* sys-screenuploader's API always returns HTTP 200 — successes and   */
/* failures alike are signalled in the JSON body:                     */
/*                                                                    */
/*   ok:    {"status":"ok"}                                           */
/*   fail:  {"status":"error","message":"invalid image file"}         */
/*                                                                    */
/* So even though the transport-level status code is fine, we still   */
/* have to peek inside the body to know if the upload actually        */
/* worked. The parser is a deliberately tiny, allocation-free state   */
/* machine: locate the *value* of a top-level string key, copy it     */
/* (with minimal escape handling) into a caller buffer.               */
/*                                                                    */
/* This isn't a full JSON parser — it does *not* handle nested        */
/* objects, comments, unicode escapes, or numeric values — but it's   */
/* completely sufficient for screenuploader's flat reply shape, and   */
/* any malformed body just falls through to "unknown error" handling. */
/* ------------------------------------------------------------------ */
static int json_field(const char *body, const char *key,
                      char *out, int out_cap) {
    if (out_cap > 0) out[0] = '\0';
    /* Build the search needle as `"<key>"` so we don't false-match
     * a substring of some other field. We accept any whitespace
     * between the colon and the opening quote. */
    char needle[64];
    int kl = (int)ps_strlen(key);
    if (kl + 3 > (int)sizeof(needle)) return -1;
    needle[0] = '"';
    ps_memcpy(needle + 1, key, kl);
    needle[1 + kl] = '"';
    needle[2 + kl] = '\0';

    const char *p = ps_strstr(body, needle);
    if (!p) return -1;
    p += ps_strlen(needle);
    /* Skip whitespace + the colon. */
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return -1;        /* only string values supported */
    p++;

    /* Copy chars until the closing quote, decoding the handful of
     * escape sequences we expect to see in error messages. Stops
     * cleanly on truncation. */
    int oi = 0;
    while (*p && *p != '"' && oi < out_cap - 1) {
        if (*p == '\\' && p[1]) {
            char esc = p[1];
            char dec = esc;
            if      (esc == 'n')  dec = '\n';
            else if (esc == 't')  dec = '\t';
            else if (esc == 'r')  dec = '\r';
            else if (esc == '"')  dec = '"';
            else if (esc == '\\') dec = '\\';
            else if (esc == '/')  dec = '/';
            /* Anything else (including \uXXXX) we just pass the
             * literal escape character through; not perfect but
             * fine for a one-line user toast. */
            out[oi++] = dec;
            p += 2;
            continue;
        }
        out[oi++] = *p++;
    }
    out[oi] = '\0';
    return 0;
}

/* `resp_out`/`resp_cap` (optional, may be NULL/0) receives a NUL-
 * terminated snippet of the HTTP response body, useful for showing
 * the server's human-readable error reason in the user toast. */

static int send_file_streamed(const ps_config_t *cfg,
                              const char *body_path, int body_len,
                              const SceDateTime *stamp,
                              int *err_out,
                              char *resp_out, int resp_cap) {

    if (err_out) *err_out = 0;
    if (!cfg->enabled) return 0;
    vlog("stream: begin path=%s len=%d", body_path, body_len);

    /* Make sure libSceNet (and its dependencies) are loaded into our
     * process. SceShell already did this for its own sceHttp use,
     * but we still need sceHttpInit to set up the memory pool that
     * sceNetSocket/sceNetResolver* use under the hood. http_ensure
     * is idempotent and cheap when already-initialised. */
    if (http_ensure() < 0) {
        vlog("stream: http_ensure failed");
        if (err_out) *err_out = -1;
        return -2;
    }

    /* Synthesise the {filename}-expanded URL just like the sceHttp
     * sender does. */
    char fname[64];
    synth_filename(stamp, fname);
    char url[1024];
    url_with_filename(cfg->upload_url, fname, url, sizeof(url));
    vlog("stream: url=%s", url);

    /* https:// is intentionally unsupported — see comment above. */
    if (sceClibStrncmp(url, "https://", 8) == 0) {
        vlog("stream: https not supported, please use http://");
        if (err_out) *err_out = (int)0x80620100u;
        return -1;
    }

    char origin_host[256], req_path[1024];
    int origin_port = 80;
    if (parse_http_url(url, origin_host, sizeof(origin_host),
                       req_path, sizeof(req_path), &origin_port) < 0) {
        vlog("stream: bad URL %s", url);
        if (err_out) *err_out = (int)0x80620101u;
        return -1;
    }
    vlog("stream: parsed host=%s port=%d path=%s",
         origin_host, origin_port, req_path);

    /* If the user configured a system HTTP proxy in Settings →
     * Network we tunnel through it (same as sceHttp does). The
     * upstream URL & Host header still refer to the *origin* —
     * proxies expect absolute-form request lines and the origin's
     * Host header for routing/Vary cache keys. */
    ps_proxy_t proxy;
    proxy_lookup(&proxy);
    const char *connect_host = proxy.in_use ? proxy.host    : origin_host;
    int         connect_port = proxy.in_use ? proxy.port    : origin_port;

    SceNetInAddr addr;
    if (resolve_host(connect_host, &addr) < 0) {
        if (err_out) *err_out = (int)0x80620102u;
        return -2;
    }
    vlog("stream: resolved %s -> 0x%08X",
         connect_host, (unsigned)addr.s_addr);

    int sock = sceNetSocket("pngshotssu_s", SCE_NET_AF_INET,
                            SCE_NET_SOCK_STREAM, 0);
    if (sock < 0) {
        vlog("sceNetSocket 0x%08X", sock);
        if (err_out) *err_out = sock;
        return -2;
    }
    vlog("stream: socket=%d", sock);


    /* Bound the send/recv waits so a half-open socket can't hang the
     * worker forever. 30 s is generous for a 1 MB upload over Vita
     * Wi-Fi but small enough that the user notices a real failure. */
    int tmo = 30 * 1000 * 1000;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO,
                     &tmo, sizeof(tmo));
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO,
                     &tmo, sizeof(tmo));

    /* Switch the socket to non-blocking *just for connect()*, so we
     * can give up after PS_CONNECT_TIMEOUT_US instead of letting
     * the kernel's TCP retransmit timer pin the worker for ~75 s
     * — a real concern in censored / dropped-packet networks where
     * SYNs go into a black hole. We watch for EPOLLOUT (or
     * EPOLLHUP/EPOLLERR for refused) via epoll, then flip the
     * socket back to blocking before send/recv so the rest of the
     * code can keep using the simpler synchronous primitives. */
    int nbio_on = 1;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO,
                     &nbio_on, sizeof(nbio_on));

    SceNetSockaddrIn sin;
    ps_memset(&sin, 0, sizeof(sin));
    sin.sin_family = SCE_NET_AF_INET;
    sin.sin_port   = sceNetHtons((unsigned short)connect_port);
    sin.sin_addr   = addr;

    vlog("stream: connecting to %s:%d%s (sin_addr=0x%08X net-order)",
         connect_host, connect_port,
         proxy.in_use ? " [via proxy]" : "",
         (unsigned)sin.sin_addr.s_addr);
    int cn = sceNetConnect(sock, (SceNetSockaddr *)&sin, sizeof(sin));
    /* Non-blocking connect on Vita returns SCE_NET_ERROR_EINPROGRESS
     * encoded as `0x80410100 | errno`. The errno here is 0x24
     * (BSD-style EINPROGRESS), so the full code is 0x80410124 — not
     * the 0x80410115 (errno 0x15 = ENOTSOCK on BSD) we initially
     * guarded against. We accept the EINPROGRESS code explicitly,
     * and as a belt-and-braces fallback we also accept any sceNet
     * error whose low byte is 0x24, since some SDK headers / future
     * firmwares may shift the upper bits. Anything else negative
     * is a real fault. */
    int conn_in_progress = (cn < 0) &&
        (((unsigned)cn == 0x80410124u) ||
         (((unsigned)cn & 0xFFFFFF00u) == 0x80410100u && ((unsigned)cn & 0xFFu) == 0x24));
    if (cn < 0 && !conn_in_progress) {
        vlog("sceNetConnect %s:%d 0x%08X", connect_host, connect_port, cn);
        sceNetSocketClose(sock);
        if (err_out) *err_out = cn;
        return -2;
    }

    if (cn < 0) {
        /* In-progress: wait for writable (or error) with a bounded
         * timeout. */
        int eid = sceNetEpollCreate("pngshotssu_ep", 0);
        if (eid < 0) {
            vlog("sceNetEpollCreate 0x%08X", eid);
            sceNetSocketClose(sock);
            if (err_out) *err_out = eid;
            return -2;
        }
        SceNetEpollEvent ev;
        ps_memset(&ev, 0, sizeof(ev));
        ev.events  = SCE_NET_EPOLLOUT;
        ev.data.fd = sock;
        sceNetEpollControl(eid, SCE_NET_EPOLL_CTL_ADD, sock, &ev);
        SceNetEpollEvent got;
        ps_memset(&got, 0, sizeof(got));
        /* sceNet uses a *milliseconds* timeout here, unlike
         * sceKernelDelayThread's microseconds. PS_CONNECT_TIMEOUT_MS
         * is intentionally short (15 s) so the user gets a quick
         * failure dialog when censorship / firewall is dropping the
         * SYN. */
        int er = sceNetEpollWait(eid, &got, 1, 15 * 1000 * 1000 /* that mu things seconds, see https://discord.com/channels/439481392548675594/465960127963463680/1497936180170199041 */);
        sceNetEpollDestroy(eid);
        if (er <= 0) {
            vlog("connect timed out after 15 s (host=%s port=%d)",
                 connect_host, connect_port);
            sceNetSocketClose(sock);
            if (err_out) *err_out = er; /* synth: connect tmo */
            return -2;
        }
        /* Confirm the actual connect outcome with SO_ERROR. */
        int so_err = 0;
        unsigned olen = sizeof(so_err);
        sceNetGetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR,
                         &so_err, &olen);
        if (so_err != 0) {
            vlog("connect failed (SO_ERROR=0x%08X) host=%s port=%d",
                 (unsigned)so_err, connect_host, connect_port);
            sceNetSocketClose(sock);
            if (err_out) *err_out = so_err;
            return -2;
        }
    }
    /* Success — flip back to blocking I/O for the simple send/recv
     * code below. */
    int nbio_off = 0;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO,
                     &nbio_off, sizeof(nbio_off));
    vlog("stream: connected, sending request");


    /* Compose request line + headers. When proxying, RFC 7230 § 5.3.2
     * wants an absolute-form target ("http://origin/path"); when
     * direct, the origin form ("/path") is correct. The Host header
     * always names the origin, regardless of which TCP endpoint we
     * actually opened. */
    char target[1280];
    if (proxy.in_use) {
        if (origin_port == 80) {
            ps_snprintf(target, sizeof(target),
                        "http://%s%s", origin_host, req_path);
        } else {
            ps_snprintf(target, sizeof(target),
                        "http://%s:%d%s", origin_host, origin_port, req_path);
        }
    } else {
        size_t n = ps_strlen(req_path);
        if (n >= sizeof(target)) n = sizeof(target) - 1;
        ps_memcpy(target, req_path, n);
        target[n] = '\0';
    }

    /* Always include the explicit port in the Host header — RFC 7230
     * allows it for any scheme, the server doesn't care, and it
     * keeps the header construction branch-free. */
    char hdr[1536];
    int hl = ps_snprintf(hdr, sizeof(hdr),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: pngshot-ssu/1.0\r\n"
        "Accept: */*\r\n"
        "Content-Type: image/png\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        target, origin_host, origin_port, body_len);


    if (hl <= 0 || hl >= (int)sizeof(hdr)) {
        sceNetSocketClose(sock);
        if (err_out) *err_out = (int)0x80620103u;
        return -1;
    }

    if (net_send_all(sock, hdr, hl) < 0) {
        sceNetSocketClose(sock);
        if (err_out) *err_out = (int)0x80620104u;
        return -2;
    }

    if (net_send_file(sock, body_path, body_len) < 0) {
        sceNetSocketClose(sock);
        if (err_out) *err_out = (int)0x80620105u;
        return -2;
    }

    int status = 0;
    /* Local body buffer; we copy into the caller's resp_out below
     * so the function still works when resp_out is NULL. 512 bytes
     * is plenty for a server-side error reason — anything longer
     * gets truncated, which is fine for a one-line user toast. */
    char body[512];
    if (read_http_response(sock, &status, body, sizeof(body)) < 0) {
        sceNetSocketClose(sock);
        if (err_out) *err_out = (int)0x80620106u;
        return -2;
    }

    /* Drain whatever's left so the server sees a clean close even
     * if the response body was longer than `body`. */
    char drain[256];
    for (int i = 0; i < 64; i++) {
        int n = sceNetRecv(sock, drain, sizeof(drain), 0);
        if (n <= 0) break;
    }
    sceNetSocketClose(sock);

    /* Hand the body snippet back to the caller (truncated to fit). */
    if (resp_out && resp_cap > 0) {
        int bl = (int)ps_strlen(body);
        if (bl > resp_cap - 1) bl = resp_cap - 1;
        ps_memcpy(resp_out, body, bl);
        resp_out[bl] = '\0';
    }

    if (status >= 200 && status < 300) {
        /* sys-screenuploader signals failures *in the body* even on
         * HTTP 200, so we have to look at the JSON `status` field
         * before declaring success. Anything other than literal
         * "ok" is treated as a permanent failure (no point retrying
         * "invalid image file" — the file isn't going to fix itself
         * on the next attempt). */
        char ssu_status[32];
        if (json_field(body, "status", ssu_status, sizeof(ssu_status)) == 0 &&
            sceClibStrncmp(ssu_status, "ok", 3) != 0) {
            char ssu_msg[160];
            if (json_field(body, "message", ssu_msg, sizeof(ssu_msg)) != 0)
                ssu_msg[0] = '\0';
            vlog("stream upload app-fail status=\"%s\" message=\"%s\" body=%s",
                 ssu_status, ssu_msg, body);
            if (resp_out && resp_cap > 0) {
                /* Replace the raw JSON we already copied with just
                 * the human-readable reason, since that's what we
                 * want to surface in the toast. */
                const char *src = ssu_msg[0] ? ssu_msg : ssu_status;
                int sl = (int)ps_strlen(src);
                if (sl > resp_cap - 1) sl = resp_cap - 1;
                ps_memcpy(resp_out, src, sl);
                resp_out[sl] = '\0';
            }
            if (err_out) *err_out = (int)0x80620108u; /* synth: app-level */
            return -1;
        }
        vlog("stream upload ok: %d bytes -> http://%s%s (HTTP %d)",
             body_len, origin_host, req_path, status);
        /* Wipe the response slot so the worker doesn't append a
         * useless "{\"status\":\"ok\"}" to its success toast — but
         * the worker already special-cases rc==0, so this is just
         * future-proofing. */
        if (resp_out && resp_cap > 0) resp_out[0] = '\0';
        return 0;
    }

    if (err_out) *err_out = status;
    if (status >= 500 || status == 408 || status == 429) {
        vlog("stream upload fail status=%d body=%s", status, body);
        return -2;
    }
    vlog("stream upload fail status=%d body=%s", status, body);
    return -1;
}



/* ------------------------------------------------------------------ */
/* Core HTTP POST (sceHttp, used by the in-RAM enqueue path)          */
/* ------------------------------------------------------------------ */

/* Return codes:
 *    0 : upload succeeded.
 *   -1 : permanent failure (HTTP 4xx, bad URL, etc).
 *   -2 : transient failure (network down, 5xx, send/read error).
 *
 * `*err_out`, on non-zero return, carries the most useful diagnostic
 *  we have: a negative sce error (e.g. 0x80431064 from sceHttpSend
 *  when Wi-Fi is asleep), or the HTTP status code if we got that far.
 *  Callers surface it to the user via the failure notification text
 *  so they can grep / report. */
static int send_buffer(const ps_config_t *cfg,
                       const void *png_data, int png_len,
                       const SceDateTime *stamp,
                       int *err_out) {
    if (err_out) *err_out = 0;
    if (!cfg->enabled) return 0;
    if (http_ensure() < 0) {
        if (err_out) *err_out = -1;
        return -2;
    }

    char fname[64];
    synth_filename(stamp, fname);

    char url[1024];
    url_with_filename(cfg->upload_url, fname, url, sizeof(url));

    int tpl = -1, conn = -1, req = -1;
    int rc = -1;

    tpl = sceHttpCreateTemplate("pngshot-ssu/1.0", 1, 1);
    if (tpl < 0) {
        vlog("CreateTemplate 0x%08X", tpl);
        if (err_out) *err_out = tpl;
        goto out;
    }

    sceHttpsSetSslCallback(tpl, https_accept_all, NULL);

    conn = sceHttpCreateConnectionWithURL(tpl, url, 0);
    if (conn < 0) {
        vlog("CreateConnection 0x%08X url=%s", conn, url);
        if (err_out) *err_out = conn;
        goto out;
    }

    req = sceHttpCreateRequestWithURL(conn, 1 /* POST */, url,
                                      (unsigned long long)png_len);
    if (req < 0) {
        vlog("CreateRequest 0x%08X", req);
        if (err_out) *err_out = req;
        goto out;
    }

    sceHttpAddRequestHeader(req, "Content-Type", "image/png", 0);
    sceHttpAddRequestHeader(req, "Accept", "*/*", 0);

    int send_rc = sceHttpSendRequest(req, (void *)png_data, (unsigned int)png_len);
    if (send_rc < 0) {
        vlog("SendRequest 0x%08X url=%s", send_rc, url);
        if (err_out) *err_out = send_rc;
        rc = -2;   /* transient */
        goto out;
    }

    int status = 0;
    sceHttpGetStatusCode(req, &status);

    /* Drain body so we don't leave the connection half-read; log a
     * snippet on failure for troubleshooting. */
    char resp[512]; int total_r = 0;
    for (;;) {
        int r = sceHttpReadData(req, resp + total_r,
                                sizeof(resp) - 1 - total_r);
        if (r <= 0) break;
        total_r += r;
        if (total_r >= (int)sizeof(resp) - 1) break;
    }
    resp[total_r] = '\0';

    if (status >= 200 && status < 300) {
        vlog("upload ok: %d bytes -> %s (HTTP %d)", png_len, url, status);
        rc = 0;
    } else if (status >= 500 || status == 408 || status == 429) {
        /* Server-side hiccup / rate-limit. */
        vlog("upload fail status=%d url=%s resp=%s", status, url, resp);
        if (err_out) *err_out = status;
        rc = -2;
    } else {
        /* 4xx other than rate-limit: the config is probably wrong. */
        vlog("upload fail status=%d url=%s resp=%s", status, url, resp);
        if (err_out) *err_out = status;
        rc = -1;
    }

out:
    if (req  >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tpl  >= 0) sceHttpDeleteTemplate(tpl);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Public producer (called from the screenshot hook).                 */
/* ------------------------------------------------------------------ */
int ps_uploader_enqueue_notify(const void *buf, int len,
                               const SceDateTime *stamp, int notify) {
    if (len <= 0 || len > PS_MAX_UPLOAD_SIZE) {
        vlog("enqueue: bad len %d", len);
        return -1;
    }
    if (g_pipe_mtx < 0) {
        vlog("enqueue: uploader not started");
        return -1;
    }

    sceKernelLockMutex(g_pipe_mtx, 1, NULL);
    if (g_ring_count >= PS_UPLOAD_QUEUE) {
        sceKernelUnlockMutex(g_pipe_mtx, 1);
        vlog("enqueue: queue full, dropping");
        return -1;
    }

    SceUID blk;
    void *body = big_alloc((size_t)len, &blk);
    if (!body) {
        sceKernelUnlockMutex(g_pipe_mtx, 1);
        return -1;
    }
    ps_memcpy(body, buf, len);

    job_t *j = &g_ring[g_ring_tail];
    j->blk       = blk;
    j->body      = body;
    j->len       = len;
    j->notify    = notify ? 1 : 0;
    j->path[0]   = '\0';
    j->stamp     = *stamp;
    g_ring_tail = (g_ring_tail + 1) % PS_UPLOAD_QUEUE;
    g_ring_count++;
    sceKernelUnlockMutex(g_pipe_mtx, 1);

    sceKernelSignalSema(g_pipe_sem, 1);
    return 0;
}

int ps_uploader_enqueue(const void *buf, int len, const SceDateTime *stamp) {

    return ps_uploader_enqueue_notify(buf, len, stamp, 0);
}

/* File-mode enqueue: defer reading the PNG body until the worker
 * actually wants to send it. The job carries ju
 * MemBlock of exactly that size, slurps the bytes in, POSTs them,
 * frees the block, and unlinks the file. The expensive contiguous
 * allocation thus happens *after* the encode burst, when SceShell's
 * memory pressure is at its lowest. */
int ps_uploader_enqueue_file(const char *path, int len,
                             const SceDateTime *stamp,
                             int notify) {
    if (!path || !path[0] || len <= 0 || len > PS_MAX_UPLOAD_SIZE) {
        vlog("enqueue_file: bad args path=%s len=%d", path ? path : "(null)", len);
        return -1;
    }
    if (g_pipe_mtx < 0) {
        vlog("enqueue_file: uploader not started");
        return -1;
    }

    sceKernelLockMutex(g_pipe_mtx, 1, NULL);
    if (g_ring_count >= PS_UPLOAD_QUEUE) {
        sceKernelUnlockMutex(g_pipe_mtx, 1);
        vlog("enqueue_file: queue full, dropping");
        return -1;
    }

    job_t *j = &g_ring[g_ring_tail];
    j->blk       = -1;
    j->body      = NULL;
    j->len       = len;
    j->notify    = notify ? 1 : 0;

    /* Bound-check + null-terminate. The path comes from main.c's
     * 128-byte staging-name builder, so this is just defensive. */
    size_t pl = ps_strlen(path);
    if (pl >= sizeof(j->path)) pl = sizeof(j->path) - 1;
    ps_memcpy(j->path, path, pl);
    j->path[pl] = '\0';
    j->stamp    = *stamp;

    g_ring_tail = (g_ring_tail + 1) % PS_UPLOAD_QUEUE;
    g_ring_count++;
    sceKernelUnlockMutex(g_pipe_mtx, 1);

    sceKernelSignalSema(g_pipe_sem, 1);
    return 0;
}

/* Wait for `path` to exist and reach at least `expected` bytes. Used
 * before reading SceShell's capture.png — SceShell flushes that file
 * *after* our encode hook returns, so the worker may briefly see it
 * missing or short. 5 s @ 50 ms = 100 polls is far more than the
 * eMMC ever needs and still bounded if SceShell decides to skip the
 * write entirely (e.g. user cancelled the screenshot). */
static int wait_file_ready(const char *path, int expected) {
    SceIoStat st;
    int last_size = -1;
    for (int i = 0; i < 100; i++) {
        ps_memset(&st, 0, sizeof(st));
        int gs = sceIoGetstat(path, &st);
        if (gs >= 0) last_size = (int)st.st_size;
        if (gs >= 0 && (int)st.st_size >= expected) {
            return 0;
        }
        sceKernelDelayThread(50 * 1000);
    }
    vlog("wait_file_ready: %s timed out, last size=%d expected=%d",
         path, last_size, expected);
    return -1;
}


/* (load_staging_file was removed: file-mode jobs now stream from
 * disk directly via send_file_streamed instead of slurping the body
 * into a 1 MB MemBlock. Kept the wait_file_ready helper since the
 * worker still needs to wait for SceShell to finish writing
 * capture.png before we open it.) */



/* ------------------------------------------------------------------ */
/* Worker                                                             */
/* ------------------------------------------------------------------ */
static int uploader_thread(SceSize args, void *argp) {
    (void)args; (void)argp;

    /* Let SceShell's own network stack settle before we poke
     * sceHttp — the plugin loads early in shell startup. */
    sceKernelDelayThread(5 * 1000 * 1000);
    vlog("uploader thread started");

    ps_config_t cfg;

    while (g_running) {
        SceUInt timeout = 10 * 1000 * 1000;
        int w = sceKernelWaitSema(g_pipe_sem, 1, &timeout);
        if (w < 0) continue;  /* timeout or shutdown; loop */

        sceKernelLockMutex(g_pipe_mtx, 1, NULL);
        if (g_ring_count == 0) {
            sceKernelUnlockMutex(g_pipe_mtx, 1);
            continue;
        }
        job_t j = g_ring[g_ring_head];
        g_ring_head = (g_ring_head + 1) % PS_UPLOAD_QUEUE;
        g_ring_count--;
        sceKernelUnlockMutex(g_pipe_mtx, 1);

        vlog("worker: dequeued job len=%d notify=%d path='%s'",
             j.len, j.notify, j.path[0] ? j.path : "(none)");

        /* Re-read config on each attempt so toggling `enabled` or
         * swapping `upload_url` takes effect without a reboot. If the
         * user has disabled or removed the config since the shot was
         * queued, drop the buffer / file. */
        int file_mode = (j.path[0] != '\0');

        if (ps_cfg_load(&cfg) != 0 || !cfg.enabled) {
            /* File-mode jobs hand us a path the caller still owns
             * (SceShell's capture.png, the user's gallery photo) —
             * we never delete it. In-RAM mode owns its MemBlock and
             * has to free it. */
            if (!file_mode) big_free(j.blk);
            continue;
        }


        /* Two upload paths:
         *   - file_mode: stream straight from disk over a raw
         *     socket, ~32 KB scratch RAM regardless of body size.
         *     This is what every real screenshot/share goes through
         *     today; it's the only path that's safe inside the
         *     Photos partition.
         *   - in-RAM: legacy sceHttp path kept for the
         *     ps_uploader_enqueue() API. Not used by anything
         *     in-tree anymore but the symbol stays exported. */
        int err = 0;
        int rc;
        /* Server-side error reason buffer. Both the streaming and
         * sceHttp paths fill this with up to ~128 chars of the
         * response body when the upload fails — sys-screenuploader
         * hands back human-readable strings like "image too big"
         * or "rate limit exceeded" that are far more actionable
         * than a bare HTTP status code. Empty on success / when
         * the server returned no body. */
        char http_resp[160];
        http_resp[0] = '\0';
        if (file_mode) {
            /* Wait for the on-disk body to actually appear at the
             * size we promised. SceShell flushes capture.png after
             * encode_type2 returns, so the worker often races it. */
            if (wait_file_ready(j.path, j.len) < 0) {
                err = 0x80020004;
                rc  = -2;
                if (j.notify) ps_progress_hide();
                goto report;
            }
            if (j.notify) ps_progress_show("Uploading screenshot...");
            rc = send_file_streamed(&cfg, j.path, j.len, &j.stamp, &err,
                                    http_resp, sizeof(http_resp));
            if (j.notify) ps_progress_hide();
        } else {
            if (j.notify) ps_progress_show("Uploading screenshot...");
            rc = send_buffer(&cfg, j.body, j.len, &j.stamp, &err);
            if (j.notify) ps_progress_hide();
        }


    report:



        /* Terminal feedback split by who asked for the upload:
         *
         *   notify=1 (Photos share):  modal MsgDialog inside Photos.
         *     Always shown — success ("Screenshot uploaded") or
         *     failure ("...failed (0x........)"). Photos is fore-
         *     ground and the user is actively waiting.
         *
         *   notify=0 (SceShell screenshot path): silent on success
         *     (the user didn't ask for any UI). On failure, fire a
         *     SceShell toast with the sce-style error code so the
         *     user notices and can grep / report.
         *
         * The displayed code is `err` (sce error from sceHttp*/
        /*  sceNet*, or HTTP status), not the internal rc, so it's   */
        /*  immediately greppable against published error tables.    */
        /* Build a short single-line summary of the failure, with
         * the server's reason string (if any) tacked on after the
         * sce/HTTP error code. We trim the response to keep the
         * toast readable — the full body is already in the log via
         * vlog from inside the senders. */
        if (j.notify) {
            if (rc == 0) {
                ps_notify("Screenshot uploaded");
            } else {
                char msg[256];
                if (http_resp[0]) {
                    ps_snprintf(msg, sizeof(msg),
                                "Screenshot upload failed (0x%08X): %s",
                                (unsigned)err, http_resp);
                } else {
                    ps_snprintf(msg, sizeof(msg),
                                "Screenshot upload failed (0x%08X)",
                                (unsigned)err);
                }
                ps_notify(msg);
            }
        } else {
            if (rc == 0) {
                ps_notify_shell("Screenshot uploaded. Tap to view in gallery.");
            } else {
                char msg[256];
                if (http_resp[0]) {
                    ps_snprintf(msg, sizeof(msg),
                                "Screenshot upload failed (0x%08X): %s",
                                (unsigned)err, http_resp);
                } else {
                    ps_snprintf(msg, sizeof(msg),
                                "Screenshot upload failed. Err:(0x%08X).",
                                (unsigned)err);
                }
                ps_notify_shell(msg);
            }
        }


        /* File-mode jobs don't allocate a MemBlock and don't own
         * the path on disk — nothing to clean up. */
        if (!file_mode) big_free(j.blk);

    }

    vlog("uploader thread exiting");
    return 0;
}


int ps_uploader_start(void) {
    if (g_thid >= 0) return 0;
    g_pipe_mtx = sceKernelCreateMutex("pngshotssu_mtx", 0, 0, NULL);
    g_pipe_sem = sceKernelCreateSema ("pngshotssu_sem", 0, 0,
                                      PS_UPLOAD_QUEUE, NULL);
    if (g_pipe_mtx < 0 || g_pipe_sem < 0) {
        vlog("mutex/sema create failed");
        return -1;
    }
    g_running = 1;
    g_thid = sceKernelCreateThread("pngshotssu_up", uploader_thread,
                                   0x10000100, 0x20000, 0, 0, NULL);
    if (g_thid < 0) {
        vlog("CreateThread failed 0x%08X", g_thid);
        g_running = 0;
        return -1;
    }
    sceKernelStartThread(g_thid, 0, NULL);
    return 0;
}

void ps_uploader_stop(void) {
    g_running = 0;
    if (g_pipe_sem >= 0) sceKernelSignalSema(g_pipe_sem, 1);
    g_thid = -1;
}
