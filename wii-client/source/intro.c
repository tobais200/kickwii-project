/*
 * intro.c - pantalla de arranque con la imagen de carga elegida por el usuario (fondo negro y
 * wordmark "KICK" pixelado verde, channel/art/source/splash-usuario.jpg -> data/intro_logo.kwa)
 * con "Clair de lune" (Debussy) de fondo.
 *
 * Musica: "Clair de lune" (C. Debussy), piano Laurens Goedhart (2011), dominio publico
 * via Wikimedia Commons (Public Domain Mark):
 * https://commons.wikimedia.org/wiki/File:Clair_de_lune_(Claude_Debussy)_Suite_bergamasque.ogg
 * Recorte de 45 s (desde 0:01.9), mono 22,05 kHz, IMA-ADPCM 4:1 (data/intro_music.kwad, ~0,5 MB);
 * se decodifica a PCM en el heap al arrancar (el PCM embebido no entraba en MEM1 junto a las tablas de FFmpeg).
 *
 * Mientras se ve la intro, la red se inicializa en un hilo aparte.
 * A / 2 / +: saltar la intro (la musica sigue de fondo).  B: saltar y silenciar.
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <asndlib.h>
#include <wiiuse/wpad.h>
#include <malloc.h>
#include <string.h>
#include <math.h>
#include "gfx.h"
#include "tv.h"
#include "intro_music_kwad.h"
#include "intro_logo_kwa.h"

#define VOICE_INTRO 1
#define MIN_MS 6500          /* duracion minima de la intro si no se salta */
#define MAX_NET_MS 20000     /* despues de esto se sigue aunque la red no responda */

static lwp_t net_th = LWP_THREAD_NULL;
static volatile int net_done = 0, net_res = 0;
static int music_on = 0;
static u64 music_t0 = 0;
static int music_ms = 0;
static s16 *pcm = NULL;

static void *net_fn(void *a) { (void)a; net_res = sh_net_ready(); net_done = 1; return NULL; }

void intro_music_stop(void) {
    if (!music_on) return;
    music_on = 0;
    ASND_StopVoice(VOICE_INTRO);
    free(pcm); pcm = NULL;
}

/* la musica se entrega al DSP en bloques de 8 KB (doble buffer de ASND): pasar el
   buffer entero de ~2 MB en una sola voz desborda el DMA (Dolphin: "Oversized range") */
#define MCH 8192
static volatile u32 mpos = 0, mlen = 0;
static void music_cb(s32 v) {
    (void)v;
    if (!music_on || mpos >= mlen) return;
    u32 n = mlen - mpos; if (n > MCH) n = MCH;
    if (ASND_AddVoice(VOICE_INTRO, (void *)((u8 *)pcm + mpos), n) == SND_OK) mpos += n;
}

static const int ima_step[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
    66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767 };
static const int ima_idx[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/* KWAD: 'KWAD' + u32 frecuencia + u32 muestras + nibbles IMA-ADPCM (alto primero) */
static int music_decode(int *rate) {
    const u8 *d = intro_music_kwad;
    if (intro_music_kwad_size < 12 || memcmp(d, "KWAD", 4)) return 0;
    *rate = (d[4] << 24) | (d[5] << 16) | (d[6] << 8) | d[7];
    u32 n = (d[8] << 24) | (d[9] << 16) | (d[10] << 8) | d[11], i;
    if (n > (intro_music_kwad_size - 12) * 2) n = (intro_music_kwad_size - 12) * 2;
    n &= ~15u;                                   /* bytes multiplo de 32 */
    pcm = memalign(32, n * 2);
    if (!pcm) return 0;
    int pred = 0, idx = 0;
    for (i = 0; i < n; i++) {
        int c = (i & 1) ? (d[12 + i / 2] & 15) : (d[12 + i / 2] >> 4);
        int st = ima_step[idx], dq = st >> 3;
        if (c & 4) dq += st;
        if (c & 2) dq += st >> 1;
        if (c & 1) dq += st >> 2;
        pred += (c & 8) ? -dq : dq;
        if (pred > 32767) pred = 32767; else if (pred < -32768) pred = -32768;
        idx += ima_idx[c & 7]; if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
        pcm[i] = (s16)pred;
    }
    DCFlushRange(pcm, n * 2);
    return (int)(n * 2);
}

static void music_start(void) {
    int rate = 22050;
    int bytes = music_decode(&rate);
    if (bytes < MCH) return;
    mlen = bytes;
    music_ms = (int)((bytes / 2) * 1000LL / rate);
    ASND_Pause(0);
    mpos = MCH;
    music_on = 1;
    if (ASND_SetVoice(VOICE_INTRO, VOICE_MONO_16BIT, rate, 0, pcm, MCH, 200, 200, music_cb) == SND_OK)
        music_t0 = gettime();
    else music_on = 0;
}

/* llamar de vez en cuando: libera la voz cuando la musica termino */
void intro_music_tick(void) {
    if (music_on && ticks_to_millisecs(gettime() - music_t0) > (u32)music_ms + 500) intro_music_stop();
}

static float clampf(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float ease_out(float t) { t = clampf(t); return 1 - (1 - t) * (1 - t) * (1 - t); }
static float ease_back(float t) { t = clampf(t); float c = 1.6f; t -= 1; return 1 + (c + 1) * t * t * t + c * t * t; }

/* marca RGBA escalada (vecino mas cercano) con alfa global y brillo diagonal que la recorre */
static void logo_blit(int cx, int cy, float sc, int alpha, int shine_x) {
    const u8 *p = intro_logo_kwa;
    if (memcmp(p, "KWA1", 4)) return;
    int lw = (p[4] << 8) | p[5], lh = (p[6] << 8) | p[7];
    const u8 *px = p + 8;
    int w = (int)(lw * sc), h = (int)(lh * sc), x, y;
    if (w < 2 || h < 2) return;
    int x0 = cx - w / 2, y0 = cy - h / 2;
    for (y = 0; y < h; y++) {
        int yy = y0 + y; if (yy < 0 || yy >= SCR_H) continue;
        int sy = y * lh / h;
        u32 *d = canvas + yy * SCR_W;
        for (x = 0; x < w; x++) {
            int xx = x0 + x; if (xx < 0 || xx >= SCR_W) continue;
            const u8 *s = px + (sy * lw + x * lw / w) * 4;
            int a = s[3] * alpha / 255;
            if (a <= 0) continue;
            int r = s[0], g = s[1], b = s[2];
            int sd = xx - shine_x + (yy - cy) / 2;          /* banda diagonal */
            if (sd > -18 && sd < 18) { int k = 150 - (sd < 0 ? -sd : sd) * 8; if (k > 0) { r += (255 - r) * k / 255; g += (255 - g) * k / 255; b += (255 - b) * k / 255; } }
            u32 o = d[xx];
            int orr = (o >> 16) & 255, og = (o >> 8) & 255, ob = o & 255;
            orr += (r - orr) * a / 255; og += (g - og) * a / 255; ob += (b - ob) * a / 255;
            d[xx] = (orr << 16) | (og << 8) | ob;
        }
    }
}

static void fade_black(int a) { if (a > 0) gfx_rect_a(0, 0, SCR_W, SCR_H, 0x000000, a > 255 ? 255 : a); }

static void draw(int ms, int out_ms) {
    /* imagen de carga elegida por el usuario: fondo negro + wordmark "KICK" pixelado verde */
    gfx_clear(0x000000);
    float t = (ms - 250) / 900.0f;
    if (t > 0) {
        float sc = 0.85f + 0.15f * ease_back(t);
        int sx = -200;
        if (ms > 1500) sx = -120 + ((ms - 1500) % 3800) * 900 / 1400;
        logo_blit(SCR_W/2, 220, sc, (int)(255 * ease_out(t * 1.4f)), sx);
    }
    /* estado de carga: tres puntos verdes */
    if (ms > 1900) {
        int i, ph = (ms / 160) % 6;
        for (i = 0; i < 3; i++) {
            int on = (ph == i || ph == i + 1);
            gfx_circle(SCR_W/2 - 16 + i * 16, 330, on ? 5 : 4, on ? C_ACCENT : 0x1E2A18, 255);
        }
        gfx_text_c(&font_small, SCR_W/2, 350, net_done ? (net_res ? T("Conectado") : T("Sin red")) : T("Conectando..."), 0x5A5E64);
    }
    /* pie: aviso y atribucion de la musica (CC BY 3.0) */
    if (ms > 2200) {
        gfx_text_c(&font_small, SCR_W/2, 430, T("App no oficial - no afiliada a Kick"), 0x45484D);
        gfx_text_c(&font_small, SCR_W/2, 448, T("M\xc3\xbasica: Debussy, Clair de lune - piano Laurens Goedhart (dominio p\xc3\xba" "blico)"), 0x45484D);
        gfx_text(&font_small, SCR_W-92, 18, T("A: saltar"), 0x45484D);
    }
    /* entrada desde negro y salida a negro */
    if (ms < 500) fade_black(255 - ms * 255 / 500);
    if (out_ms >= 0) fade_black(out_ms * 255 / 350);
}

/* devuelve el resultado de sh_net_ready() */
int intro_run(void) {
    net_done = 0;
    if (LWP_CreateThread(&net_th, net_fn, NULL, NULL, 64 * 1024, 60) < 0) { net_th = LWP_THREAD_NULL; net_res = sh_net_ready(); net_done = 1; }
    music_start();
    u64 t0 = gettime(), tout = 0;
    int skip = 0;
    while (SYS_MainLoop()) {
        int ms = (int)ticks_to_millisecs(gettime() - t0);
        u32 d = in_poll();
        while (in_key()) d |= WPAD_BUTTON_A;
        if (!skip && ms > 300 && (d & (WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_2 | WPAD_BUTTON_PLUS | WPAD_BUTTON_HOME))) {
            skip = 1;
            if (d & WPAD_BUTTON_B) intro_music_stop();
        }
        int ready = (net_done || ms > MAX_NET_MS) && (skip || ms >= MIN_MS);
        if (ready && !tout) tout = gettime();
        int out_ms = tout ? (int)ticks_to_millisecs(gettime() - tout) : -1;
        draw(ms, out_ms);
        gfx_present();
        if (out_ms >= 350) break;
    }
    if (net_th != LWP_THREAD_NULL) {
        if (!net_done) { sui_busy("KickWii", T("Iniciando la red...")); while (!net_done) { ui_busy_tick(); VIDEO_WaitVSync(); } sui_busy_end(); }
        LWP_JoinThread(net_th, NULL); net_th = LWP_THREAD_NULL;
    }
    return net_res;
}
