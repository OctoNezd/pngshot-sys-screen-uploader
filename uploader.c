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
 *   on-disk (SceShell encode hook): the caller streamed the PNG
 *     into a staging file to keep memory pressure off the encode
 *     thread. `body`/`blk` start unset; the worker mmap-style-reads
 *     the file into a fresh MemBlock right before sending, so the
 *     one big allocation happens *after* the encode burst is over.
 *     The worker also unlinks the file when it's done.
 */
typedef struct {
    SceUID      blk;       /* MemBlock holding the body, -1 for file mode */
    void       *body;      /* base pointer inside blk, NULL for file mode */
    int         len;
    int         notify;    /* if non-zero: pop a toast on success/failure */
    int         keep_file; /* file mode: 1 = leave file alone (shared with
                            *            SceShell), 0 = unlink after read */
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

/* SceShell already brings the net stack up, but we may race it on
 * boot, or Wi-Fi may be asleep when the user takes a screenshot right
 * after waking from standby. Try to nudge it awake once per call and
 * then poll for up to `wait_us` microseconds. Return 1 when we see
 * state == 3 (SCE_NETCTL_STATE_CONNECTED). */
static int net_wait_ready(unsigned wait_us) {
    int state = 0;
    unsigned waited = 0;
    const unsigned step = 500 * 1000;  /* 500 ms poll */

    /* Initial check: if we're already up, return immediately. */
    if (sceNetCtlInetGetState(&state) >= 0 && state == 3) return 1;

    /* sceNetCtlInetGetState returning >=0 with state != 3 means the
     * stack is available but not connected. We can't programmatically
     * re-associate to Wi-Fi from user-mode without triggering system
     * UI, but polling gives the user / SceShell time to finish it. */
    while (waited < wait_us) {
        sceKernelDelayThread(step);
        waited += step;
        if (sceNetCtlInetGetState(&state) >= 0 && state == 3) return 1;
    }
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
/* Core HTTP POST                                                     */
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
    /* Give the network up to 15 s to come up (e.g. just woke from
     * standby) before giving up. */
    if (!net_wait_ready(15 * 1000 * 1000)) {
        int state = 0;
        sceNetCtlInetGetState(&state);
        vlog("net not ready, state=%d", state);
        /* No sce error to surface — fabricate something the user
         * recognises as "network problem" without colliding with a
         * real Sony code. The upper byte 0x8062 isn't allocated to
         * any module on Vita; "0x80620000 | state" gives us a code
         * whose low nibble identifies which net stage we got stuck
         * at (0=disabled, 1=wifi up, 2=ip pending, 3=connected). */
        if (err_out) *err_out = (int)(0x80620000u | (unsigned)(state & 0xFFFF));
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
    j->keep_file = 0;       /* in-RAM mode owns nothing on disk */
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
 * actually wants to send it. The job carries just the staging path
 * + expected length; the worker stat()s the file, allocates a single
 * MemBlock of exactly that size, slurps the bytes in, POSTs them,
 * frees the block, and unlinks the file. The expensive contiguous
 * allocation thus happens *after* the encode burst, when SceShell's
 * memory pressure is at its lowest. */
int ps_uploader_enqueue_file(const char *path, int len,
                             const SceDateTime *stamp) {
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
    j->blk      = -1;
    j->body     = NULL;
    j->len      = len;
    j->notify   = 0;
    /* The path is shared with SceShell (it's the same capture.png
     * SceShell writes after every screenshot). Don't unlink it from
     * under SceShell — the next screenshot just overwrites it, and
     * removing it could confuse SceShell's own bookkeeping. */
    j->keep_file = 1;
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
    for (int i = 0; i < 100; i++) {
        ps_memset(&st, 0, sizeof(st));
        if (sceIoGetstat(path, &st) >= 0 &&
            (int)st.st_size >= expected) {
            return 0;
        }
        sceKernelDelayThread(50 * 1000);
    }
    return -1;
}

/* Slurp a staging file into a freshly-allocated MemBlock. Returns
 * the buffer (and SceUID via *out_blk) on success, NULL on any
 * failure (alloc, open, short read). Caller frees on success. */
static void *load_staging_file(const char *path, int len, SceUID *out_blk) {
    *out_blk = -1;

    /* SceShell writes capture.png *after* our encode hook returns,
     * so the worker normally beats SceShell to the file. Block here
     * until the on-disk size matches what the encoder reported, or
     * give up after a few seconds. */
    if (wait_file_ready(path, len) < 0) {
        vlog("load_staging: %s did not reach %d bytes in time", path, len);
        return NULL;
    }

    SceUID blk;
    void *body = big_alloc((size_t)len, &blk);
    if (!body) return NULL;

    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        vlog("load_staging: open %s failed 0x%08X", path, fd);
        big_free(blk);
        return NULL;
    }


    int total = 0;
    while (total < len) {
        int r = sceIoRead(fd, (char *)body + total, len - total);
        if (r <= 0) {
            vlog("load_staging: read %s short %d/%d (rc=%d)",
                 path, total, len, r);
            sceIoClose(fd);
            big_free(blk);
            return NULL;
        }
        total += r;
    }
    sceIoClose(fd);

    *out_blk = blk;
    return body;
}



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

        /* Re-read config on each attempt so toggling `enabled` or
         * swapping `upload_url` takes effect without a reboot. If the
         * user has disabled or removed the config since the shot was
         * queued, drop the buffer / file. */
        int file_mode = (j.path[0] != '\0');
        if (ps_cfg_load(&cfg) != 0 || !cfg.enabled) {
            if (file_mode) {
                if (!j.keep_file) sceIoRemove(j.path);
            } else {
                big_free(j.blk);
            }
            continue;
        }


        /* In file mode, the body still lives on disk — pull it into
         * a MemBlock now (much later than the encode burst, so the
         * one big contiguous allocation has the best chance of
         * succeeding). */
        int err = 0;
        int rc;
        if (file_mode) {
            j.body = load_staging_file(j.path, j.len, &j.blk);
            if (!j.body) {
                /* Couldn't reserve memory or read the file. Treat as
                 * a transient failure for notification purposes; the
                 * staging file is unlinked either way. */
                err = 0x80020004;  /* SCE_ERROR_ERRNO_ENOMEM-ish */
                rc  = -2;
                sceIoRemove(j.path);
                if (j.notify) ps_progress_hide();
                goto report;
            }
        }

        if (j.notify) ps_progress_show("Uploading screenshot...");

        rc = send_buffer(&cfg, j.body, j.len, &j.stamp, &err);

        if (j.notify) ps_progress_hide();

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
        if (j.notify) {
            if (rc == 0) {
                ps_notify("Screenshot uploaded");
            } else {
                char msg[128];
                ps_snprintf(msg, sizeof(msg),
                            "Screenshot upload failed (0x%08X)",
                            (unsigned)err);
                ps_notify(msg);
            }
        } else {
            if (rc == 0) {
                ps_notify_shell("Screenshot uploaded. Tap to view in gallery.");
            } else {
                char msg[128];
                ps_snprintf(msg, sizeof(msg),
                            "Screenshot upload failed. Err:(0x%08X).",
                            (unsigned)err);
                ps_notify_shell(msg);
            } 
        }

        big_free(j.blk);
        /* File-mode cleanup: only unlink staging files we own. The
         * SceShell capture.png is shared (keep_file=1) — we leave it
         * in place so SceShell's own bookkeeping isn't disturbed and
         * the next screenshot just overwrites it. */
        if (file_mode && j.path[0] && !j.keep_file) sceIoRemove(j.path);

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
