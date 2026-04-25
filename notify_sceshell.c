/* SceShell-side notification helper.
 *
 * Fires a top-right toast bubble (the trophy/download style) by
 * calling SceShell's *internal* notice builders directly — same trick
 * as Princess of Sleeping's
 *   https://github.com/Princess-of-Sleeping/SceShell-Notification-PoC
 *
 * The user-space `sceNotificationUtilSendNotification` export is
 * gated and returns 0x80106301 INTERNAL when called from non-allowed
 * processes (e.g. Photos), so we can't lean on it.
 *
 * These internal calls only resolve from inside the SceShell process.
 * That's exactly where the screenshot-encode hook (the only path
 * that calls into here) lives, so a plain in-process function call
 * is enough — no IPC needed.
 *
 * Tapping the toast launches the Photos app (NPXS10004), giving the
 * user a one-tap path to "go look at the screenshot that just
 * (failed to) upload(ed)".
 */

#include "pngshot.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/clib.h>

#include <taihen.h>

/* taiHEN ships `module_get_offset` only in its kernel headers. The
 * user-space stub library doesn't expose the symbol, so re-implement
 * it locally on top of sceKernelGetModuleInfo — cribbed verbatim
 * from Princess of Sleeping's taihen_min.c. */
static int module_get_offset(SceUID modid, SceSize segidx,
                             uint32_t offset, void *stub_out) {
    SceKernelModuleInfo info;
    if (segidx > 3)            return -1;
    if (stub_out == NULL)      return -2;

    int res = sceKernelGetModuleInfo(modid, &info);
    if (res < 0) return res;

    if (offset > info.segments[segidx].memsz) return -3;

    *(uint32_t *)stub_out = (uint32_t)(info.segments[segidx].vaddr + offset);
    return 0;
}

/* SceShell internal notice API; resolved once on first call. The
 * struct layout at `data[]` and the call sequence are reverse-
 * engineered in the PoC. */
typedef int (*sce_shell_notice_init_t) (void *data);
typedef int (*sce_shell_set_utf8_t)    (void *dst, const char *text, SceSize len);
typedef int (*sce_shell_notice_clean_t)(void *data);
typedef int (*SceLsdb_315B9FD6_t)      (void *data, int a2);

static sce_shell_notice_init_t  s_notice_init  = NULL;
static sce_shell_set_utf8_t     s_set_utf8     = NULL;
static sce_shell_notice_clean_t s_notice_clean = NULL;
static SceLsdb_315B9FD6_t       s_lsdb_dispatch = NULL;

static int s_resolved = 0;     /* tri-state via s_ready */
static int s_ready    = 0;
static SceUID s_mtx   = -1;

/* Resolve SceShell internal offsets for the running firmware. The
 * offsets come from the PoC; the 3.67 / 3.68 retail builds reuse
 * the 3.65 layout for these symbols (confirmed by symbol-diffing
 * public SceShell dumps). Returns 1 on full success.
 *
 * Self-disables silently in non-SceShell processes (taiGetModuleInfo
 * for "SceShell" fails because we're loaded into Photos / etc, and
 * `s_ready` stays 0).
 */
static int resolve_offsets(void) {
    tai_module_info_t info;
    info.size = sizeof(info);
    if (taiGetModuleInfo("SceShell", &info) < 0) {
        vlog("notify-shell: SceShell not loaded, disabled");
        return 0;
    }

    uint32_t off_init = 0, off_set = 0, off_clean = 0;
    switch (info.module_nid) {
    case 0x0552F692: /* 3.60 retail */
        off_init = 0x42930C; off_set = 0x408E14; off_clean = 0x4163E8;
        break;
    case 0x6CB01295: /* 3.60 devkit */
        off_init = 0x41AA30; off_set = 0x3FAD88; off_clean = 0x408298;
        break;
    case 0x5549BF1F: /* 3.65 retail */
    case 0x34B4D82E: /* 3.67 retail — same layout as 3.65 */
    case 0x12DAC0F3: /* 3.68 retail — same layout as 3.65 */
        off_init = 0x429754; off_set = 0x40925C; off_clean = 0x416830;
        break;
    default:
        vlog("notify-shell: unknown SceShell nid 0x%08X, disabled",
             info.module_nid);
        return 0;
    }

    /* All thumb code; OR 1 into the address so BLX targets the
     * correct ISA. */
    if (module_get_offset(info.modid, 0, off_init  | 1, (uintptr_t *)&s_notice_init)  < 0) return 0;
    if (module_get_offset(info.modid, 0, off_set   | 1, (uintptr_t *)&s_set_utf8)     < 0) return 0;
    if (module_get_offset(info.modid, 0, off_clean | 1, (uintptr_t *)&s_notice_clean) < 0) return 0;

    /* SceLsdb's dispatcher is a real exported function, so we don't
     * need a per-FW offset for it. */
    if (taiGetModuleExportFunc("SceLsdb", 0xFFFFFFFF, 0x315B9FD6,
                               (uintptr_t *)&s_lsdb_dispatch) < 0) {
        vlog("notify-shell: SceLsdb_315B9FD6 lookup failed");
        return 0;
    }

    return 1;
}

/* Lazy one-time init: defer offset resolution until the first
 * notification, so we don't pay the cost (or eat the log line) on
 * boot when no upload has happened yet. */
static void ensure_resolved(void) {
    if (s_resolved) return;
    if (s_mtx < 0) s_mtx = sceKernelCreateMutex("pngshotssu_nfs", 0, 0, NULL);
    if (s_mtx >= 0) sceKernelLockMutex(s_mtx, 1, NULL);
    if (!s_resolved) {
        s_ready    = resolve_offsets();
        s_resolved = 1;
    }
    if (s_mtx >= 0) sceKernelUnlockMutex(s_mtx, 1);
}

void ps_notify_shell(const char *text) {
    if (!text) return;
    ensure_resolved();
    if (!s_ready) {
        /* Either we're not in SceShell, or this firmware isn't
         * supported. The screenshot path's caller will still see
         * vlog output; nothing else we can do. */
        return;
    }

    /* Serialize: SceShell's notice builders touch BSS state and we
     * don't want two simultaneous uploads racing for the same
     * scratch struct. */
    if (s_mtx >= 0) sceKernelLockMutex(s_mtx, 1, NULL);

    /* The struct lives entirely on our stack; SceShell copies what
     * it needs into its own queue inside s_lsdb_dispatch, so we can
     * safely tear it down on return. 0x140 bytes is comfortably
     * larger than the highest field (0xFC + 4) the PoC writes. */
    char data[0x140];
    ps_memset(data, 0, sizeof(data));

    s_notice_init(data);

    /* Field map (offsets reverse-engineered in PoC):
     *   [0x00] originator title id   — "from app" label
     *   [0x0C] originator content id — small token, not visibly
     *                                  rendered but required
     *   [0x28] = 2 → "tap to launch app" mode
     *   [0x2C] = 1 → user-visible toast (vs silent log)
     *   [0x30] icon path
     *   [0xBC] body text
     *   [0xCC] target title id (launched on tap)
     *   [0xD8] launch argument
     *   [0xC8] no idea what this does, took it from vitadb downloader and without it app doesnt launch
     */
    s_set_utf8(&data[0x00], "NPXS10004", sceClibStrnlen("NPXS10004", 0x10));
    s_set_utf8(&data[0x0C], "photos",       sceClibStrnlen("photos", 0x10));

    *(uint32_t *)(&data[0x28]) = 0x2;
    data[0x2C] = 0x1;

    s_set_utf8(&data[0x30],
               "vs0:app/NPXS10004/sce_sys/icon0.png",
               sceClibStrnlen("vs0:app/NPXS10004/sce_sys/icon0.png", 0xFFFF));

    s_set_utf8(&data[0xBC], text, sceClibStrnlen(text, 0xFF));
    s_set_utf8(&data[0xCC], "NPXS10004", sceClibStrnlen("NPXS10004", 0x10));
    s_set_utf8(&data[0xD8], "photo:browse?category=SCREENSHOT", sceClibStrnlen("photo:browse?category=ALL", 0x20));
    *(uint32_t *)(&data[0xC8]) = 0x20000;
    s_lsdb_dispatch(data, 1);
    s_notice_clean(data);

    if (s_mtx >= 0) sceKernelUnlockMutex(s_mtx, 1);
}
