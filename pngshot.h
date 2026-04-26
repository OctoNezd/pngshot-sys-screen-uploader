#ifndef PNGSHOT_SSU_H
#define PNGSHOT_SSU_H

/* pngshot-sys-screenuploader
 *
 * Uploads every screenshot the user takes (via PS+Start) to a
 * sys-screenuploader-compatible HTTP endpoint. Designed as a fork of
 * upstream pngshot with the upload pipeline wired directly into the
 * PNG encode hook — no filesystem watcher, no state file, no polling.
 *
 * Why sys-screenuploader only:
 *   Telegram's Bot API and basically every modern HTTPS endpoint is
 *   served under a Let's Encrypt / ISRG Root X1 chain. The Vita's
 *   sceHttp/sceSsl CA bundle predates that root, so TLS handshakes
 *   against api.telegram.org fail with SCE_SSL_ERROR_UNTRUSTED even
 *   with every verify flag disabled. We'd need to statically link
 *   mbedTLS + a bundled CA to work around it, which defeats the point
 *   of a tiny taiHEN plugin. Instead we target sys-screenuploader —
 *   it's plain HTTP (or user-controlled HTTPS on their own host), the
 *   filename scheme embeds the timestamp + title-id hash, and the
 *   server is the component that fans out to Telegram / Discord /
 *   whatever.
 *
 * Reference implementation: https://github.com/ScreensSub/sys-screenuploader
 *
 * Built with -nostdlib (same as upstream pngshot), so everything is
 * sceClib* + sceKernel*; helpers below. */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#include <psp2/types.h>
#include <psp2/kernel/clib.h>
#include <psp2/rtc.h>




/* Paths and tuning ----------------------------------------------------- */

#define PS_DATA_DIR        "ux0:data/pngshot-ssu"
#define PS_CONFIG_PATH     "ux0:data/pngshot-ssu/config.txt"

#define PS_MAX_UPLOAD_SIZE (8 * 1024 * 1024)  /* 8 MB sanity cap */
#define PS_UPLOAD_QUEUE    8                  /* max queued uploads */


/* Config --------------------------------------------------------------- */

typedef struct {
    char upload_url[512];   /* may contain "{filename}" placeholder.
                             * Required. Prefer plain http:// unless
                             * you control the server's cert chain. */
    int  enabled;
} ps_config_t;

/* libc-ish shims (no newlib!) ----------------------------------------- */

static inline int    ps_strcmp(const char *a, const char *b)            { return sceClibStrcmp(a, b); }
static inline char  *ps_strchr(const char *s, int c)                    { return sceClibStrchr(s, c); }
static inline size_t ps_strlen(const char *s)                           { return sceClibStrnlen(s, 0x7FFFFFFF); }
static inline void  *ps_memset(void *d, int c, size_t n)                { return sceClibMemset(d, c, n); }
static inline void  *ps_memcpy(void *d, const void *s, size_t n)        { return sceClibMemcpy(d, s, n); }

#define ps_snprintf   sceClibSnprintf
#define ps_vsnprintf  sceClibVsnprintf

/* Subsystems ----------------------------------------------------------- */

void vlog_init(void);
void vlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

int  ps_cfg_load(ps_config_t *cfg);

/* Quick existence check — no parse, no allocation, no logging. Used
 * by the encode hook to skip the capture copy entirely when the user
 * hasn't created a config yet. Returns 1 if the file exists. */
int  ps_cfg_present(void);

/* Upload `buf` (len bytes, typically a PNG we just produced in the
 * screenshot hook). `stamp` is an SceDateTime captured at encode time;
 * it's used to synthesise a sys-screenuploader filename. Non-blocking:
 * the buffer is copied into a dedicated MemBlock queue entry and a
 * worker thread performs the HTTP POST. Returns 0 if queued. */
int  ps_uploader_enqueue(const void *buf, int len, const SceDateTime *stamp);

/* On-disk variant: defer the big body allocation until the worker
 * actually wants to send. The worker stat()s the file, allocates a
 * MemBlock of exactly that size, slurps + POSTs, frees. The file
 * itself is always left in place — every current caller hands us a
 * path it owns (SceShell's capture.png, the user's gallery photo)
 * and doesn't want pngshot touching it.
 *
 * Used in two places:
 *   - SceShell encode hook: hands SceShell's own capture.png path
 *     (notify=0) so the encode burst doesn't have to allocate a
 *     1 MB buffer at exactly the moment ScePaf is most fragmented.
 *   - Photos share hook: hands the user's photo path directly
 *     (notify=1) so the Photos process doesn't have to keep the
 *     file in ScePaf *and* a MemBlock at the same time — its tiny
 *     ddrmain partition can't hold both.
 *
 * Returns 0 if queued. */
int  ps_uploader_enqueue_file(const char *path, int len,
                              const SceDateTime *stamp,
                              int notify);



/* Same as ps_uploader_enqueue, but emits a system toast popup
 * (success/failure) when the job finishes. Used by the photos-app
 * email-hook path so users get visual feedback that their share
 * actually went somewhere. `notify` is treated as a bool. */
int  ps_uploader_enqueue_notify(const void *buf, int len,
                                const SceDateTime *stamp, int notify);

int  ps_uploader_start(void);
void ps_uploader_stop(void);

/* Modal popup. Used inside the Photos process for both success and
 * failure feedback — Photos already has a CommonDialog render loop
 * running so SceMsgDialog "just works" there with no GXM setup on
 * our part. */
void ps_notify(const char *text);

/* Non-blocking progress (spinner-style) dialog. Used by the Photos
 * upload path so the user sees something happen while the HTTP POST
 * is in flight (a 1 MB screenshot over Vita Wi-Fi can take several
 * seconds). `ps_progress_show` returns immediately after the dialog
 * is queued; `ps_progress_hide` tears it down before we put up the
 * final ps_notify() result dialog. Safe to call from a worker
 * thread inside the same process where the dialog should appear. */
void ps_progress_show(const char *text);
void ps_progress_hide(void);

/* Top-right toast bubble, fired through SceShell's *internal* notice
 * builders (Princess-of-Sleeping PoC). Only resolves from inside the
 * SceShell process; in any other process this is a silent no-op.
 * Tap-action launches Photos (NPXS10004). */
void ps_notify_shell(const char *text);

/* Photos app (NPXS10004) hooks. Installs both the email-builder
 * function-offset hook and the deferred scePafToplevelGetText import
 * hook (the latter via sceSysmoduleLoadModuleInternalWithArg, since
 * ScePaf isn't loaded into Photos until after module_start runs).
 * `info` is a pointer to a tai_module_info_t for the Photos main
 * module — typed as void* here so this header doesn't need to drag
 * <taihen.h> into every translation unit. See photos.c. */
void ps_photos_init(const void *info);

#endif
