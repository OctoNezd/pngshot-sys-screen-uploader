#include <taihen.h>
#include <psp2/paf.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/rtc.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <libpng16/png.h>

#include <libk/string.h>

#include "pngshot.h"

int __errno = 0;
void abort(void) {
	__builtin_trap();
}

/* Current vitasdk exports these as sce_paf_malloc / sce_paf_free (the
 * original pngshot predates the rename and used the long-since-removed
 * `_private_` variants, which no longer exist in libScePaf_stub). */
extern void *sce_paf_malloc(size_t sz);
extern void  sce_paf_free(void *p);

/* The encode hook runs on SceShell's UI thread while the user is
 * holding PS+Start. We want a copy of the final PNG in RAM so we can
 * pass it to the upload worker without re-reading the file from flash
 * (SceShell hasn't finished writing it yet, anyway). This growable
 * capture buffer is owned by ScePaf and filled incrementally inside
 * write_func. It lives across a single encode_type2() call only.
 *
 * We cap the buffer at PS_MAX_UPLOAD_SIZE and silently disable capture
 * above that threshold so we never OOM SceShell. */
typedef struct {
	unsigned char *buf;
	int            len;
	int            cap;
	int            overflow;   /* set once we stopped growing */
	int            enabled;    /* set per encode: 1 iff config exists */
} capture_t;

static capture_t g_cap;

static void capture_reset(void) {
	if (g_cap.buf) sce_paf_free(g_cap.buf);
	g_cap.buf = 0;
	g_cap.len = 0;
	g_cap.cap = 0;
	g_cap.overflow = 0;
	g_cap.enabled = 0;
}

static void capture_append(const void *data, int len) {
	if (!g_cap.enabled || g_cap.overflow || len <= 0) return;
	if (g_cap.len + len > PS_MAX_UPLOAD_SIZE) {
		g_cap.overflow = 1;
		vlog("capture: over %d bytes, disabling", PS_MAX_UPLOAD_SIZE);
		return;
	}
	if (g_cap.len + len > g_cap.cap) {
		/* Grow geometrically. PS Vita screenshots are ~1 MB so 2x
		 * from 64 KB gets us there in 4 realloc steps. */
		int ncap = g_cap.cap ? g_cap.cap * 2 : 64 * 1024;
		while (ncap < g_cap.len + len) ncap *= 2;

		unsigned char *nb = sce_paf_malloc(ncap);
		if (!nb) {
			g_cap.overflow = 1;
			vlog("capture: sce_paf_malloc(%d) failed", ncap);
			return;
		}
		if (g_cap.buf) {
			for (int i = 0; i < g_cap.len; i++) nb[i] = g_cap.buf[i];
			sce_paf_free(g_cap.buf);
		}
		g_cap.buf = nb;
		g_cap.cap = ncap;
	}
	for (int i = 0; i < len; i++) g_cap.buf[g_cap.len + i] = ((const unsigned char *)data)[i];
	g_cap.len += len;
}

void *malloc(size_t sz) {
	return sce_paf_malloc(sz);
}

void free(void *p) {
	sce_paf_free(p);
}

//////

tai_hook_ref_t watermark_hook, encode_hook, encode_type2_hook, reg_hook;
tai_hook_ref_t photos_email_hook;

int get_key_int(const char *dir, const char *file, int *out) {
	if (strcmp(dir, "/CONFIG/PHOTO") == 0 && strcmp(file, "debug_screenshot") == 0) {
		*out = 1;
		return 0;
	}

	return TAI_CONTINUE(int, reg_hook, dir, file, out);
}

int place_watermark_hook() {
	return 0;
}

/* Photos app (NPXS10004) email-sender hook.
 *
 * Original FUN_81075b0c(undefined4 *param_1) builds an
 *   "email:send?to=&cc=&subject=...&body=...&attach=<path>"
 * URI and hands it to SceAppMgrUser_003C634F(0x20000, ...) which
 * launches the Email app. We hijack it: read the attached file off
 * disk and hand the bytes to our normal sys-screenuploader pipeline
 * instead. The original is *not* called → no Email app launch.
 *
 * Photos passes paths rooted at "photo0:/SCREENSHOT/...". photo0:
 * isn't a real partition userland code can sceIoOpen — internally it
 * resolves to ux0:picture (or the equivalent on internal storage).
 * Plain prefix swap to ux0:picture/ works for the cases we care
 * about (screenshots).
 *
 * param_1 is a pointer to a C-string pointer (the email format uses
 * `%s` with `*param_1` as the arg). */

/* Translate "photo0:/<rest>" -> "ux0:/picture/<rest>" into `out`.
 * Returns 0 on success, -1 if input doesn't have the photo0: prefix
 * or won't fit. */
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

void photos_send_email(void *param_1) {
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

	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) {
		vlog("photos: sceIoOpen %s -> 0x%08X", path, fd);
		return;
	}
	SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	if (sz <= 0 || sz > PS_MAX_UPLOAD_SIZE) {
		vlog("photos: bad size %lld for %s", (long long)sz, path);
		sceIoClose(fd);
		return;
	}

	void *buf = sce_paf_malloc((size_t)sz);
	if (!buf) {
		vlog("photos: malloc %lld failed", (long long)sz);
		sceIoClose(fd);
		return;
	}
	int total = 0;
	while (total < (int)sz) {
		int r = sceIoRead(fd, (char *)buf + total, (int)sz - total);
		if (r <= 0) break;
		total += r;
	}
	sceIoClose(fd);

	if (total != (int)sz) {
		vlog("photos: short read %d/%lld", total, (long long)sz);
		sce_paf_free(buf);
		return;
	}

	SceDateTime stamp;
	sceRtcGetCurrentClockLocalTime(&stamp);
	/* Photos path asks for a toast on completion so users get visual
	 * confirmation their share went somewhere (the email-app launch
	 * they'd normally see is suppressed by us). */
	int qr = ps_uploader_enqueue_notify(buf, total, &stamp, 1);
	vlog("photos: enqueue %s len=%d -> %d", path, total, qr);

	sce_paf_free(buf);

	/* Intentional: do NOT TAI_CONTINUE. We swallow the call so the
	 * Email app isn't launched. */
}

int encode_screenshot(void **ss_arg1, unsigned unk) {
	// set format to 2 = "raw" but we will hook and change that later
	((int*)(*ss_arg1))[8/4] = 2;

	return TAI_CONTINUE(int, encode_hook, ss_arg1, unk);
}

typedef struct picture_t picture_t;

typedef struct {
	int width;
	int height;
} dimensions_t;

typedef struct {
	void *field_0;
	void *field_4;
	void *field_8;
	int (*get_type)(picture_t *);
	int (*get_dimensions)(dimensions_t *, picture_t *);
	void *field_14;
	unsigned (*get_pixel)(picture_t *, int x, int y);
	void *field_1c;
	void *field_20;
} picture_vtable_t;

struct picture_t {
	picture_vtable_t *vptr;
};

typedef struct encode_t encode_t;

typedef struct {
	void *field_0;
	void *field_4;
	int (*is_buffer_init)(encode_t *);
	int (*append)(encode_t *, const void *buffer, size_t sz);
	void *field_10;
	void *field_14;
	void *field_18;
	void *field_1c;
	void *field_20;
} encode_vtable_t;

struct encode_t {
	encode_vtable_t *vptr;
} __attribute__((packed));

typedef struct {
	picture_t *picture;
	encode_t *encode;
	void *field_8;
} actual_encode_args_t;

typedef struct {
	int (*func)();
	unsigned field_4;
} unk_obj_t;

size_t g_png_size;

void write_func(png_structp png_ptr, png_bytep data, png_size_t length) {
	g_png_size += length;
	actual_encode_args_t *args = png_get_io_ptr(png_ptr);
	args->encode->vptr->append(args->encode, data, length);

	/* Mirror the bytes into our own buffer so the uploader thread
	 * can re-send them after SceShell finishes this encode call. We
	 * don't want to re-read from ux0:picture/SCREENSHOT because
	 * SceShell writes that file *after* this function returns. */
	capture_append(data, (int)length);
}

enum {
	ENCODE_ERROR1 = 0x80103001,
	ENCODE_ERROR = 0x80103002,
};

int encode_type2(actual_encode_args_t *args) {
	int ret = 0;
	unsigned *pixels = NULL;
	png_structp png_ptr = NULL;
	png_infop info_ptr = NULL;
	dimensions_t wh = {0};

	encode_t *encode = args->encode;
	picture_t *picture = args->picture;

	if (!encode || !encode->vptr->is_buffer_init(encode) || !picture || !picture->vptr->get_type(picture))
		return ENCODE_ERROR;

	g_png_size = 0;
	/* Only mirror the PNG if the user has opted into uploads by
	 * creating a config. Checked once up-front so write_func's hot
	 * loop just tests one flag per chunk — no per-write stat()s. */
	g_cap.enabled = ps_cfg_present();
	picture->vptr->get_dimensions(&wh, picture);

	png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png_ptr)
		goto cleanup;

	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr)
		goto cleanup;

	png_set_check_for_invalid_index(png_ptr, 0);

	png_set_IHDR(png_ptr, info_ptr, wh.width, wh.height, 8,
		PNG_COLOR_TYPE_RGBA,
		PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT,
		PNG_FILTER_TYPE_DEFAULT);

	png_set_write_fn(png_ptr, args, write_func, NULL);

	pixels = sce_paf_malloc(wh.width * 4);

	png_write_info(png_ptr, info_ptr);

	for (int i = 0; i < wh.height; ++i) {
		// haven't reversed what this does, just copied from assembly
		unk_obj_t *unk = args->field_8;
		if (unk && unk->func && unk->func(unk->field_4)) {
			ret = ENCODE_ERROR1;
			goto cleanup;
		}

		for (int j = 0; j < wh.width; ++j)
			pixels[j] = picture->vptr->get_pixel(picture, j, i) | 0xFF000000u;

		png_write_row(png_ptr, (void*)pixels);
	}

	png_write_end(png_ptr, info_ptr);

	/* Hand a copy of the PNG to the uploader worker. Safe to call
	 * with a NULL/empty buffer — enqueue validates the length.
	 * g_cap.enabled is false when the user hasn't configured uploads,
	 * in which case the capture buffer is empty and we silently skip. */
	if (g_cap.enabled && !g_cap.overflow && g_cap.len > 0) {
		SceDateTime stamp;
		sceRtcGetCurrentClockLocalTime(&stamp);
		ps_uploader_enqueue(g_cap.buf, g_cap.len, &stamp);
	}

cleanup:
	capture_reset();

	if (png_ptr || info_ptr)
		png_destroy_write_struct(&png_ptr, &info_ptr);

	if (pixels)
		sce_paf_free(pixels);

	if (ret < 0)
		return ret;

	return g_png_size;
}

int module_start() {
	tai_module_info_t info = {0};
	info.size = sizeof(info);
	taiGetModuleInfo(TAI_MAIN_MODULE, &info);
	vlog_init();
	vlog("pngshot module_start");

	/* Start the background uploader thread up-front — it's cheap
	 * and the first screenshot shouldn't have to pay for thread
	 * creation + module load. */
	ps_uploader_start();

	if (info.module_nid == 0x0552F692) { // 3.60 retail

		// disable watermark
		taiHookFunctionOffset(&watermark_hook, info.modid, 0, 0x247e00, 1, place_watermark_hook);

		// enable type=2 screenshot encoding
		taiHookFunctionOffset(&encode_hook, info.modid, 0, 0x365f46, 1, encode_screenshot);

		// replace type=2 encoding with our png implementation
		taiHookFunctionOffset(&encode_type2_hook, info.modid, 0, 0x36bd22, 1, encode_type2);

		// change branch for 0x34560004 (screenshot disable) to 0x34560003 (screenshot enable)
		int value = 0x3b;
		taiInjectData(info.modid, 0, 0x248840, &value, 2);

		// change extension from jpg to png
		const char *path = "ur0:temp/screenshot/capture.png";
		taiInjectData(info.modid, 0, 0x5148b8, path, strlen(path) + 1);
	} else if (info.module_nid == 0xEAB89D5C) //3.60 Testkit
	{
		// disable watermark
		taiHookFunctionOffset(&watermark_hook, info.modid, 0, 0x240234, 1, place_watermark_hook);

		// enable type=2 screenshot encoding
		taiHookFunctionOffset(&encode_hook, info.modid, 0, 0x35c98e, 1, encode_screenshot);

		// replace type=2 encoding with our png implementation
		taiHookFunctionOffset(&encode_type2_hook, info.modid, 0, 0x36276a, 1, encode_type2);

		// change branch for 0x34560004 (screenshot disable) to 0x34560003 (screenshot enable)
		int value = 0x3b;
		taiInjectData(info.modid, 0, 0x240C74, &value, 2);

		// change extension from jpg to png
		const char *path = "ur0:temp/screenshot/capture.png";
		taiInjectData(info.modid, 0, 0x508B18, path, strlen(path) + 1);
	}else if (info.module_nid == 0x5549BF1F ||
		   info.module_nid == 0x34B4D82E ||
		   info.module_nid == 0x12DAC0F3) { // 3.65/3.67/3.68 retail
		// disable watermark
		taiHookFunctionOffset(&watermark_hook, info.modid, 0, 0x247e9c, 1, place_watermark_hook);

		// enable type=2 screenshot encoding
		taiHookFunctionOffset(&encode_hook, info.modid, 0, 0x36638a, 1, encode_screenshot);

		// replace type=2 encoding with our png implementation
		taiHookFunctionOffset(&encode_type2_hook, info.modid, 0, 0x36c166, 1, encode_type2);

		// change branch for 0x34560004 (screenshot disable) to 0x34560003 (screenshot enable)
		int value = 0x3b;
		taiInjectData(info.modid, 0, 0x2488dc, &value, 2);

		// change extension from jpg to png
		const char *path = "ur0:temp/screenshot/capture.png";
		taiInjectData(info.modid, 0, 0x514df8, path, strlen(path) + 1);
	} else {
		/* Photos app (NPXS10004) — taiHEN config must load us under
		 * NPXS10004 for this branch to ever run. We don't gate on a
		 * specific module_nid yet because we only have one known
		 * sample; log it so future firmwares are easy to identify. */
		vlog("photos: main module nid=0x%08X modid=0x%X — hooking email send",
		     info.module_nid, info.modid);

		/* FUN_81075b0c lives at file offset 0x75b0c in the photos
		 * app main eboot (load base 0x81000000). */
		taiHookFunctionOffset(&photos_email_hook, info.modid, 0, 0x75b0c, 1, photos_send_email);
	}

	return 0;
}

int module_stop() {
	ps_uploader_stop();
	return 0;
}

