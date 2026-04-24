/* Log everything through sceClibPrintf. princesslog (and
 * psp2shell / netdbg) will pick these up. No file logs are written —
 * we don't want to scribble over the user's FS. */

#include <stdarg.h>              /* va_list, va_start, va_end */
#include <psp2/kernel/clib.h>    /* sceClibPrintf, sceClibVsnprintf */

void vlog_init(void) { /* nothing */ }

void vlog(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    sceClibVsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    sceClibPrintf("[pngshot-screenuploader] %s\n", msg);
}
