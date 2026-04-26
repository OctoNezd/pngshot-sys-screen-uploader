/* photos.c — Photos app (NPXS10004) hooks.
 *
 * Everything in this file is only relevant when pngshot-ssu is loaded
 * under *NPXS10004 in taiHEN config. Two hooks are installed:
 *
 *  1. FUN_81075b0c (file offset 0x75b0c in the photos main eboot) —
 *     the function that builds the "email:send?...&attach=<path>" URI
 *     and hands it to SceAppMgrUser. We swallow the call and instead
 *     pump the attached file through our normal sys-screenuploader
 *     pipeline. The Email app is never launched.
 *
 *  2. scePafToplevelGetText — the localised-string fetcher ScePaf uses
 *     to resolve menu labels by 32-bit id. We swap the "Send via
 *     email" entry (id 0xB8CFCC45) for our own "Upload to ssu"
 *     so the visible label matches what the hijacked function does.
 *     Because ScePaf is loaded by Photos *after* module_start runs,
 *     we can't bind the import directly here — see the
 *     sceSysmoduleLoadModuleInternalWithArg trick below
 *     (lifted from FloW's CustomWarning). */

#include <taihen.h>
#include <psp2/sysmodule.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>

#include "pngshot.h"

extern void *sce_paf_malloc(size_t sz);
extern void  sce_paf_free(void *p);


/* Hooks ---------------------------------------------------------------- */

static tai_hook_ref_t photos_email_hook;
static tai_hook_ref_t sceSysmoduleLoadModuleInternalWithArgRef;
static tai_hook_ref_t scePafToplevelGetTextRef;

static SceUID         photos_email_hook_id  = -1;
static SceUID         photos_paf_text_hook  = -1;
static SceUID         photos_paf_load_hook  = -1;

/* Replacement string. UTF-16LE, allocated once ScePaf is up. Stored as
 * uint16_t* (NOT wchar_t*): arm-vita-eabi wchar_t is 32-bit while
 * ScePaf walks 16-bit code units, so a wchar_t buffer would render as
 * just the first character. We cast to wchar_t* only at the return
 * boundary. */
static uint16_t *custom_warning = NULL;

/* One-shot logger for distinct text-ids so a wrong ID assumption is
 * easy to spot in vlog output. */
#define PS_TEXT_ID_LOG_MAX 64
static uint32_t seen_text_ids[PS_TEXT_ID_LOG_MAX];
static int      seen_text_ids_n = 0;

static int seen_text_id(uint32_t id) {
	for (int i = 0; i < seen_text_ids_n; i++)
		if (seen_text_ids[i] == id) return 1;
	if (seen_text_ids_n < PS_TEXT_ID_LOG_MAX)
		seen_text_ids[seen_text_ids_n++] = id;
	return 0;
}

/* Translate "photo0:/<rest>" -> "ux0:/picture/<rest>". photo0: is a
 * VFS alias only the photos process can sceIoOpen; userland code we
 * link against can't, so we rewrite to the underlying mount before
 * trying to read the file. */
static int photos_translate_path(const char *src, char *out, int out_cap) {
	const char *photo_prefix = "photo0:";
	const char *ux_prefix    = "ux0:/picture";
	int pl = (int)ps_strlen(photo_prefix);
	if (!src || sceClibStrncmp(src, photo_prefix, pl) != 0) return -1;
	int needed = (int)ps_strlen(ux_prefix) + (int)ps_strlen(src + pl) + 1;
	if (needed > out_cap) return -1;
	ps_snprintf(out, out_cap, "%s%s", ux_prefix, src + pl);
	return 0;
}

/* Email-builder hook. Original signature is FUN_81075b0c(undefined4 *)
 * where param_1 points at a pointer to the attachment path string.
 * Intentionally does NOT TAI_CONTINUE: the original would launch the
 * Email app, which is exactly what we're replacing. */
static void photos_send_email(void *param_1) {
	if (!param_1) {
		vlog("photos_send_email: param_1=NULL");
		return;
	}
	const char *attach = *(const char **)param_1;
	vlog("photos_send_email: attach=%s", attach ? attach : "(null)");
	if (!attach) return;

	char path[512];
	if (photos_translate_path(attach, path, sizeof(path)) < 0) {
		vlog("photos: unrecognized path prefix, skipping: %s", attach);
		return;
	}
	vlog("photos: translated -> %s", path);

	/* Skip filesystem hit if user hasn't configured uploads yet. */
	if (!ps_cfg_present()) {
		vlog("photos: no config, skipping upload");
		return;
	}

	/* Stat the photo for size — we don't read it here. The Photos
	 * process partition (`ddrmain: NPXS10004`) is *tiny* (a handful
	 * of MB free in practice), and double-buffering a 1 MB JPEG
	 * (paf_malloc + MemBlock copy in enqueue_notify) was enough to
	 * crash the process on the *second* upload. Hand the path to
	 * the worker instead — the worker allocates a single MemBlock
	 * of exactly file-size bytes, reads, sends, frees. Net cost:
	 * one buffer instead of two. */
	SceIoStat st;
	ps_memset(&st, 0, sizeof(st));
	if (sceIoGetstat(path, &st) < 0) {
		vlog("photos: sceIoGetstat %s failed", path);
		return;
	}
	SceOff sz = st.st_size;
	if (sz <= 0 || sz > PS_MAX_UPLOAD_SIZE) {
		vlog("photos: bad size %lld for %s", (long long)sz, path);
		return;
	}

	SceDateTime stamp;
	sceRtcGetCurrentClockLocalTime(&stamp);
	/* notify=1: toast on
	 * completion since we suppressed the normal Email-app launch
	 * and the user otherwise gets no visual confirmation. */
	int qr = ps_uploader_enqueue_file(path, (int)sz, &stamp,
	                                  /*notify=*/1);
	vlog("photos: enqueue %s len=%lld -> %d", path, (long long)sz, qr);
}


/* scePafToplevelGetText hook. a1+0xC is the 32-bit id of the requested
 * label. Intercept "Send via email" (0xB8CFCC45) and substitute. */
static wchar_t *scePafToplevelGetTextPatched(void *a0, void *a1) {
	if (a1) {
		uint32_t id = *(uint32_t *)((char *)a1 + 0xC);
		if (!seen_text_id(id))
			vlog("paf_text: id=0x%08X", id);
		if (id == 0xB8CFCC45 && custom_warning)
			return (wchar_t *)custom_warning;
	}

	return TAI_CONTINUE(wchar_t *, scePafToplevelGetTextRef, a0, a1);
}

/* ASCII -> UTF-16LE buffer (no BOM; ScePaf passes the pointer straight
 * to the renderer). */
static uint16_t *make_u16(const char *ascii) {
	int n = 0;
	while (ascii[n]) n++;
	uint16_t *w = sce_paf_malloc((n + 1) * sizeof(uint16_t));
	if (!w) return NULL;
	for (int i = 0; i < n; i++) w[i] = (unsigned char)ascii[i];
	w[n] = 0;
	return w;
}

/* sysmodule-load hook — installs the GetText hook the moment ScePaf
 * has finished loading. Same trick CustomWarning uses, because at
 * module_start time PAF isn't in the Photos process's address space
 * yet so a direct import hook would never bind. */
static int sceSysmoduleLoadModuleInternalWithArgPatched(SceUInt32 id, SceSize args,
                                                        void *argp, void *unk) {
	int res = TAI_CONTINUE(int, sceSysmoduleLoadModuleInternalWithArgRef,
	                       id, args, argp, unk);

	if (res >= 0 && id == SCE_SYSMODULE_INTERNAL_PAF && photos_paf_text_hook < 0) {
		if (!custom_warning)
			custom_warning = make_u16("Upload to ssu");

		photos_paf_text_hook = taiHookFunctionImport(&scePafToplevelGetTextRef,
			TAI_MAIN_MODULE, 0x4D9A9DD0, 0x19CEFDA7,
			scePafToplevelGetTextPatched);
		vlog("photos: paf loaded, GetText hook=0x%08X warn=%p",
		     photos_paf_text_hook, custom_warning);
	}

	return res;
}

/* Public entry point. Called once from module_start when our module
 * happens to be loaded into NPXS10004 (anything that isn't SceShell).
 * `info_v` is a pointer to tai_module_info_t for the Photos main
 * module (passed as void* through the header so taihen.h doesn't
 * leak into pngshot.h). */
void ps_photos_init(const void *info_v) {
	const tai_module_info_t *info = info_v;

	vlog("photos: main module nid=0x%08X modid=0x%X — hooking email send",
	     info->module_nid, info->modid);

	/* FUN_81075b0c lives at file offset 0x75b0c in the photos
	 * app main eboot (load base 0x81000000). */
	photos_email_hook_id = taiHookFunctionOffset(&photos_email_hook,
		info->modid, 0, 0x75b0c, 1, photos_send_email);
	vlog("photos: email hook=0x%08X", photos_email_hook_id);

	photos_paf_load_hook = taiHookFunctionImport(
		&sceSysmoduleLoadModuleInternalWithArgRef,
		TAI_MAIN_MODULE, 0x03FCF19D, 0xC3C26339,
		sceSysmoduleLoadModuleInternalWithArgPatched);
	vlog("photos: sysmodule-load hook=0x%08X", photos_paf_load_hook);
}
