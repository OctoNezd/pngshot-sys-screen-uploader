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
/* Queue                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    SceUID      blk;       /* MemBlock holding the body */
    void       *body;      /* base pointer inside blk */
    int         len;
    int         attempts;  /* how many times we've tried to send */
    int         notify;    /* if non-zero: pop a toast on success/failure */
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
 *   -1 : permanent failure (HTTP 4xx, bad URL, etc). No retry.
 *   -2 : transient failure (network down, 5xx, send/read error).
 *        Caller should reschedule.
 */
static int send_buffer(const ps_config_t *cfg,
                       const void *png_data, int png_len,
                       const SceDateTime *stamp) {
    if (!cfg->enabled) return 0;
    if (http_ensure() < 0) return -2;
    /* Wait up to 15 s for the network to come up (e.g. just woke from
     * standby). If we time out, treat as transient so the retry loop
     * gets another crack at it later. */
    if (!net_wait_ready(15 * 1000 * 1000)) {
        vlog("net not ready, will retry");
        return -2;
    }

    char fname[64];
    synth_filename(stamp, fname);

    char url[1024];
    url_with_filename(cfg->upload_url, fname, url, sizeof(url));

    int tpl = -1, conn = -1, req = -1;
    int rc = -1;

    tpl = sceHttpCreateTemplate("pngshot-ssu/1.0", 1, 1);
    if (tpl < 0) { vlog("CreateTemplate 0x%08X", tpl); goto out; }

    sceHttpsSetSslCallback(tpl, https_accept_all, NULL);

    conn = sceHttpCreateConnectionWithURL(tpl, url, 0);
    if (conn < 0) { vlog("CreateConnection 0x%08X url=%s", conn, url); goto out; }

    req = sceHttpCreateRequestWithURL(conn, 1 /* POST */, url,
                                      (unsigned long long)png_len);
    if (req < 0) { vlog("CreateRequest 0x%08X", req); goto out; }

    sceHttpAddRequestHeader(req, "Content-Type", "image/png", 0);
    sceHttpAddRequestHeader(req, "Accept", "*/*", 0);

    int send_rc = sceHttpSendRequest(req, (void *)png_data, (unsigned int)png_len);
    if (send_rc < 0) {
        vlog("SendRequest 0x%08X url=%s (will retry)", send_rc, url);
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
        /* Server-side hiccup / rate-limit — retry. */
        vlog("upload fail status=%d url=%s resp=%s (will retry)", status, url, resp);
        rc = -2;
    } else {
        /* 4xx other than rate-limit: the config is probably wrong.
         * No point burning retries on a permanent error. */
        vlog("upload fail status=%d url=%s resp=%s", status, url, resp);
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
    j->blk      = blk;
    j->body     = body;
    j->len      = len;
    j->attempts = 0;
    j->notify   = notify ? 1 : 0;
    j->stamp    = *stamp;
    g_ring_tail = (g_ring_tail + 1) % PS_UPLOAD_QUEUE;
    g_ring_count++;
    sceKernelUnlockMutex(g_pipe_mtx, 1);

    sceKernelSignalSema(g_pipe_sem, 1);
    return 0;
}

int ps_uploader_enqueue(const void *buf, int len, const SceDateTime *stamp) {
    return ps_uploader_enqueue_notify(buf, len, stamp, 0);
}

/* Re-insert a job at the head of the ring for another try. The ring
 * is small; if someone else filled the last slot while we were
 * uploading, the retry is dropped (we log and move on). */
static int requeue_head(job_t *j) {
    sceKernelLockMutex(g_pipe_mtx, 1, NULL);
    if (g_ring_count >= PS_UPLOAD_QUEUE) {
        sceKernelUnlockMutex(g_pipe_mtx, 1);
        return -1;
    }
    /* Walk head back one slot. */
    g_ring_head = (g_ring_head + PS_UPLOAD_QUEUE - 1) % PS_UPLOAD_QUEUE;
    g_ring[g_ring_head] = *j;
    g_ring_count++;
    sceKernelUnlockMutex(g_pipe_mtx, 1);
    sceKernelSignalSema(g_pipe_sem, 1);
    return 0;
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
         * queued, drop the buffer. */
        if (ps_cfg_load(&cfg) != 0 || !cfg.enabled) {
            big_free(j.blk);
            continue;
        }

        j.attempts++;
        int rc = send_buffer(&cfg, j.body, j.len, &j.stamp);

        if (rc == -2 && j.attempts < PS_MAX_ATTEMPTS) {
            /* Transient failure — back off (linear, starting at
             * PS_RETRY_BASE_US) and requeue. With the defaults that's
             * 10 s, 20 s, 30 s, ... between attempts. Avoiding
             * exponential growth so a transient outage finishes its
             * retry budget inside ~1 min rather than stretching for
             * half an hour. */
            unsigned backoff = PS_RETRY_BASE_US * (unsigned)j.attempts;
            vlog("retry %d/%d in %u s", j.attempts, PS_MAX_ATTEMPTS,
                 backoff / 1000000u);
            sceKernelDelayThread(backoff);
            if (requeue_head(&j) == 0) {
                /* requeue took ownership of the MemBlock. */
                continue;
            }
            vlog("requeue failed (queue full); dropping");
            /* Fall through to terminal failure path so the toast
             * (if any) gets emitted, then the body is freed. */
            rc = -2;
        }

        /* Terminal toast — only when caller asked (j.notify), and
         * only on definite outcomes (not the requeue-and-continue
         * case above, which `continue`d out of the loop). */
        if (j.notify) {
            if (rc == 0) {
                ps_notify("Screenshot uploaded");
            } else if (rc == -1) {
                ps_notify("Upload failed (check config)");
            } else { /* rc == -2 after exhausting retries */
                ps_notify("Upload failed (network)");
            }
        }
        if (rc == -2) {
            vlog("giving up after %d attempts", j.attempts);
        }

        big_free(j.blk);
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
