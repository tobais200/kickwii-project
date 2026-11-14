/*
 * dlog.c - registro de diagnostico para la Wii real (KickWii 1.3-rc8).
 * Escribe sd:/apps/KickWii/log.txt (la sesion anterior queda en log-anterior.txt).
 * Se desactiva creando sd:/apps/KickWii/nolog.txt. Tope: 1 MB por sesion.
 * Nunca se registran tokens: las URLs se reducen al host.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <gccore.h>
#include <ogc/mutex.h>
#include <ogc/lwp_watchdog.h>
#include "dlog.h"

#define DLOG_DIR   "sd:/apps/KickWii/"
#define DLOG_BUF   (24 * 1024)
#define DLOG_MAX   (1024 * 1024)
static char buf[DLOG_BUF];
static int blen = 0, on = 0, mtx_ok = 0, dropped = 0;
static long written = 0;
static mutex_t m;
static u64 t0 = 0, last_flush = 0;

int dlog_enabled(void) { return on; }

void dlog_host(const char *url, char *out, int sz) {
    const char *h = url ? strstr(url, "://") : NULL;
    h = h ? h + 3 : (url ? url : "");
    int n = (int)strcspn(h, "/?#");
    if (n >= sz) n = sz - 1;
    memcpy(out, h, n); out[n] = 0;
}

void dlog_init(int sd_ok) {
    struct stat st;
    if (!mtx_ok) { LWP_MutexInit(&m, false); mtx_ok = 1; }
    t0 = gettime();
    on = 0;
    if (!sd_ok) return;
    if (stat(DLOG_DIR "nolog.txt", &st) == 0) return;
    mkdir("sd:/apps", 0777); mkdir("sd:/apps/KickWii", 0777);
    remove(DLOG_DIR "log-anterior.txt");
    rename(DLOG_DIR "log.txt", DLOG_DIR "log-anterior.txt");
    FILE *f = fopen(DLOG_DIR "log.txt", "w");
    if (!f) return;
    fclose(f);
    on = 1; written = 0;
}

void dlog(const char *fmt, ...) {
    char line[400];
    va_list ap;
    if (!on || !mtx_ok) return;
    u32 ms = ticks_to_millisecs(gettime() - t0);
    int n = snprintf(line, sizeof(line), "[%5lu.%01lu] ", (unsigned long)(ms / 1000), (unsigned long)((ms % 1000) / 100));
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    n = strlen(line);
    line[n++] = '\n'; line[n] = 0;
    LWP_MutexLock(m);
    if (blen + n < DLOG_BUF) { memcpy(buf + blen, line, n); blen += n; }
    else dropped++;
    LWP_MutexUnlock(m);
}

void dlog_flush(int force) {
    static char out[DLOG_BUF];
    int n, dr;
    if (!on) return;
    u64 now = gettime();
    if (!force && ticks_to_millisecs(now - last_flush) < 2000) return;
    last_flush = now;
    LWP_MutexLock(m);
    n = blen; dr = dropped;
    if (n) memcpy(out, buf, n);
    blen = 0; dropped = 0;
    LWP_MutexUnlock(m);
    if (!n && !dr) return;
    if (written > DLOG_MAX) return;
    FILE *f = fopen(DLOG_DIR "log.txt", "a");
    if (!f) return;
    if (n) fwrite(out, 1, n, f);
    if (dr) fprintf(f, "(%d lineas perdidas: buffer lleno)\n", dr);
    written += n;
    if (written > DLOG_MAX) fputs("(log.txt llego a 1 MB: no se escribe mas en esta sesion)\n", f);
    fclose(f);
}
