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

/* Retry policy. If the network is down, or the POST fails, we'll
 * re-try the same job up to PS_MAX_ATTEMPTS times, sleeping a bit
 * longer between each attempt. 6 * 10 s ≈ 1 min of retries per shot,
 * which is plenty for the Vita's Wi-Fi to reconnect after standby. */
#define PS_MAX_ATTEMPTS    6
#define PS_RETRY_BASE_US   (10 * 1000 * 1000)  /* 10 s */

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

int  ps_uploader_start(void);
void ps_uploader_stop(void);

#endif
