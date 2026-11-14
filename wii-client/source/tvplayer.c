/*
 * tvplayer.c - reproductor de TV en vivo (HLS) para Wii:
 *   hilo de red    : libcurl (HTTP/HTTPS) baja la lista maestra, elige la variante
 *                    mas liviana, recarga la lista en vivo y baja segmentos MPEG-TS (Kick)
 *                    o fMP4/CMAF (EXT-X-MAP + fragmentos moof/mdat, sin cifrado)
 *   hilo demux     : libavformat (mpegts o mov) lee los segmentos encadenados; el audio AAC
 *                    se decodifica aca mismo (libavcodec) -> 48 kHz estereo -> ASND
 *   hilo de video  : libavcodec H.264 -> cuadros YUV 4:2:0 reducidos a <= 640 px
 *                    con descarte adaptativo (B-frames / solo claves) si la CPU no da
 *   hilo principal : tp_picture() elige el cuadro segun el reloj del audio
 *
 * rc6 (arreglo "0 fps despues de un rato" en la Wii real):
 *   - vigilante (wd_tick, desde tp_picture): si no llegan segmentos, el video no avanza,
 *     el audio se queda quieto, no arranca o hubo un error de red, reinicia los hilos desde la
 *     lista maestra (con token renovado en Kick) mostrando "Reconectando..."; tras 2 reintentos
 *     baja a 160p.
 *   - fallos de segmento contados: 3 seguidos -> renovar token / volver a la lista maestra.
 *   - lista "congelada" (sin segmentos nuevos) -> volver a la lista maestra.
 *   - limite de renovaciones por tiempo (antes: 20 por sesion, se agotaba en sesiones largas).
 *   - timeouts por pedido (lista 12 s, segmento 25 s, velocidad minima 4 KB/s en 8 s).
 *   - el decodificador ya no se bloquea si el reloj del audio se para (se descarta el cuadro
 *     mas viejo y se muestra con reloj de pared); prioridad del hilo de video 40 -> 52
 *     (por encima del chat y los avisos).
 *   - registro en sd:/apps/KickWii/log.txt (dlog.c).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <unistd.h>
#include <math.h>
#include <gccore.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/lwp_watchdog.h>
#include <asndlib.h>
#include <curl/curl.h>
#include <sys/socket.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include "tv.h"
#include "dlog.h"

/* ------------------------------------------------------------------ audio */
#define NBLK      64
#define BLKFR     2048                    /* 42.7 ms por bloque */
#define BLKBYTES  (BLKFR * 4)
#define START_BLK 20                      /* ~0.85 s antes de empezar */
enum { B_FREE = 0, B_FULL, B_QUEUED };
static short *blk[NBLK];
static double blk_pts[NBLK];
static volatile int blk_state[NBLK];
static volatile int play_next = 0, voice_on = 0, starved = 0;
static int wr_blk = 0, wr_fill = 0;
static volatile double clk_pts = 0;
static volatile u64 clk_t0 = 0;
static volatile int clk_valid = 0;
static int volume = 220;
static volatile float alevel = 0;
static volatile int blk_peak[NBLK];

/* ------------------------------------------------------------------ estado */
static volatile int pstate = TP_IDLE, abort_req = 0, net_eof = 0;
static int cur_vmode = VM_AUTO;
static char perr[256] = "", pstatus[96] = "";
static char purl[4096];
static lwp_t th_net = LWP_THREAD_NULL, th_dmx = LWP_THREAD_NULL, th_vid = LWP_THREAD_NULL;
static mutex_t mtx;
static volatile long long net_bytes = 0;
static volatile int has_audio = 0, has_video = 0, stream_w = 0, stream_h = 0;
static volatile int nframes = 0, ndropped = 0, nshown = 0;
volatile u32 tp_dec_us = 0;
static double fps_val = 0;
static int fps_cnt = 0; static u64 fps_t = 0;
static char variant_info[64] = "";
static int sd_stream = 1;
/* Kick: limite de variante y refresco del token */
static int lim_h = 0;                 /* 0 = sin limite (comportamiento TV Gratis) */
static char refresh_api[200] = "";
static char fallback_url[4096] = "";   /* variante mas liviana (160p) para bajar si la red no da */
static int cur_h = 0, fallback_h = 0;
static volatile int n_refresh = 0, n_downgrade = 0;
static long last_code = 0;
static volatile int force_refresh = 0;
static volatile u64 last_pic_call = 0;  /* la UI esta mostrando video (no hay un menu abierto) */
void tp_force_refresh(void) { force_refresh = 1; }   /* pruebas: simula token vencido */
static volatile int force_reinit = 0;
void tp_force_reinit(void) { force_reinit = 1; }     /* pruebas: simula un init fMP4 distinto (anuncio) */

/* ------------------------------------------------------------------ vigilante (rc6) */
static volatile int net_done = 1, dmx_done = 1, vid_done = 1;
static volatile u64 last_seg_t = 0, last_show_t = 0, play_t0 = 0;
static volatile int n_seg_ok = 0, n_seg_fail = 0, fatal_err = 0, lowq = 0;
static volatile u32 last_seg_ms = 0;
static volatile u64 last_rx_t = 0;
static volatile long long cur_seq = -1;
static int n_restart = 0, restart_streak = 0, wd_phase = 0;
static u64 wd_t = 0, err_t = 0, last_clk_move = 0, wd_log_t = 0, restart_ok_t = 0;
static double last_clk_val = -1;
static u64 refresh_hist[6];
static int refresh_hist_n = 0;
static int start_threads(void);
#define FATAL(...) do { snprintf(perr, sizeof(perr), __VA_ARGS__); fatal_err = 1; } while (0)

/* ------------------------------------------------------------------ cola de segmentos */
typedef struct seg { unsigned char *d; int n; struct seg *next; } seg_t;   /* n == 0: marca "nuevo init fMP4" */
static volatile int reopen_req = 0;     /* el demuxer tiene que reabrirse (cambio el init fMP4, p. ej. anuncio) */
static volatile int vgen = 0;           /* generacion de parametros de video (cambia al reabrir) */
static seg_t *sq_head = NULL, *sq_tail = NULL;
static volatile int sq_bytes = 0, sq_count = 0;
static seg_t *rd_seg = NULL; static int rd_pos = 0;

static void sq_push(unsigned char *d, int n) {
    seg_t *s = malloc(sizeof(seg_t));
    if (!s) { free(d); return; }
    s->d = d; s->n = n; s->next = NULL;
    LWP_MutexLock(mtx);
    if (sq_tail) sq_tail->next = s; else sq_head = s;
    sq_tail = s; sq_bytes += n; sq_count++;
    LWP_MutexUnlock(mtx);
}
static seg_t *sq_pop(void) {
    seg_t *s;
    LWP_MutexLock(mtx);
    s = sq_head;
    if (s) { sq_head = s->next; if (!sq_head) sq_tail = NULL; sq_bytes -= s->n; sq_count--; }
    LWP_MutexUnlock(mtx);
    return s;
}
static void sq_clear(void) {
    seg_t *s;
    while ((s = sq_pop())) { free(s->d); free(s); }
    if (rd_seg) { free(rd_seg->d); free(rd_seg); rd_seg = NULL; }
    rd_pos = 0;
}

/* ------------------------------------------------------------------ cola de paquetes de video */
#define VQ_MAX 360
static AVPacket *vq[VQ_MAX];
static volatile int vq_r = 0, vq_w = 0, vq_bytes = 0;
static int vq_needkey = 0;
static AVCodecParameters *vpar = NULL;
static AVRational vtb;
static int vq_count(void) { return (vq_w - vq_r + VQ_MAX) % VQ_MAX; }

/* ------------------------------------------------------------------ cuadros de salida */
#define NPIC 4
#define PIC_MAXW 640
#define PIC_MAXH 480
enum { P_FREE = 0, P_WRITING, P_READY, P_CUR };
static tv_pic_t pics[NPIC];
static volatile int pic_state[NPIC];
static int cur_pic = -1;
static double anchor_pts = 0; static u64 anchor_t = 0; static int anchor_ok = 0;

/* ================================================================== DSP */
static void voice_cb(s32 v) {
    int k = play_next;
    (void)v;
    if (!voice_on) return;
    if (blk_state[k] == B_FULL) {
        if (ASND_AddVoice(0, blk[k], BLKBYTES) == SND_OK) {
            int prev = (k + NBLK - 1) % NBLK, done = (k + NBLK - 2) % NBLK;
            blk_state[k] = B_QUEUED;
            if (blk_state[done] == B_QUEUED) blk_state[done] = B_FREE;
            clk_pts = blk_pts[prev]; clk_t0 = gettime(); clk_valid = 1;
            alevel = blk_peak[prev] / 32768.0f;
            play_next = (k + 1) % NBLK;
            starved = 0;
        }
    } else starved = 1;
}

static void voice_start(void) {
    int k = play_next;
    if (blk_state[k] != B_FULL) return;
    blk_state[k] = B_QUEUED;
    play_next = (k + 1) % NBLK;
    starved = 0;
    voice_on = 1;
    clk_pts = blk_pts[k]; clk_t0 = gettime(); clk_valid = 1;
    ASND_SetVoice(0, VOICE_STEREO_16BIT, 48000, 0, blk[k], BLKBYTES, volume, volume, voice_cb);
}

static void voice_reset(void) {
    int i;
    voice_on = 0;
    ASND_StopVoice(0);
    for (i = 0; i < NBLK; i++) blk_state[i] = B_FREE;
    play_next = 0; wr_blk = 0; wr_fill = 0; starved = 0; clk_valid = 0; alevel = 0;
}

static int full_blocks(void) {
    int i, n = 0;
    for (i = 0; i < NBLK; i++) if (blk_state[i] == B_FULL) n++;
    return n;
}

static double tp_clock(int *valid) {
    if (has_audio) {
        if (!clk_valid) { *valid = 0; return 0; }
        double e = ticks_to_microsecs(gettime() - clk_t0) / 1e6;
        if (e > BLKFR / 48000.0 * 1.5) e = BLKFR / 48000.0 * 1.5;
        *valid = 1;
        return clk_pts + e;
    }
    if (!anchor_ok) { *valid = 0; return 0; }
    *valid = 1;
    return anchor_pts + ticks_to_microsecs(gettime() - anchor_t) / 1e6;
}

void tp_init(void) {
    int i;
    ASND_Init();
    ASND_Pause(0);
    LWP_MutexInit(&mtx, false);
    for (i = 0; i < NBLK; i++) {
        blk[i] = memalign(32, BLKBYTES);
        memset(blk[i], 0, BLKBYTES);
    }
    for (i = 0; i < NPIC; i++) {
        pics[i].y = memalign(32, PIC_MAXW * PIC_MAXH);
        pics[i].u = memalign(32, PIC_MAXW * PIC_MAXH / 4);
        pics[i].v = memalign(32, PIC_MAXW * PIC_MAXH / 4);
        pic_state[i] = P_FREE;
    }
    av_log_set_level(AV_LOG_QUIET);
}

/* bloquea mientras el anillo esta lleno */
static void push_pcm(const short *p, int frames, double pts) {
    while (frames > 0 && !abort_req) {
        if (blk_state[wr_blk] != B_FREE) {
            if (!voice_on && full_blocks() >= START_BLK) voice_start();
            usleep(6000); continue;
        }
        if (wr_fill == 0) { blk_pts[wr_blk] = pts; blk_peak[wr_blk] = 0; }
        int take = BLKFR - wr_fill, i, pk = blk_peak[wr_blk];
        if (take > frames) take = frames;
        memcpy(blk[wr_blk] + wr_fill * 2, p, take * 4);
        for (i = 0; i < take * 2; i += 8) { int a = p[i] < 0 ? -p[i] : p[i]; if (a > pk) pk = a; }
        blk_peak[wr_blk] = pk;
        wr_fill += take; p += take * 2; frames -= take; pts += take / 48000.0;
        if (wr_fill == BLKFR) {
            DCFlushRange(blk[wr_blk], BLKBYTES);
            blk_state[wr_blk] = B_FULL;
            wr_blk = (wr_blk + 1) % NBLK;
            wr_fill = 0;
            int fb = full_blocks();
            if (!voice_on && fb >= START_BLK) { voice_start(); pstate = TP_PLAYING; }
            else if (voice_on && starved && fb >= 8) {
                /* el DSP se quedo sin audio: reanudar */
                int i2;
                ASND_StopVoice(0);
                for (i2 = 0; i2 < NBLK; i2++) if (blk_state[i2] == B_QUEUED) blk_state[i2] = B_FREE;
                while (blk_state[play_next] != B_FULL) play_next = (play_next + 1) % NBLK;
                voice_start();
                pstate = TP_PLAYING;
            }
        }
    }
}

/* ================================================================== red / HLS */
typedef struct { unsigned char *d; size_t n, cap, max; } mem_t;
static CURL *nc = NULL;
static size_t mem_wr(char *p, size_t s, size_t m, void *u) {
    mem_t *b = u; size_t n = s * m;
    if (abort_req) return 0;
    if (!b->cap) {
        /* reservar de una vez segun Content-Length (evita realloc y fragmentacion) */
        curl_off_t cl = -1;
        curl_easy_getinfo(nc, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
        if (cl > 0 && (size_t)cl + 1 <= b->max) {
            b->d = malloc((size_t)cl + 1 + 4096);
            if (b->d) b->cap = (size_t)cl + 1 + 4096;
        }
    }
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256 * 1024;
        while (nc < b->n + n + 1) nc *= 2;
        if (nc > b->max) nc = b->max;
        if (b->n + n + 1 > nc) return 0;
        unsigned char *nd = realloc(b->d, nc);
        if (!nd) return 0;
        b->d = nd; b->cap = nc;
    }
    memcpy(b->d + b->n, p, n); b->n += n; b->d[b->n] = 0;
    net_bytes += n; last_rx_t = gettime();
    return n;
}
static int net_prog(void *c, curl_off_t a, curl_off_t b, curl_off_t d, curl_off_t e) {
    (void)c; (void)a; (void)b; (void)d; (void)e;
    return abort_req ? 1 : 0;
}

/* rc6: sockets del hilo de red. Si IOS deja un recv() bloqueado (conexion muerta por Wi-Fi), curl
   no puede aplicar sus timeouts; el vigilante los corta con shutdown() desde el hilo principal. */
#define NSOCK 8
static volatile int socks[NSOCK] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static volatile u64 xfer_t0 = 0; static volatile long xfer_tmo = 0;
static int n_sock_kill = 0;
static curl_socket_t open_sock(void *cl, curlsocktype purpose, struct curl_sockaddr *a) {
    int i; curl_socket_t fd;
    (void)cl; (void)purpose;
    fd = socket(a->family, a->socktype, a->protocol);
    if (fd >= 0) for (i = 0; i < NSOCK; i++) if (socks[i] < 0) { socks[i] = fd; break; }
    return fd;
}
static int close_sock(void *cl, curl_socket_t fd) {
    int i; (void)cl;
    for (i = 0; i < NSOCK; i++) if (socks[i] == fd) socks[i] = -1;
    return close(fd);
}
static void kill_socks(const char *why) {
    int i, n = 0;
    for (i = 0; i < NSOCK; i++) if (socks[i] >= 0) { shutdown(socks[i], SHUT_RDWR); n++; }
    n_sock_kill++;
    dlog("red: corto %d socket(s) del video (%s)", n, why);
}

static char eff_url[4096];
static int fetch_t(const char *url, mem_t *m, size_t max, long tmo) {
    char errbuf[CURL_ERROR_SIZE] = "";
    long code = 0; char *eu = NULL;
    m->d = NULL; m->n = 0; m->cap = 0; m->max = max;
    curl_easy_setopt(nc, CURLOPT_TIMEOUT, tmo);
    curl_easy_setopt(nc, CURLOPT_URL, url);
    curl_easy_setopt(nc, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(nc, CURLOPT_WRITEDATA, m);
    xfer_tmo = tmo; last_rx_t = xfer_t0 = gettime();
    CURLcode r = curl_easy_perform(nc);
    xfer_t0 = 0;
    curl_easy_getinfo(nc, CURLINFO_RESPONSE_CODE, &code);
    last_code = code;
    curl_easy_getinfo(nc, CURLINFO_EFFECTIVE_URL, &eu);
    snprintf(eff_url, sizeof(eff_url), "%s", eu ? eu : url);
    if (r != CURLE_OK || code < 200 || code >= 300 || !m->n) {
        if (!abort_req) {
            if (r != CURLE_OK) snprintf(perr, sizeof(perr), "Red: %s %s", curl_easy_strerror(r), errbuf);
            else snprintf(perr, sizeof(perr), "El servidor respondio HTTP %ld", code);
            char h[96]; dlog_host(url, h, sizeof(h));
            dlog("red: fallo %s curl=%d http=%ld bytes=%u %s", h, (int)r, code, (unsigned)m->n, r != CURLE_OK ? curl_easy_strerror(r) : "");
        }
        free(m->d); m->d = NULL; m->n = 0;
        return 0;
    }
    if (pstate != TP_ERROR) perr[0] = 0;
    return 1;
}
#define fetch(u, m, max) fetch_t(u, m, max, 12L)          /* listas y pedidos chicos */
#define fetch_seg(u, m) fetch_t(u, m, 24 * 1024 * 1024, 25L)

/* POST (solo para renovar el acceso de Twitch por GQL); deja el handle en GET al salir */
__attribute__((unused)) static int fetch_post(const char *url, const char *body, struct curl_slist *hl, mem_t *m, size_t max) {
    curl_easy_setopt(nc, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(nc, CURLOPT_POSTFIELDS, body);
    int ok = fetch(url, m, max);
    curl_easy_setopt(nc, CURLOPT_HTTPHEADER, NULL);
    curl_easy_setopt(nc, CURLOPT_HTTPGET, 1L);
    return ok;
}

static void resolve(const char *base, const char *rel, char *out, int sz) {
    if (!strncmp(rel, "http://", 7) || !strncmp(rel, "https://", 8)) { snprintf(out, sz, "%s", rel); return; }
    if (rel[0] == '/' && rel[1] == '/') { const char *c = strstr(base, "//"); snprintf(out, sz, "%.*s%s", c ? (int)(c - base) : 0, base, rel); return; }
    if (rel[0] == '/') {
        const char *h = strstr(base, "//");
        const char *p = h ? strchr(h + 2, '/') : NULL;
        snprintf(out, sz, "%.*s%s", p ? (int)(p - base) : (int)strlen(base), base, rel);
        return;
    }
    const char *q = strchr(base, '?');
    int bl = q ? (int)(q - base) : (int)strlen(base);
    while (bl > 0 && base[bl - 1] != '/') bl--;
    snprintf(out, sz, "%.*s%s", bl, base, rel);
}

static char *next_line(char **p) {
    char *s = *p, *e;
    if (!s || !*s) return NULL;
    e = s + strcspn(s, "\r\n");
    if (*e) { *e++ = 0; while (*e == '\r' || *e == '\n') e++; }
    *p = e;
    return s;
}

/* elige la variante. Sin limite: la mas chica con video. Con limite (Kick): la mas
   grande con alto <= lim_h (360p), y guarda la mas liviana (160p) como reserva.
   Nunca elige >= 480p cuando hay limite. */
static int pick_variant(char *txt, const char *base, char *out, int sz) {
    char *p = txt, *l, *pending = NULL;
    long best = -1; int found = 0;
    long fb_best = -1;
    char pend[512];
    fallback_url[0] = 0; fallback_h = 0; cur_h = 0;
    while ((l = next_line(&p))) {
        if (!strncmp(l, "#EXT-X-STREAM-INF", 17)) { snprintf(pend, sizeof(pend), "%s", l); pending = pend; continue; }
        if (pending && l[0] && l[0] != '#') {
            int w = 0, h = 0; long bw = 0; char *r;
            if ((r = strstr(pending, "RESOLUTION="))) sscanf(r + 11, "%dx%d", &w, &h);
            if ((r = strstr(pending, "BANDWIDTH="))) bw = atol(r + 10);
            int audio_only = (strstr(pending, "CODECS=") && !strstr(pending, "avc1") && !strstr(pending, "avc3") && !w);
            long score;
            if (lim_h > 0) {
                if (!h || h > lim_h || audio_only) { pending = NULL; continue; }   /* nunca 480p+ */
                if (fb_best < 0 || h < fb_best) { fb_best = h; resolve(base, l, fallback_url, sizeof(fallback_url)); fallback_h = h; }
                score = cur_vmode == VM_AUDIO ? bw : (long)(10000 - h);        /* la mas alta <= limite */
            }
            else if (cur_vmode == VM_AUDIO) score = bw;
            else if (audio_only) score = 0x7FFFFFF0;
            else if (w) {
                score = (long)w * h;
                if (h < 200) score += 200000;   /* 144p/180p solo si no hay otra */
            } else score = 300000 + bw / 100;
            if (best < 0 || score < best) {
                best = score; found = 1; cur_h = h;
                resolve(base, l, out, sz);
                if (w) snprintf(variant_info, sizeof(variant_info), "%dp %ld kbps", h, bw / 1000);
                else snprintf(variant_info, sizeof(variant_info), "%ld kbps", bw / 1000);
            }
            pending = NULL;
        }
    }
    if (fallback_h >= cur_h) fallback_url[0] = 0;
    if (found && lowq && fallback_url[0]) {   /* rc6: tras reintentos, directo a la mas liviana */
        snprintf(out, sz, "%s", fallback_url);
        cur_h = fallback_h; fallback_url[0] = 0;
        snprintf(variant_info, sizeof(variant_info), "%dp (auto, liviano)", fallback_h);
    }
    if (found) dlog("hls: variante %s%s", variant_info, lowq ? " [modo liviano]" : "");
    return found;
}

/* baja la lista maestra (y sub-maestras) y deja en url la variante elegida */
static int open_master(const char *murl, char *url, int usz) {
    mem_t m;
    int level;
    snprintf(url, usz, "%s", murl);
    for (level = 0; level < 3 && !abort_req; level++) {
        if (!fetch(url, &m, 2 * 1024 * 1024)) return 0;
        if (!strstr((char *)m.d, "#EXTM3U")) { FATAL("La direccion no es una lista HLS (.m3u8)."); free(m.d); return 0; }
        if (!strstr((char *)m.d, "#EXT-X-STREAM-INF")) { free(m.d); return 1; }
        char base[4096]; snprintf(base, sizeof(base), "%s", eff_url);
        char nu[4096];
        if (!pick_variant((char *)m.d, base, nu, sizeof(nu))) {
            free(m.d);
            FATAL("%s", lim_h ? "El canal no ofrece 360p/160p (la Wii no puede con 480p o mas)." : "Lista maestra sin variantes.");
            return 0;
        }
        free(m.d);
        snprintf(url, usz, "%s", nu);
    }
    return 1;
}

/* Kick: el token JWT de playback_url vence (~10 min). Pide de nuevo el canal (a kick.com
   o al companion, que responde con la misma forma) y vuelve a abrir la lista maestra.
   Una sola peticion, solo si el video fallo. */
int kick_parse_channel(const char *json, kstream_t *info, char *url, int usz);
static int refresh_token(char *url, int usz) {
    mem_t m;
    char murl[4096];
    kstream_t ks;
    if (!refresh_api[0] || abort_req) return 0;
    /* rc6: como mucho 6 renovaciones cada 5 minutos (antes 20 por sesion: en sesiones largas
       se agotaban y el video quedaba parado) */
    {
        u64 now = gettime();
        if ((refresh_hist_n >= 6 && ticks_to_millisecs(now - refresh_hist[refresh_hist_n % 6]) < 300000) ||
            (refresh_hist_n && ticks_to_millisecs(now - refresh_hist[(refresh_hist_n - 1) % 6]) < 8000)) {
            dlog("hls: renovacion de token frenada (max. 1 cada 8 s y 6 cada 5 min)");
            return 0;
        }
        refresh_hist[refresh_hist_n % 6] = now; refresh_hist_n++;
    }
    n_refresh++;
    dlog("hls: renovando token/lista maestra (#%d)", n_refresh);
    snprintf(pstatus, sizeof(pstatus), "Renovando acceso...");
    if (!fetch(refresh_api, &m, 512 * 1024)) return 0;
    memset(&ks, 0, sizeof(ks));
    int live = kick_parse_channel((char *)m.d, &ks, murl, sizeof(murl));
    free(m.d);
#ifdef STRESSTEST
    if (live != 1 || strncmp(murl, "http", 4)) {
#else
    if (live != 1 || strncmp(murl, "https://", 8)) {
#endif
        if (live == 0) FATAL("La transmision termino (el canal ya no esta en vivo).");
        else snprintf(perr, sizeof(perr), "Kick no devolvio el canal (reintento).");
        dlog("hls: renovacion sin URL (live=%d)", live);
        return 0;
    }
    snprintf(purl, sizeof(purl), "%s", murl);
    return open_master(purl, url, usz);
}

/* vuelve a pedir la lista maestra (Kick: con token nuevo) y elige variante otra vez */
static int remaster(char *url, int usz, const char *why) {
    static u64 last_rm = 0; static int rm_streak = 0;
    u64 now = gettime();
    /* espera creciente si se repite seguido (no martillar a Kick mientras la falla sigue) */
    if (last_rm && ticks_to_millisecs(now - last_rm) < 15000) { if (rm_streak < 4) rm_streak++; } else rm_streak = 0;
    last_rm = now;
    if (rm_streak) {
        int k, ms = 1000 << rm_streak;          /* 2, 4, 8, 16 s */
        for (k = 0; k < ms / 50 && !abort_req; k++) usleep(50000);
        if (abort_req) return 0;
        last_rm = gettime();
    }
    dlog("hls: vuelvo a la lista maestra (%s)", why);
    if (refresh_api[0] && refresh_token(url, usz)) return 1;
    if (fatal_err || abort_req) return 0;
    return open_master(purl, url, usz);
}

static void *net_thread_body(void) {
    static char url[4096], segu[4096], cur_map[4096], mapu[4096];
    unsigned char *last_init = NULL; int last_init_n = 0;
    mem_t m;
    int fails = 0, slow = 0, cpu_d0 = 0, cpu_f0 = 0, cpu_s0 = 0, seg_streak = 0, behind = 0;
    cur_map[0] = 0;
    u64 cpu_t = 0, behind_t = 0, end_t = 0;
    long long next_seq = -1, last_end = -1;
    nc = curl_easy_init();
    if (!nc) { snprintf(perr, sizeof(perr), "No se pudo iniciar libcurl."); pstate = TP_ERROR; net_eof = 1; return NULL; }
    {
        static char eb[CURL_ERROR_SIZE];
        sh_curl_common(nc, eb);
    }
    curl_easy_setopt(nc, CURLOPT_WRITEFUNCTION, mem_wr);
    curl_easy_setopt(nc, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(nc, CURLOPT_XFERINFOFUNCTION, net_prog);
    /* rc6: un socket colgado se corta en 8 s (antes 15 s x 2 intentos por segmento) */
    curl_easy_setopt(nc, CURLOPT_LOW_SPEED_LIMIT, 4000L);
    curl_easy_setopt(nc, CURLOPT_LOW_SPEED_TIME, 8L);
    curl_easy_setopt(nc, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(nc, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(nc, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(nc, CURLOPT_DNS_CACHE_TIMEOUT, 600L);  /* menos resoluciones DNS bloqueantes */
    curl_easy_setopt(nc, CURLOPT_BUFFERSIZE, 16L * 1024);   /* recv() de IOS falla con bloques grandes por HTTP plano ("Not enough space") */
    curl_easy_setopt(nc, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(nc, CURLOPT_OPENSOCKETFUNCTION, open_sock);
    curl_easy_setopt(nc, CURLOPT_CLOSESOCKETFUNCTION, close_sock);

    snprintf(pstatus, sizeof(pstatus), n_restart ? "Reconectando..." : "Conectando...");
    if (n_restart && refresh_api[0]) {
        /* reinicio del vigilante: el token viejo puede estar vencido */
        if (!refresh_token(url, sizeof(url))) {
            if (fatal_err || abort_req || !open_master(purl, url, sizeof(url))) goto fail;
        }
    } else if (!open_master(purl, url, sizeof(url))) {
        if (abort_req || fatal_err || !(last_code == 403 || last_code == 401) || !refresh_token(url, sizeof(url))) goto fail;
    }
    {
        int w = 0, h = 0; char *x = strchr(variant_info, 'x');
        if (x) { w = atoi(variant_info); h = atoi(x + 1); }
        sd_stream = !w || (w * h <= 640 * 368);
        if (lim_h) sd_stream = 1;
    }
    snprintf(pstatus, sizeof(pstatus), n_restart ? "Reconectando..." : "Cargando...");
    last_seg_t = gettime();
    while (!abort_req) {
        if (force_reinit && cur_map[0]) {
            force_reinit = 0; cur_map[0] = 0;
            free(last_init); last_init = malloc(1); last_init_n = -1;   /* el proximo init se toma como distinto */
        }
        if (force_refresh && refresh_api[0]) {
            force_refresh = 0;
            if (!refresh_token(url, sizeof(url))) goto fail;
            snprintf(pstatus, sizeof(pstatus), "Cargando...");
        }
        if (!fetch(url, &m, 2 * 1024 * 1024)) {
            /* 403/401/404 en la lista: casi siempre token vencido -> renovar */
            if (!abort_req && refresh_api[0] && (last_code == 403 || last_code == 401 || last_code == 404 || fails >= 2)) {
                if (refresh_token(url, sizeof(url))) { fails = 0; next_seq = -1; snprintf(pstatus, sizeof(pstatus), "Cargando..."); continue; }
                if (fatal_err) goto fail;
                fails++;   /* renovacion fallida (red): esperar y seguir; el vigilante decide */
            } else fails++;
            if (fails >= 6) {
                if (abort_req || fatal_err || !remaster(url, sizeof(url), "6 fallos de lista")) goto fail;
                fails = 0; next_seq = -1;
            }
            usleep(1000 * 1000); continue;
        }
        static char base[4096]; snprintf(base, sizeof(base), "%s", eff_url);
        char *p = (char *)m.d, *l, *map_now = NULL;
        int td = 6, endlist = 0, nseg = 0, i;
        long long seq0 = 0;
        static char *seg_uri[2048], *seg_map[2048];
        while ((l = next_line(&p))) {
            if (!strncmp(l, "#EXT-X-TARGETDURATION:", 22)) td = atoi(l + 22);
            else if (!strncmp(l, "#EXT-X-MEDIA-SEQUENCE:", 22)) seq0 = atoll(l + 22);
            else if (!strncmp(l, "#EXT-X-ENDLIST", 14)) endlist = 1;
            else if (!strncmp(l, "#EXT-X-KEY:", 11) && !strstr(l, "METHOD=NONE")) {
                FATAL("Transmision cifrada (DRM/AES): no compatible.");
                free(m.d); goto fail;
            } else if (!strncmp(l, "#EXT-X-MAP:", 11)) {
                /* fMP4 (Twitch): segmento de inicializacion (moov). Sin BYTERANGE. */
                char *u = strstr(l, "URI=\"");
                if (!u || strstr(l, "BYTERANGE")) { FATAL("Formato fMP4 no compatible (EXT-X-MAP con BYTERANGE)."); free(m.d); goto fail; }
                u += 5; u[strcspn(u, "\"")] = 0;
                map_now = u;
            } else if (l[0] && l[0] != '#' && nseg < 2048) { seg_map[nseg] = map_now; seg_uri[nseg++] = l; }
        }
        if (td < 1) td = 1;
        if (td > 20) td = 20;
        fails = 0;
        if (!nseg) { free(m.d); usleep(1000 * 1000); continue; }
        /* rc6: lista congelada (el CDN sigue dando la misma lista) -> volver a la maestra */
        {
            u64 now = gettime();
            u32 lim = td * 3000 < 15000 ? 15000 : td * 3000;
            if (seq0 + nseg > last_end) { last_end = seq0 + nseg; end_t = now; }
            else if (!endlist && end_t && ticks_to_millisecs(now - end_t) > lim) {
                free(m.d);
                dlog("hls: la lista no avanza hace %u ms (seq %lld)", (unsigned)ticks_to_millisecs(now - end_t), last_end);
                if (!remaster(url, sizeof(url), "lista congelada")) { if (fatal_err || abort_req) goto fail; usleep(1000 * 1000); }
                next_seq = -1; last_end = -1; end_t = gettime();
                continue;
            }
        }
        if (next_seq >= 0 && next_seq < seq0) {
            u64 now = gettime();
            dlog("hls: atrasado %lld segmentos, salto al vivo", seq0 - next_seq);
            if (behind_t && ticks_to_millisecs(now - behind_t) < 60000) behind++; else behind = 1;
            behind_t = now;
            if (behind >= 2 && fallback_url[0]) slow = 3;   /* la red no da: bajar a 160p */
        }
        if (next_seq < 0 || next_seq < seq0 || next_seq > seq0 + nseg + 2) {
            int back = td >= 8 ? 2 : 3;
            next_seq = seq0 + (nseg > back ? nseg - back : 0);
        }
        int got = 0, need_master = 0;
        for (i = (int)(next_seq - seq0); i < nseg && !abort_req; i++) {
            if (slow >= 3 && fallback_url[0]) break;
            while (!abort_req && (sq_count >= 3 || sq_bytes > 8 * 1024 * 1024)) usleep(20000);
            if (abort_req) break;
            if (seg_map[i]) {
                resolve(base, seg_map[i], mapu, sizeof(mapu));
                if (strcmp(mapu, cur_map)) {
                    mem_t im;
                    if (!fetch(mapu, &im, 1024 * 1024) && (abort_req || !fetch(mapu, &im, 1024 * 1024))) { free(m.d); goto fail; }
                    snprintf(cur_map, sizeof(cur_map), "%s", mapu);
                    if (last_init && last_init_n == (int)im.n && !memcmp(last_init, im.d, im.n)) {
                        free(im.d);                          /* mismo init con otra URL (token renovado): seguir igual */
                    } else {
                        if (last_init) sq_push(NULL, 0);     /* init distinto (anuncio o cambio de calidad): reabrir demuxer */
                        free(last_init);
                        last_init = malloc(im.n); last_init_n = (int)im.n;
                        if (last_init) memcpy(last_init, im.d, im.n);
                        sq_push(im.d, (int)im.n);
                    }
                }
            }
            resolve(base, seg_uri[i], segu, sizeof(segu));
            mem_t s;
            u64 ts0 = gettime();
            if (fetch_seg(segu, &s) || (!abort_req && last_code != 403 && last_code != 401 && fetch_seg(segu, &s))) {
                /* red lenta: si 3 segmentos seguidos tardan mas del 85% de su duracion, bajar a 160p */
                u32 ms = ticks_to_millisecs(gettime() - ts0);
                last_seg_ms = ms;
                if (fallback_url[0] && ms > (u32)(td * 850)) slow++; else slow = 0;
                if (!seg_map[i] && s.n >= 188 && s.d[0] != 0x47) {
                    /* buscar el primer byte de sincronia TS */
                    size_t o = 0;
                    while (o + 188 < s.n && !(s.d[o] == 0x47 && s.d[o + 188] == 0x47)) o++;
                    if (o + 188 >= s.n) { free(s.d); snprintf(perr, sizeof(perr), "El segmento no es MPEG-TS."); free(m.d); goto fail; }
                    memmove(s.d, s.d + o, s.n - o); s.n -= o;
                }
                sq_push(s.d, (int)s.n);
                got++; n_seg_ok++; seg_streak = 0;
                last_seg_t = gettime(); cur_seq = seq0 + i;
            } else if (!abort_req) {
                n_seg_fail++;
                if (++seg_streak >= 3) { need_master = 1; next_seq = seq0 + i + 1; break; }
            }
            next_seq = seq0 + i + 1;
            /* CPU: si en 8 s se descarta mas del 40% de los cuadros (la Wii no llega
               a decodificar 360p a tiempo) o se muestran menos de 4 cuadros/s, se baja a 160p */
            if (fallback_url[0] && has_video && pstate == TP_PLAYING) {
                u64 now = gettime();
                if (ticks_to_millisecs(now - last_pic_call) > 300) cpu_t = 0;   /* menu abierto: no medir */
                else if (!cpu_t) { cpu_t = now; cpu_d0 = ndropped; cpu_f0 = nframes; cpu_s0 = nshown; }
                else if (ticks_to_millisecs(now - cpu_t) >= 8000) {   /* rc6: ventana 8 s (antes 12) */
                    int dd = ndropped - cpu_d0, ff = nframes - cpu_f0, ss = nshown - cpu_s0;
                    if (dd > 70 && dd * 100 > ff * 40) slow = 3;
                    if (ss < 32 && dd > 20) slow = 3;
                    if (slow >= 3) dlog("cpu: 8 s con %d decodificados, %d descartados, %d mostrados -> 160p", ff, dd, ss);
                    cpu_t = now; cpu_d0 = ndropped; cpu_f0 = nframes; cpu_s0 = nshown;
                }
            }
            if (slow >= 3 && fallback_url[0]) break;
            /* la URL del segmento cambio la URL efectiva: recuperar la de la lista */
        }
        free(m.d);
        if (slow >= 3 && fallback_url[0]) {
            dlog("hls: red/CPU lenta (ultimo segmento %u ms, td %d s) -> %dp", (unsigned)last_seg_ms, td, fallback_h);
            snprintf(url, sizeof(url), "%s", fallback_url);
            snprintf(variant_info, sizeof(variant_info), "%dp (auto, liviano)", fallback_h);
            cur_h = fallback_h; fallback_url[0] = 0; slow = 0; n_downgrade++;
            last_end = -1; end_t = 0;
            continue;
        }
        if (need_master) {
            dlog("hls: 3 segmentos seguidos fallaron (http %ld)", last_code);
            seg_streak = 0;
            if (!remaster(url, sizeof(url), "segmentos fallan")) { if (fatal_err || abort_req) goto fail; usleep(1000 * 1000); }
            next_seq = -1; last_end = -1; end_t = 0;
            continue;
        }
        if (endlist && next_seq >= seq0 + nseg) { net_eof = 1; dlog("hls: fin de la lista (ENDLIST)"); break; }
        if (!got) {
            int k, wait = td * 500;        /* media duracion de segmento */
            if (wait < 1000) wait = 1000;
            for (k = 0; k < wait / 50 && !abort_req; k++) usleep(50000);
        }
    }
    net_eof = 1;
    free(last_init);
    curl_easy_cleanup(nc); nc = NULL;
    return NULL;
fail:
    if (!abort_req) { pstate = TP_ERROR; if (!perr[0]) snprintf(perr, sizeof(perr), "No se pudo abrir la transmision."); dlog("hls: error: %s%s", perr, fatal_err ? " (definitivo)" : ""); }
    net_eof = 1;
    free(last_init);
    curl_easy_cleanup(nc); nc = NULL;
    return NULL;
}
static void *net_thread(void *arg) { (void)arg; net_thread_body(); net_done = 1; return NULL; }

/* ================================================================== demux + audio */
static int io_read(void *o, uint8_t *b, int sz) {
    (void)o;
    /* despues de la marca de init nuevo, EOF "pegajoso": el demuxer viejo no debe
       comerse el init ni los fragmentos que siguen (son para el demuxer nuevo) */
    if (reopen_req) return AVERROR_EOF;
    while (!abort_req) {
        if (rd_seg && rd_pos < rd_seg->n) {
            int k = rd_seg->n - rd_pos;
            if (k > sz) k = sz;
            memcpy(b, rd_seg->d + rd_pos, k);
            rd_pos += k;
            return k;
        }
        if (rd_seg) { free(rd_seg->d); free(rd_seg); rd_seg = NULL; }
        rd_seg = sq_pop(); rd_pos = 0;
        if (rd_seg && rd_seg->n == 0) { free(rd_seg); rd_seg = NULL; reopen_req = 1; return AVERROR_EOF; }
        if (rd_seg) continue;
        if (net_eof) return AVERROR_EOF;
        usleep(5000);
    }
    return AVERROR_EXIT;
}
static int io_interrupt(void *o) { (void)o; return abort_req; }

static void vq_push(AVPacket *pk) {
    int key = (pk->flags & AV_PKT_FLAG_KEY) != 0;
    if (vq_needkey && !key) { ndropped++; return; }
    if (vq_count() >= VQ_MAX - 1 || vq_bytes > 5 * 1024 * 1024) { vq_needkey = 1; ndropped++; return; }
    vq_needkey = 0;
    AVPacket *c = av_packet_clone(pk);
    if (!c) return;
    c->pos = vgen;                       /* generacion de vpar con la que hay que decodificarlo */
    vq[vq_w] = c;
    LWP_MutexLock(mtx); vq_bytes += c->size; LWP_MutexUnlock(mtx);
    vq_w = (vq_w + 1) % VQ_MAX;
}

/* espera el primer segmento y mira si es MPEG-TS (0x47) o fMP4 (ftyp/moov/styp) */
static const char *detect_format(void) {
    while (!abort_req) {
        seg_t *h;
        LWP_MutexLock(mtx);
        h = (rd_seg && rd_pos < rd_seg->n) ? rd_seg : sq_head;
        int n = h ? h->n : -1;
        unsigned char b[8] = { 0 };
        if (h && n >= 8) memcpy(b, h->d + (h == rd_seg ? rd_pos : 0), 8);
        LWP_MutexUnlock(mtx);
        if (n >= 8) {
            if (!memcmp(b + 4, "ftyp", 4) || !memcmp(b + 4, "moov", 4) || !memcmp(b + 4, "styp", 4)) return "mov";
            return "mpegts";
        }
        if (n == 0) { seg_t *x = sq_pop(); if (x) { free(x->d); free(x); } continue; }   /* marca suelta */
        if (net_eof) return NULL;
        usleep(5000);
    }
    return NULL;
}

static void *dmx_thread_body(void *arg) {
    AVFormatContext *fc = NULL;
    AVIOContext *io = NULL;
    AVCodecContext *ac = NULL;
    SwrContext *swr = NULL;
    AVPacket *pk = NULL; AVFrame *fr = NULL;
    int ai = -1, vi = -1, opened = 0;
    static short outbuf[8192 * 2];
    double a_next = -1;
    u64 t_start = gettime();
    (void)arg;

    pk = av_packet_alloc(); fr = av_frame_alloc();
    while (!abort_req) {
        /* (re)abrir el demuxer: al empezar y cada vez que llega un init fMP4 nuevo */
        const char *fmt = detect_format();
        if (!fmt) break;
        reopen_req = 0;
        unsigned char *ib = av_malloc(32768);
        io = avio_alloc_context(ib, 32768, 0, NULL, io_read, NULL, NULL);
        fc = avformat_alloc_context();
        if (!io || !fc || !pk || !fr) { snprintf(perr, sizeof(perr), "Sin memoria."); pstate = TP_ERROR; goto out; }
        fc->pb = io;
        fc->interrupt_callback.callback = io_interrupt;
        fc->probesize = 512 * 1024;
        fc->max_analyze_duration = 2 * AV_TIME_BASE;
        if (avformat_open_input(&fc, "", av_find_input_format(fmt), NULL) < 0) {
            if (!abort_req && pstate != TP_ERROR) { snprintf(perr, sizeof(perr), "No se pudo leer el video (%s).", fmt[0] == 'm' && fmt[1] == 'o' ? "fMP4" : "MPEG-TS"); pstate = TP_ERROR; }
            goto out;
        }
        ai = -1; vi = -1;
        if (opened) {   /* reapertura: el audio puede cambiar de parametros */
            avcodec_free_context(&ac); swr_free(&swr);
        }
        opened++;
        if (pstate != TP_ERROR && pstate != TP_PLAYING) { snprintf(pstatus, sizeof(pstatus), "Llenando buffer..."); pstate = TP_BUFFERING; }
        while (!abort_req) {
            int r = av_read_frame(fc, pk);
            if (r < 0) break;
            AVStream *st = (pk->stream_index < (int)fc->nb_streams) ? fc->streams[pk->stream_index] : NULL;
            if (!st) { av_packet_unref(pk); continue; }
            int ct = st->codecpar->codec_type;
            if (ai < 0 && ct == AVMEDIA_TYPE_AUDIO && (st->codecpar->codec_id == AV_CODEC_ID_AAC || st->codecpar->codec_id == AV_CODEC_ID_MP3 || st->codecpar->codec_id == AV_CODEC_ID_AAC_LATM)) {
                const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
                ac = c ? avcodec_alloc_context3(c) : NULL;
                if (ac && avcodec_parameters_to_context(ac, st->codecpar) >= 0 && avcodec_open2(ac, c, NULL) >= 0) { ai = pk->stream_index; has_audio = 1; }
                else avcodec_free_context(&ac);
            }
            if (vi < 0 && ct == AVMEDIA_TYPE_VIDEO && st->codecpar->codec_id == AV_CODEC_ID_H264 && cur_vmode != VM_AUDIO) {
                AVCodecParameters *np = avcodec_parameters_alloc();
                avcodec_parameters_copy(np, st->codecpar);
                LWP_MutexLock(mtx);
                AVCodecParameters *old = vpar;
                vpar = np; vtb = st->time_base; vgen++;
                LWP_MutexUnlock(mtx);
                if (old) avcodec_parameters_free(&old);
                vi = pk->stream_index; has_video = 1;
            }
            if (pk->stream_index == ai) {
                if (avcodec_send_packet(ac, pk) >= 0) {
                    while (avcodec_receive_frame(ac, fr) == 0) {
                        double pts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp * av_q2d(st->time_base) : a_next;
                        if (!swr) {
                            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                            if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, 48000, &fr->ch_layout, fr->format, fr->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0) {
                                snprintf(perr, sizeof(perr), "No se pudo convertir el audio."); swr_free(&swr); break;
                            }
                        }
                        uint8_t *o[1] = { (uint8_t *)outbuf };
                        int n = swr_convert(swr, o, 8192, (const uint8_t **)fr->extended_data, fr->nb_samples);
                        if (n > 0) {
                            push_pcm(outbuf, n, pts);
                            a_next = pts + n / 48000.0;
                        }
                    }
                }
            } else if (pk->stream_index == vi) {
                vq_push(pk);
            } else if (ai < 0 && vi >= 0 && ticks_to_millisecs(gettime() - t_start) > 4000 && pstate == TP_BUFFERING) {
                pstate = TP_PLAYING;   /* sin audio */
            }
            if (!has_audio && has_video && pstate == TP_BUFFERING && ticks_to_millisecs(gettime() - t_start) > 4000) pstate = TP_PLAYING;
            av_packet_unref(pk);
        }
        avformat_close_input(&fc);
        if (io) { av_freep(&io->buffer); avio_context_free(&io); }
        if (!reopen_req || abort_req) break;
    }
    if (!abort_req && pstate != TP_ERROR) {
        if (!perr[0]) snprintf(perr, sizeof(perr), "La transmision termino o se corto.");
        dlog("demux: fin: %s", perr);
        pstate = TP_ERROR;
    }
out:
    av_frame_free(&fr); av_packet_free(&pk);
    avcodec_free_context(&ac);
    swr_free(&swr);
    if (fc) avformat_close_input(&fc);
    if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    return NULL;
}
static void *dmx_thread(void *arg) { dmx_thread_body(arg); dmx_done = 1; return NULL; }

/* ================================================================== video */
static void store_frame(AVFrame *f, double pts) {
    int k = -1, i, j;
    u64 w0 = gettime();
    while (!abort_req) {
        for (i = 0; i < NPIC; i++) if (pic_state[i] == P_FREE) { k = i; break; }
        if (k >= 0) break;
        /* rc6: si nadie consume cuadros en 400 ms (reloj de audio parado o menu abierto),
           descartar el mas viejo en vez de bloquear el decodificador para siempre */
        if (ticks_to_millisecs(gettime() - w0) > 400) {
            int old = -1;
            LWP_MutexLock(mtx);
            for (i = 0; i < NPIC; i++) if (pic_state[i] == P_READY && (old < 0 || pics[i].pts < pics[old].pts)) old = i;
            if (old >= 0) { pic_state[old] = P_WRITING; k = old; }
            LWP_MutexUnlock(mtx);
            if (k >= 0) { ndropped++; break; }
            w0 = gettime();
        }
        usleep(3000);
    }
    if (k < 0) return;
    pic_state[k] = P_WRITING;
    tv_pic_t *p = &pics[k];
    int fw = f->width, fh = f->height, step = 1;
    while (fw / step > PIC_MAXW || fh / step > PIC_MAXH) step++;
    int dw = (fw / step) & ~1, dh = (fh / step) & ~1;
    p->aspect = (float)fw / fh;
    if (f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0)
        p->aspect *= (float)f->sample_aspect_ratio.num / f->sample_aspect_ratio.den;
    p->w = dw; p->h = dh; p->sw = fw; p->sh = fh; p->pts = pts;
    for (j = 0; j < dh; j++) {
        const u8 *s = f->data[0] + j * step * f->linesize[0];
        u8 *d = p->y + j * dw;
        if (step == 1) memcpy(d, s, dw);
        else for (i = 0; i < dw; i++) d[i] = s[i * step];
    }
    for (j = 0; j < dh / 2; j++) {
        const u8 *su = f->data[1] + j * step * f->linesize[1], *sv = f->data[2] + j * step * f->linesize[2];
        u8 *du = p->u + j * (dw / 2), *dv = p->v + j * (dw / 2);
        if (step == 1) { memcpy(du, su, dw / 2); memcpy(dv, sv, dw / 2); }
        else for (i = 0; i < dw / 2; i++) { du[i] = su[i * step]; dv[i] = sv[i * step]; }
    }
    pic_state[k] = P_READY;
}

static void *vid_thread_body(void *arg) {
    AVCodecContext *vc = NULL;
    AVFrame *fr = av_frame_alloc();
    u64 last_store = 0;
    long long dec_gen = -1;
    (void)arg;
    while (!abort_req) {
        if (vq_r == vq_w) { usleep(4000); continue; }
        AVPacket *pk = vq[vq_r];
        vq[vq_r] = NULL;
        LWP_MutexLock(mtx); vq_bytes -= pk->size; LWP_MutexUnlock(mtx);
        vq_r = (vq_r + 1) % VQ_MAX;
        if (vc && pk->pos != dec_gen) avcodec_free_context(&vc);   /* init fMP4 nuevo: otro SPS/PPS */
        if (!vc) {
            const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_H264);
            vc = avcodec_alloc_context3(c);
            LWP_MutexLock(mtx);
            if (vpar) avcodec_parameters_to_context(vc, vpar);
            dec_gen = vgen;
            LWP_MutexUnlock(mtx);
            vc->thread_count = 1;
            vc->flags2 |= AV_CODEC_FLAG2_FAST;
            if (avcodec_open2(vc, c, NULL) < 0) { avcodec_free_context(&vc); av_packet_free(&pk); has_video = 0; dlog("video: no abre el decodificador H.264"); break; }
        }
        int valid = 0;
        double clock = tp_clock(&valid);
        double ppts = pk->pts != AV_NOPTS_VALUE ? pk->pts * av_q2d(vtb) : (pk->dts != AV_NOPTS_VALUE ? pk->dts * av_q2d(vtb) : -1);
        double lag = (valid && ppts >= 0) ? clock - ppts : 0;
        int key = (pk->flags & AV_PKT_FLAG_KEY) != 0;
        if (lag > 30 || lag < -30) lag = 0;      /* discontinuidad: no descartar */
        if (vc->width * vc->height > 640 * 368) vc->skip_loop_filter = AVDISCARD_ALL;
        if (lag > 1.2 && !key) { av_packet_free(&pk); ndropped++; continue; }
        vc->skip_frame = lag > 0.3 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
        u64 td0 = gettime();
        int sent = avcodec_send_packet(vc, pk) >= 0;
        tp_dec_us += ticks_to_microsecs(gettime() - td0);
        if (sent) {
            while (1) {
                td0 = gettime();
                int rr = avcodec_receive_frame(vc, fr);
                tp_dec_us += ticks_to_microsecs(gettime() - td0);
                if (rr != 0) break;
                double fpts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp * av_q2d(vtb) : ppts;
                nframes++;
                stream_w = fr->width; stream_h = fr->height;
                clock = tp_clock(&valid);
                if (valid && fpts < clock - 0.25 && fpts > clock - 30 && ticks_to_millisecs(gettime() - last_store) < 700) { ndropped++; continue; }
                if (fr->format != AV_PIX_FMT_YUV420P && fr->format != AV_PIX_FMT_YUVJ420P) continue;
                store_frame(fr, fpts);
                last_store = gettime();
            }
        }
        av_packet_free(&pk);
    }
    avcodec_free_context(&vc);
    av_frame_free(&fr);
    return NULL;
}
static void *vid_thread(void *arg) { vid_thread_body(arg); vid_done = 1; return NULL; }

/* ================================================================== hilo principal */
static void wd_tick(void);
const tv_pic_t *tp_picture(void) {
    int i, valid = 0, best = -1, minp = -1;
    double clock = tp_clock(&valid);
    u64 now0 = gettime();
    if (last_pic_call && ticks_to_millisecs(now0 - last_pic_call) > 1000) {
        /* volvemos de un menu: no contar ese tiempo como video congelado */
        last_show_t = now0; last_clk_move = now0;
    }
    last_pic_call = now0;
    if (pstate == TP_IDLE) return NULL;
    wd_tick();
    LWP_MutexLock(mtx);
    for (i = 0; i < NPIC; i++) {
        if (pic_state[i] != P_READY) continue;
        if (minp < 0 || pics[i].pts < pics[minp].pts) minp = i;
        if (valid && pics[i].pts <= clock + 0.02 && (best < 0 || pics[i].pts > pics[best].pts)) best = i;
    }
    if (best < 0 && minp >= 0) {
        if (cur_pic < 0) best = minp;                                    /* primer cuadro: mostrarlo ya */
        else if (valid && pics[minp].pts > clock + 2.5) best = minp;     /* salto de tiempo */
        else if (!has_audio && !anchor_ok) best = minp;
        /* rc6: el reloj del audio no avanza (DSP sin datos): seguir con reloj de pared */
        else if (has_audio && (starved || !valid) && ticks_to_millisecs(now0 - last_show_t) > 700) best = minp;
    }
    if (best >= 0) {
        if (!has_audio && (!anchor_ok || fabs(pics[best].pts - clock) > 2.5)) { anchor_pts = pics[best].pts; anchor_t = gettime(); anchor_ok = 1; }
        for (i = 0; i < NPIC; i++)
            if (i != best && pic_state[i] == P_READY && pics[i].pts <= pics[best].pts) pic_state[i] = P_FREE;
        if (cur_pic >= 0 && cur_pic != best) pic_state[cur_pic] = P_FREE;
        pic_state[best] = P_CUR;
        cur_pic = best;
        nshown++; fps_cnt++;
        last_show_t = now0;
    }
    LWP_MutexUnlock(mtx);
    u64 now = gettime();
    if (!fps_t) fps_t = now;
    if (ticks_to_millisecs(now - fps_t) >= 1000) { fps_val = fps_cnt * 1000.0 / ticks_to_millisecs(now - fps_t); fps_cnt = 0; fps_t = now; }
    return cur_pic >= 0 ? &pics[cur_pic] : NULL;
}

/* espera los hilos ya terminados y deja colas/decodificadores limpios (no toca la URL ni los limites) */
static void pipeline_reset(void) {
    int i;
    if (th_vid != LWP_THREAD_NULL) { LWP_JoinThread(th_vid, NULL); th_vid = LWP_THREAD_NULL; }
    if (th_dmx != LWP_THREAD_NULL) { LWP_JoinThread(th_dmx, NULL); th_dmx = LWP_THREAD_NULL; }
    if (th_net != LWP_THREAD_NULL) { LWP_JoinThread(th_net, NULL); th_net = LWP_THREAD_NULL; }
    voice_reset();
    sq_clear();
    while (vq_r != vq_w) { av_packet_free(&vq[vq_r]); vq_r = (vq_r + 1) % VQ_MAX; }
    vq_r = vq_w = 0; vq_needkey = 0; vq_bytes = 0; reopen_req = 0;
    if (vpar) avcodec_parameters_free(&vpar);
    for (i = 0; i < NPIC; i++) pic_state[i] = P_FREE;
    cur_pic = -1; anchor_ok = 0;
    net_eof = 0;
    has_audio = has_video = 0; stream_w = stream_h = 0;
    fps_val = 0; fps_cnt = 0; fps_t = 0;
}

static void mem_line(char *o, int sz) {
    struct mallinfo mi = mallinfo();
    u32 a1 = (u32)SYS_GetArena1Hi() - (u32)SYS_GetArena1Lo(), a2 = (u32)SYS_GetArena2Hi() - (u32)SYS_GetArena2Lo();
    snprintf(o, sz, "heap usado %d KB libre-en-heap %d KB | sin reservar MEM1 %lu KB MEM2 %lu KB",
             mi.uordblks >> 10, mi.fordblks >> 10, (unsigned long)(a1 >> 10), (unsigned long)(a2 >> 10));
}

static void wd_restart(const char *why) {
    char ml[160];
    n_restart++; restart_streak++;
    mem_line(ml, sizeof(ml));
    dlog("VIGILANTE: reinicio #%d (seguidos %d): %s | seg ok %d fallos %d http %ld | %s",
         n_restart, restart_streak, why, n_seg_ok, n_seg_fail, last_code, ml);
    if (restart_streak >= 2 && lim_h && !lowq && cur_vmode != VM_AUDIO) { lowq = 1; dlog("VIGILANTE: sigo en 160p (modo liviano)"); }
    if (restart_streak > 1) snprintf(pstatus, sizeof(pstatus), "Reconectando... (%s, intento %d)", why, restart_streak);
    else snprintf(pstatus, sizeof(pstatus), "Reconectando... (%s)", why);
    abort_req = 1;
    wd_phase = 1; wd_t = gettime();
    pstate = TP_CONNECTING;
}

static void wd_tick(void) {
    u64 now = gettime();
    if (wd_phase == 1) {
        /* esperar que los 3 hilos terminen (abort_req) sin congelar la pantalla */
        u32 backoff = restart_streak <= 1 ? 500 : (restart_streak >= 8 ? 15000 : 2000 * (restart_streak - 1));
        if (!net_done && ticks_to_millisecs(now - wd_t) > 4000) {   /* hilo de red trabado en un recv() */
            kill_socks("el hilo de red no responde");
            wd_t = now - (u64)secs_to_ticks(2);   /* reintentar el corte cada ~2 s */
            return;
        }
        if (!(net_done && dmx_done && vid_done) || ticks_to_millisecs(now - wd_t) < backoff) return;
        pipeline_reset();
        abort_req = 0; perr[0] = 0;
        pstate = TP_CONNECTING;
        play_t0 = last_show_t = last_clk_move = last_seg_t = restart_ok_t = gettime(); err_t = 0;
        wd_phase = 0;
        if (!start_threads()) { snprintf(perr, sizeof(perr), "No se pudieron crear los hilos."); pstate = TP_ERROR; fatal_err = 1; }
        return;
    }
    if (ticks_to_millisecs(now - wd_log_t) >= 10000) {
        char ml[160];
        wd_log_t = now; mem_line(ml, sizeof(ml));
        dlog("estado %d %s | %s | %.1f fps mostrados %d decod %d desc %d | segs %d/%d fallos, ultimo %u ms seq %lld | colas seg %d (%d KB) vq %d (%d KB) audio %d blq%s | red %lld KB | renov %d baja %d reinicios %d | %s",
             pstate, pstate == TP_ERROR ? perr : "", variant_info, fps_val, nshown, nframes, ndropped,
             n_seg_ok, n_seg_fail, (unsigned)last_seg_ms, cur_seq, sq_count, sq_bytes >> 10, vq_count(), vq_bytes >> 10,
             full_blocks(), starved ? " SIN AUDIO" : "", net_bytes >> 10, n_refresh, n_downgrade, n_restart, ml);
        { static int said = 0; if (n_sock_kill != said) { said = n_sock_kill; dlog("sockets cortados por el vigilante: %d", n_sock_kill); } }
    }
    {   /* pedido HTTP sin recibir nada en 12 s, o que supera su timeout: socket colgado en IOS
           (en Dolphin se vio que curl no logra aplicar sus propios timeouts en ese caso) */
        u64 x0 = xfer_t0, rx = last_rx_t;
        if (x0 && now > x0) {
            u64 ref = rx > x0 ? rx : x0;
            if ((now > ref && ticks_to_millisecs(now - ref) > 12000) || ticks_to_millisecs(now - x0) > (u32)(xfer_tmo + 6) * 1000) {
                kill_socks("pedido colgado, sin datos");
                xfer_t0 = last_rx_t = now;   /* no repetir enseguida */
            }
        }
    }
    if (pstate == TP_ERROR) {
        if (fatal_err) return;
        if (!err_t) { err_t = now; dlog("VIGILANTE: error '%s', reintento en 3 s", perr); return; }
        if (ticks_to_millisecs(now - err_t) >= 3000) wd_restart("error de red");
        return;
    }
    /* rc6: no se rinde (la red de la Wii puede volver en cualquier momento); a partir del
       8vo intento seguido reintenta cada ~15 s */
    u32 since_start = ticks_to_millisecs(now - play_t0);
    if (pstate == TP_CONNECTING || pstate == TP_BUFFERING) {
        if (since_start > 35000) wd_restart("no arranca");
        else if (since_start > 20000 && ticks_to_millisecs(now - last_seg_t) > 20000) wd_restart("no llegan segmentos");
        return;
    }
    if (pstate != TP_PLAYING) return;
    /* el reloj del audio avanza? */
    if (has_audio) {
        int v = 0; double c = tp_clock(&v);
        if (v && fabs(c - last_clk_val) > 0.05) { last_clk_val = c; last_clk_move = now; }
    } else last_clk_move = now;
    u32 no_seg = ticks_to_millisecs(now - last_seg_t);
    u32 no_pic = ticks_to_millisecs(now - last_show_t);
    u32 no_clk = ticks_to_millisecs(now - last_clk_move);
    if (!net_eof && no_seg > 20000) { wd_restart("no llegan segmentos"); return; }
    if (has_video && cur_vmode != VM_AUDIO && no_pic > 10000) { wd_restart("video congelado"); return; }
    if (has_audio && no_clk > 10000) { wd_restart("audio detenido"); return; }
    if (restart_streak && (no_pic < 2000 || !has_video) && ticks_to_millisecs(now - restart_ok_t) > 30000) {
        dlog("VIGILANTE: 30 s estable despues del reinicio");
        restart_streak = 0;
    }
}

static int start_threads(void) {
    net_done = dmx_done = vid_done = 0;
    if (LWP_CreateThread(&th_net, net_thread, NULL, NULL, 128 * 1024, 72) < 0) { th_net = LWP_THREAD_NULL; net_done = dmx_done = vid_done = 1; return 0; }
    if (LWP_CreateThread(&th_dmx, dmx_thread, NULL, NULL, 256 * 1024, 70) < 0) { th_dmx = LWP_THREAD_NULL; dmx_done = vid_done = 1; abort_req = 1; return 0; }
    /* rc6: 52 (antes 40): por encima del chat (48), los avisos (50) y las miniaturas (30),
       por debajo del hilo principal (64), el demux (70) y la red (72) */
    if (LWP_CreateThread(&th_vid, vid_thread, NULL, NULL, 256 * 1024, 52) < 0) { th_vid = LWP_THREAD_NULL; vid_done = 1; abort_req = 1; return 0; }
    return 1;
}

void tp_stop(void) {
    if (pstate != TP_IDLE || th_net != LWP_THREAD_NULL)
        dlog("player: stop (mostrados %d, reinicios %d, renov %d)", nshown, n_restart, n_refresh);
    abort_req = 1;
    pipeline_reset();
    pstate = TP_IDLE; abort_req = 0; wd_phase = 0;
    nframes = ndropped = nshown = 0;
    net_bytes = 0; variant_info[0] = 0; pstatus[0] = 0;
}

void tp_set_limits(int max_h, const char *api) {
    lim_h = max_h;
    snprintf(refresh_api, sizeof(refresh_api), "%s", api ? api : "");
}

int tp_play(const char *url, int vmode) {
    tp_stop();
    perr[0] = 0;
    cur_vmode = vmode;
    n_refresh = 0; n_downgrade = 0;
    n_restart = restart_streak = 0; lowq = 0; n_sock_kill = 0; fatal_err = 0; refresh_hist_n = 0;
    n_seg_ok = n_seg_fail = 0; last_seg_ms = 0; cur_seq = -1; err_t = 0; last_clk_val = -1;
    snprintf(purl, sizeof(purl), "%s", url);
#ifdef STRESSTEST
    {
        char b[200];
        if (sh_cfg_read("stress-url.txt", b, sizeof(b))) {
            snprintf(purl, sizeof(purl), "%s/master.m3u8", b);
            snprintf(refresh_api, sizeof(refresh_api), "%s/api/channel", b);
            dlog("STRESSTEST: proxy local %s", b);
        }
    }
#endif
    {
        char h[96]; dlog_host(purl, h, sizeof(h));
        dlog("player: play %s (limite %dp, renovable %d, modo %d)", h, lim_h, refresh_api[0] != 0, vmode);
    }
    pstate = TP_CONNECTING;
    play_t0 = last_show_t = last_clk_move = last_seg_t = restart_ok_t = wd_log_t = gettime();
    if (!sh_net_ready()) { snprintf(perr, sizeof(perr), "La red no esta disponible."); pstate = TP_ERROR; return 0; }
    if (!start_threads()) {
        snprintf(perr, sizeof(perr), "No se pudieron crear los hilos."); pstate = TP_ERROR; fatal_err = 1; return 0;
    }
    return 1;
}

int tp_state(void) { return pstate; }
const char *tp_error(void) { return perr; }
const char *tp_status(void) {
    static char s[96];
    switch (pstate) {
    case TP_IDLE: return "Detenido";
    case TP_CONNECTING: return pstatus[0] ? pstatus : "Conectando...";
    case TP_BUFFERING: {
        int fb = full_blocks();
        snprintf(s, sizeof(s), "Cargando %d%%", has_audio ? (fb * 100 / START_BLK > 99 ? 99 : fb * 100 / START_BLK) : 50);
        return s; }
    case TP_PLAYING:
        if (has_video && cur_vmode != VM_AUDIO && last_show_t && ticks_to_millisecs(gettime() - last_show_t) > 3000) return "Esperando video...";
        return "En vivo";
    case TP_ERROR: return "Error";
    }
    return "";
}
void tp_set_volume(int v) {
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    volume = v;
    if (voice_on) ASND_ChangeVolumeVoice(0, v, v);
}
int tp_volume(void) { return volume; }
int tp_has_video(void) { return has_video; }
int tp_has_audio(void) { return has_audio; }
int tp_vmode(void) { return cur_vmode; }
const char *tp_mode_text(void) {
    if (cur_vmode == VM_AUDIO) return "Solo audio";
    if (pstate == TP_PLAYING && !has_video) return "Solo audio (sin video)";
    if (lim_h) return cur_h && cur_h < 300 ? "Video 160p (liviano)" : "Video 360p";
    if (stream_w * stream_h > 640 * 368 || !sd_stream) return "Video HD reducido";
    return "Video completo";
}
float tp_audio_level(void) { return voice_on ? alevel : 0; }
double tp_fps(void) { return fps_val; }
int tp_frames(void) { return nframes; }
int tp_dropped(void) { return ndropped; }
long long tp_bytes(void) { return net_bytes; }
void tp_stats(char *o, int sz) {
    snprintf(o, sz, "%s%s%dx%d  %.0f fps  buf %.1fs  red %lld KB  desc %d%s",
             variant_info, variant_info[0] ? " | " : "", stream_w, stream_h, fps_val,
             full_blocks() * BLKFR / 48000.0, net_bytes >> 10, ndropped, n_refresh ? "  token renovado" : "");
    if (n_restart) { int l = strlen(o); snprintf(o + l, sz > l ? sz - l : 0, "  reconexiones %d", n_restart); }
}
