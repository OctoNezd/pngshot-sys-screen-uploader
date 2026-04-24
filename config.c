/* ux0:data/pngshot-ssu/config.txt parser.
 *
 * Minimal format: `key = value` lines, `#` / `;` comments, blank
 * lines ignored. Only two keys matter:
 *
 *   upload_url = http://your-sys-screenuploader/upload?file={filename}
 *   enabled    = 1
 *
 * `{filename}` gets expanded to the synthesised sys-screenuploader
 * filename (see uploader.c). That's the identifier the server uses
 * to pick apart the timestamp + title-id hash.
 *
 * Example config file is shipped as config.sample.txt. */

#include "pngshot.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

int ps_cfg_present(void) {
    SceIoStat st;
    return (sceIoGetstat(PS_CONFIG_PATH, &st) >= 0) ? 1 : 0;
}

static void rstrip(char *s) {
    int n = (int)ps_strlen(s);
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' ||
                     s[n-1] == ' '  || s[n-1] == '\t')) {
        s[--n] = '\0';
    }
}

static char *lstrip(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static int read_whole(const char *path, char *buf, int cap) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;
    int total = 0;
    while (total < cap - 1) {
        int r = sceIoRead(fd, buf + total, cap - 1 - total);
        if (r <= 0) break;
        total += r;
    }
    buf[total] = '\0';
    sceIoClose(fd);
    return total;
}

/* Local strncpy replacement. `dst_cap` is the buffer size *including*
 * the terminating NUL. sceClibStrncpy's semantics match ISO strncpy
 * (no guaranteed NUL) which isn't what we want here. */
static void cpy(char *dst, const char *src, size_t dst_cap) {
    if (dst_cap == 0) return;
    size_t n = ps_strlen(src);
    if (n >= dst_cap) n = dst_cap - 1;
    ps_memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Return codes:
 *    0 : config loaded OK, `cfg` is populated.
 *   -1 : config file missing. Caller should treat this as "uploads
 *        disabled" — *not* an error. The user simply hasn't opted in
 *        yet. We log this at most once per boot (see g_missing_logged)
 *        to avoid flooding psp2shell on every screenshot.
 *   -2 : config file present but malformed (missing upload_url).
 *        Logged every time so the user can fix it.
 */
static int g_missing_logged = 0;

int ps_cfg_load(ps_config_t *cfg) {
    ps_memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = 1;

    char buf[1024];
    int n = read_whole(PS_CONFIG_PATH, buf, sizeof(buf));
    if (n < 0) {
        if (!g_missing_logged) {
            vlog("config %s not found, uploads disabled", PS_CONFIG_PATH);
            g_missing_logged = 1;
        }
        return -1;
    }
    g_missing_logged = 0;  /* user created the file; reset latch */

    char *line = buf;
    while (line && *line) {
        char *nl = ps_strchr(line, '\n');
        if (nl) *nl = '\0';

        char *trim = lstrip(line);
        if (*trim == '#' || *trim == ';' || *trim == '\0') goto next;

        rstrip(trim);
        char *eq = ps_strchr(trim, '=');
        if (!eq) goto next;

        *eq = '\0';
        char *key = trim;
        char *val = lstrip(eq + 1);
        rstrip(key);
        rstrip(val);

        if      (!ps_strcmp(key, "upload_url")) cpy(cfg->upload_url, val, sizeof(cfg->upload_url));
        else if (!ps_strcmp(key, "enabled"))
            cfg->enabled = (val[0] == '1' || val[0] == 't' || val[0] == 'T' ||
                            val[0] == 'y' || val[0] == 'Y');

    next:
        if (!nl) break;
        line = nl + 1;
    }

    if (cfg->upload_url[0] == '\0') {
        vlog("config: upload_url is required");
        return -2;
    }
    return 0;
}
