/* Log through sceClibPrintf — visible via princesslog / psp2shell /
 * netdbg. We deliberately don't touch the filesystem from the logger
 * to keep the encode hook (which runs on SceShell's UI thread while
 * a screenshot is being written) as light as possible. */

#include "pngshot.h"

void vlog_init(void) { /* nothing */ }

void vlog(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    ps_vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    sceClibPrintf("[pngshot-ssu] %s\n", msg);
}
