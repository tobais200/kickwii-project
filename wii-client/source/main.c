/*
 * KickWii - cliente NO OFICIAL (fan-made) de kick.com para la Wii (Homebrew Channel).
 * Interfaz al estilo kick.com: barra superior con buscador, barra lateral de canales
 * recomendados, grilla de tarjetas en vivo (miniatura, EN VIVO, espectadores, avatar,
 * titulo, categoria), pagina de canal y reproductor a pantalla completa.
 * Datos: servidor companion en la LAN (miniaturas, proxy/transcodificacion) o directo
 * a kick.com. Datos publicos con login opcional y chat OAuth. No afiliado a Kick
 * ni a Nintendo.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>
#include <fat.h>
#include <curl/curl.h>
#include "tv.h"
#include "dlog.h"
#include "mem2.h"
#include "social.h"
static int chat_write_pressed,watch_social_pressed,chat_actions_pressed;
#include "catalog.h"

/* ---------------------------------------------------------------- layout (640/854x480) */
#define TOP_H    48
#define SB_W     150
#define MX       (SB_W + 12)
#define MW       (SCR_W - MX - 10)
#define HDR_Y    (TOP_H + 8)
#define CHIP_Y   (TOP_H + 36)
#define GRID_Y   (TOP_H + 66)
#define GRID_H   (SCR_H - GRID_Y)
#define CW       ((MW-(COLS-1)*CGAP_X)/COLS)
#define TH       (CW*9/16)
#define CH       (TH + 56)
#define CGAP_X   20
#define CGAP_Y   14
#define COLS     (gfx_widescreen()?3:2)

enum { SCR_HOME = 0, SCR_SEARCH, SCR_CHAN, SCR_WATCH, SCR_BROWSE };
static int screen = SCR_HOME, prev_screen = SCR_HOME;
static int fat_ok = 0, quit = 0;

static klist_t live;                 /* feed en vivo */
static char live_after[160] = "";
static int live_more = 0, live_loaded = 0;
static char live_err[220] = "";
static int *flt = NULL, nflt = 0, nhidden = 0;
static char cats[8][48],catsart[8][768]; static int ncats = 0, cat_sel = 0;
static int bsel=0,b_act=0,rec_pick=-1,watch_details=1;
static int watch_chat_started = 0;
static int browse_ids[KC_CAP],browse_n,browse_top,browse_gen=-1,browse_mature=-1;
static unsigned long browse_group;
static char browse_query[64];
static kc_category active_category;
#define BCOLS (gfx_widescreen()?6:4)
#define BCW ((MW-(BCOLS-1)*12)/BCOLS)
#define BCH (BCW+46)
static void browse_sync(void) {
    kc_status st;kc_status_get(&st);
    if(browse_gen==st.gen && browse_mature==k_show_mature)return;
    /* Turning +18 off can remove the selected parent group entirely. Never
       leave a hidden Gambling filter behind a misleading "All groups" label. */
    if(browse_mature!=k_show_mature && !k_show_mature && browse_group) {
        kc_group groups[24];int ng=kc_groups(groups,24,k_show_mature),found=0;
        for(int i=0;i<ng;i++)if(groups[i].id==browse_group)found=1;
        if(!found)browse_group=0;
    }
    int old=(bsel>=0&&bsel<browse_n)?browse_ids[bsel]:-1;
    browse_n=kc_view(browse_ids,KC_CAP,k_show_mature,browse_group,browse_query);
    bsel=0;for(int i=0;i<browse_n;i++)if(browse_ids[i]==old){bsel=i;break;}
    browse_gen=st.gen;browse_mature=k_show_mature;
}
static void browse_visible(void) {
    if(bsel<0)bsel=0;
    if(bsel>=browse_n)bsel=browse_n?browse_n-1:0;
    int row=bsel/BCOLS;if(row<browse_top)browse_top=row;if(row>=browse_top+2)browse_top=row-1;
    int max=(browse_n+BCOLS-1)/BCOLS-2;if(max<0)max=0;
    if(browse_top>max)browse_top=max;
    if(browse_top<0)browse_top=0;
}
static void browse_move(u32 d) {
    if(d&WPAD_BUTTON_LEFT)bsel=bsel>0?bsel-1:0;
    if(d&WPAD_BUTTON_RIGHT)bsel=bsel+1<browse_n?bsel+1:bsel;
    if(d&WPAD_BUTTON_UP)bsel=bsel>=BCOLS?bsel-BCOLS:bsel;
    if(d&WPAD_BUTTON_DOWN)bsel=bsel+BCOLS<browse_n?bsel+BCOLS:(browse_n?browse_n-1:0);
    if(d&(WPAD_BUTTON_LEFT|WPAD_BUTTON_RIGHT|WPAD_BUTTON_UP|WPAD_BUTTON_DOWN)) {
        if(in_ptr.virt)in_ptr.valid=0;
        browse_visible();
    }
}
/* rc10: scroll de la grilla de categorias con el puntero (como la grilla de inicio).
   Mueve la ventana visible una fila y arrastra la seleccion para que browse_visible()
   no la devuelva a la fila anterior. */
static u64 browse_edge_t = 0;
static void browse_scroll(int dr) {
    int max=(browse_n+BCOLS-1)/BCOLS-2;if(max<0)max=0;
    int top=browse_top+dr;if(top>max)top=max;if(top<0)top=0;
    if(top==browse_top)return;
    browse_top=top;
    int row=bsel/BCOLS,col=bsel%BCOLS;
    if(row<top)row=top;
    if(row>top+1)row=top+1;
    bsel=row*BCOLS+col;
    if(bsel>=browse_n)bsel=browse_n?browse_n-1:0;
}
static void browse_edge_scroll(void) {
    if(!in_ptr.valid||browse_n<=BCOLS*2)return;
    int dr=0;
    if(ui_hover(MX,SCR_H-36,MW,36))dr=1;              /* borde de abajo (pie de la grilla) */
    else if(ui_hover(MX,118,MW,20))dr=-1;              /* borde de arriba (entre el estado y la grilla) */
    if(!dr){browse_edge_t=0;return;}
    u64 now=gettime();
    if(browse_edge_t&&ticks_to_millisecs(now-browse_edge_t)<300)return;
    browse_edge_t=now;browse_scroll(dr);
}
static void browse_enter(void) {
    if(!kc_start())ui_toast("No se pudo cargar el catalogo de Kick.");
    browse_sync();browse_visible();screen=SCR_BROWSE;
}
static int sel = 0;                  /* indice en flt (o "mostrar mas" = nflt) */
static int scroll = 0, scroll_t = 0;

static klist_t results;              /* busqueda */
static char search_q[48] = "";
static int rsel = 0, rscroll = 0;
static char search_err[220] = "";

static kstream_t chan;               /* canal abierto */
static char chan_err[220] = "";
static int chan_focus = 0;           /* 0 ver, 1 modo, 2 volver */

static kstream_t pinfo;              /* canal que se esta viendo */
static int playing = 0;
static u64 osd_t = 0;
static const tv_pic_t *curpic = NULL;
static long k_time_off = 0;          /* reloj UTC del servidor - reloj de la Wii */
static FILE *atlog = NULL;

static u64 t_start = 0;
static int at_block = 0;             /* prueba automatica: no avanzar el guion durante un pedido */
#define LOG(...) do { if (atlog) { fprintf(atlog, "[%6.1fs] ", t_start ? ticks_to_millisecs(gettime() - t_start) / 1000.0 : 0.0); fprintf(atlog, __VA_ARGS__); fputc('\n', atlog); fflush(atlog); } } while (0)
#define BUSY(x) do { at_block = 1; x; at_block = 0; } while (0)

/* ---------------------------------------------------------------- utilidades */
static void fmt_count(int n, char *o, int sz) {
    if (n >= 1000000) snprintf(o, sz, "%d,%d M", n / 1000000, (n / 100000) % 10);
    else if (n >= 10000) snprintf(o, sz, "%d mil", n / 1000);
    else if (n >= 1000) snprintf(o, sz, "%d,%d mil", n / 1000, (n / 100) % 10);
    else snprintf(o, sz, "%d", n);
}

static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
/* "hace 2 h 15 min" a partir de start (UTC, "2026-11-04T20:22:43Z" o "2026-11-04 17:06:58") */
static void fmt_since(const char *start, char *o, int sz) {
    int Y, M, D, h, m, s;
    o[0] = 0;
    if (!k_server_ok || !start[0] || sscanf(start, "%d-%d-%d%*c%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return;
    long t0 = days_from_civil(Y, M, D) * 86400L + h * 3600 + m * 60 + s;
    long now = (long)sh_clock_now() + k_time_off;
    long mins = (now - t0) / 60;
    if (mins < 0 || mins > 60 * 72) return;
    if (mins < 60) snprintf(o, sz, "hace %ld min", mins);
    else snprintf(o, sz, "hace %ld h %ld min", mins / 60, mins % 60);
}

static int hidden_by_filter(const kstream_t *s) { return !k_show_mature && (s->mature || s->gambling); }

static void rebuild_filter(void) {
    int i, j;
    int *replacement = malloc(sizeof(int) * (live.n + 1));
    int *previous = flt; flt = replacement; free(previous);
    nflt = 0; nhidden = 0;
    if (!flt || (live.n && !live.v)) {
        ncats = cat_sel = sel = 0;
        ui_toast("Sin memoria para filtrar la lista. Reintenta actualizar.");
        return;
    }
    /* categorias (en orden de aparicion = por espectadores) */
    ncats = 0;
    for (i = 0; i < live.n && ncats < 8; i++) {
        if (hidden_by_filter(&live.v[i]) || !live.v[i].cat[0]) continue;
        for (j = 0; j < ncats; j++) if (!strcmp(cats[j], live.v[i].cat)) break;
        if (j == ncats) {snprintf(cats[ncats],sizeof(cats[0]),"%s",live.v[i].cat);snprintf(catsart[ncats],sizeof(catsart[0]),"%s",live.v[i].catart);ncats++;}
    }
    if (cat_sel < 0 || cat_sel > ncats) cat_sel = 0;
    for (i = 0; i < live.n; i++) {
        if (hidden_by_filter(&live.v[i])) { nhidden++; continue; }
        if (cat_sel && strcmp(live.v[i].cat, cats[cat_sel - 1])) continue;
        flt[nflt++] = i;
    }
    if (sel < 0) sel = 0;
    if (sel > nflt) sel = nflt;
}

/* Language table is static; keep all three entry points in sync. */
static void next_language(void) {
    if (k_lang < 0 || k_lang >= k_nlangs) k_lang = 0;
    k_lang = (k_lang + 1) % k_nlangs;
    sel = scroll = scroll_t = rsel = rscroll = 0;
    cfg_save();
}
static const kstream_t *filtered_stream(int i) {
    if (!flt || !live.v || i < 0 || i >= nflt || flt[i] < 0 || flt[i] >= live.n) return NULL;
    return &live.v[flt[i]];
}

/* ---------------------------------------------------------------- red (a pedido) */
static void load_live(int reset) {
    ui_backdrop_suspend(1);
    char err[220] = "";
    int more = 0;
    sui_busy("KickWii", reset ? "Cargando canales en vivo..." : "Cargando m\xc3\xa1s canales...");
    if (reset) {
        thumbs_flush();
        /* busy_draw()/curl progress can re-enter draw_home(): never leave stale
           filter indices pointing into a cleared or partially rebuilt list. */
        free(flt); flt = NULL; nflt = nhidden = ncats = 0;
        live_more = 0; cat_sel = sel = 0; scroll = scroll_t = 0;
        klist_clear(&live); live_after[0] = 0;
    }
    int r;
    BUSY(r = active_category.id ? kw_category_live(active_category.id,live_after,sizeof(live_after),&live,&more,err,sizeof(err)) : kw_live(live_after, sizeof(live_after), &live, &more, err, sizeof(err)));
    if(active_category.id)for(int ci=0;ci<live.n;ci++){
        snprintf(live.v[ci].catart,sizeof(live.v[ci].catart),"%s",active_category.art);
        live.v[ci].gambling=active_category.gambling;
        if(active_category.mature)live.v[ci].mature=1;
    }
    live_loaded = 1;
    live_more = r > 0 && more;
    snprintf(live_err, sizeof(live_err), "%s", r < 0 ? err : "");
    if (r < 0) ui_toast(err);
    LOG("live: r=%d n=%d more=%d mock=%d err=%s", r, live.n, live_more, k_last_mock, err);
    rebuild_filter();
    sui_busy_end();
    ui_backdrop_suspend(0);
}

static void check_server(void) {
    char err[200] = "";
    if (!k_server[0]) { k_server_ok = 0; return; }
    sui_busy("KickWii", "Conectando con el servidor companion...");
    int ok; BUSY(ok = kw_health(err, sizeof(err)));
    sui_busy_end();
    if (ok) {
        sh_resp_t r; char url[200];
        snprintf(url, sizeof(url), "%s/wii/health", k_server);
        if (sh_request("GET", url, NULL, NULL, NULL, &r) == 0 && r.data) {
            const char *v = jget(jws(r.data), "time");
            if (v) k_time_off = (long)jnum(v) - (long)sh_clock_now();
        }
        sh_resp_free(&r);
    } else ui_toast(err);
    LOG("health: ok=%d err=%s", ok, err);
}

static void open_channel(const kstream_t *from, const char *slug) {
    char err[220] = "";
    if (from) chan = *from; else { memset(&chan, 0, sizeof(chan)); snprintf(chan.slug, sizeof(chan.slug), "%s", slug); }
    sui_busy("KickWii", "Abriendo canal...");
    int r; BUSY(r = kw_channel(slug, &chan, err, sizeof(err)));
    sui_busy_end();
    snprintf(chan_err, sizeof(chan_err), "%s", r < 0 ? err : "");
    if (r < 0 && !from) { ui_toast(err); return; }
    LOG("channel %s: r=%d live=%d viewers=%d cat=%s err=%s", slug, r, chan.live, chan.viewers, chan.cat, err);
    chan_focus = chan.live ? 0 : 2;
    prev_screen = screen == SCR_CHAN ? prev_screen : screen;
    screen = SCR_CHAN;
}

static const kstream_t *recommendation(const char *slug,int nth) {
    int i;for(i=0;i<live.n;i++)if(!hidden_by_filter(&live.v[i])&&strcmp(live.v[i].slug,slug)) {
        if(!nth--)return &live.v[i];
    }
    return NULL;
}
static void warm_watch_images(void) {
    const char *paths[8];int n=0,i,ready;
    paths[n++]=chan.avatar;paths[n++]=chan.thumb;paths[n++]=chan.banner;paths[n++]=chan.catart;
    for(i=0;i<4;i++){const kstream_t *r=recommendation(chan.slug,i);if(r)paths[n++]=r->thumb;}
    ui_set_backdrop(NULL);sui_busy("KickWii","Preparando imagenes del directo...");
    thumbs_pause(0);at_block=1;u64 start=gettime();
    do {thumbs_frame();ready=0;for(i=0;i<n;i++)if(!paths[i][0]||thumb_get(paths[i]))ready++;
        ui_busy_tick();if(ready==n)break;usleep(50000);
    }while(ticks_to_millisecs(gettime()-start)<8000);
    LOG("image prefetch: %d/%d ready",ready,n);at_block=0;sui_busy_end();thumbs_pause(1);
}
static void start_watch(void) {
    kc_pause(1);
    warm_watch_images();
    watch_details=k_chat_on;
    static char url[4096];
    char refresh[300], err[220] = "";
    int max_h = 360;
    if (!kw_play_url(&chan, url, sizeof(url), refresh, sizeof(refresh), &max_h, err, sizeof(err))) { thumbs_pause(0); ui_toast(err); LOG("play: %s", err); return; }
    pinfo = chan;
    tp_set_limits(max_h, refresh);
    intro_music_stop();
    tp_play(url, VM_AUTO);
    if(auth_due())auth_ensure();
    watch_chat_started = watch_details;
    if (watch_chat_started) chat_start(&pinfo);
    playing = 1;
    screen = SCR_WATCH;
    osd_t = gettime();
    LOG("play: channel=%s direct=%d",pinfo.slug,!k_server[0]);
    dlog("ver canal %s (%s)", pinfo.slug, k_server[0] ? "servidor LAN" : "directo");
}

static void stop_watch(void) { chat_stop();tp_stop(); playing = 0; thumbs_pause(0); kc_pause(0); }

static void do_search(void) {
    char q[48];
    snprintf(q, sizeof(q), "%s", search_q);
    if (!sui_text_input("Buscar en Kick", "Escrib\xc3\xad el nombre de un canal o su slug (kick.com/slug). +: buscar", q, sizeof(q) - 1)) return;
    char *p = q; while (*p == ' ') p++;
    int n = strlen(p); while (n && p[n - 1] == ' ') p[--n] = 0;
    if (!*p) return;
    snprintf(search_q, sizeof(search_q), "%s", p);
    klist_clear(&results); rsel = 0; rscroll = 0;
    char err[220] = "";
    sui_busy("Buscar", "Buscando canales...");
    int r; BUSY(r = kw_search(search_q, &results, err, sizeof(err)));
    sui_busy_end();
    snprintf(search_err, sizeof(search_err), "%s", r < 0 ? err : "");
    LOG("search '%s': r=%d err=%s", search_q, r, err);
    screen = SCR_SEARCH;
}

/* ---------------------------------------------------------------- dibujo: piezas */
static void text_bold(const Font *f, int x, int y, const char *s, u32 c) { gfx_text(f, x, y, s, c); gfx_text(f, x + 1, y, s, c); }

static void live_pill(int x, int y) {
    int w = gfx_text_w(&font_small, "EN VIVO") + 12;
    gfx_rrect(x, y, w, 17, 4, C_ACCENT, 255);
    text_bold(&font_small, x + 6, y + 1, "EN VIVO", C_DARK);
}

static void viewers_pill(int x, int y, int n, const char *suffix) {
    char v[24], t[48];
    fmt_count(n, v, sizeof(v));
    snprintf(t, sizeof(t), "%s%s", v, suffix);
    int w = gfx_text_w(&font_small, t) + 12;
    gfx_rrect(x, y, w, 17, 4, 0x000000, 190);
    gfx_text(&font_small, x + 6, y + 1, t, C_TEXT);
}

static int chip(int x, int y, const char *t, int on, int maxw) {
    int w = gfx_text_w(&font_small, t) + 16;
    if (maxw && w > maxw) w = maxw;
    int hov = ui_hover(x, y, w, 20);
    gfx_rrect(x, y, w, 20, 10, on ? C_ACCENT : (hov ? 0x30343A : C_PANEL2), 255);
    gfx_text_fit(&font_small, x + 8, y + 2, t, on ? C_DARK : (hov ? C_TEXT : C_DIM), w - 14);
    return w;
}

static void placeholder(const kstream_t *s, int x, int y, int w, int h, const Font *f) {
    gfx_vgrad(x, y, w, h, s->color, gfx_mix(s->color, C_BG, 200));
    gfx_text_c(f, x + w / 2, y + h / 2 - f->line_h / 2 - 4, s->shortn, 0xFFFFFF);
    if (h > 60) {
        char t[80]; snprintf(t, sizeof(t), "kick.com/%s", s->slug);
        gfx_text_c(&font_small, x + w / 2, y + h / 2 + f->line_h / 2, t, gfx_mix(0xFFFFFF, s->color, 90));
    }
}

static void avatar(const kstream_t *s, const char *path, int x, int y, int d, int ring) {
    const kimg_t *im = thumb_get(path);
    if (ring) gfx_circle(x + d / 2, y + d / 2, d / 2 + 3, C_ACCENT, 255);
    if (im) gfx_blit_scaled_round(im->px, im->w, im->h, x, y, d, d, d/2);
    else {
        gfx_circle(x + d / 2, y + d / 2, d / 2, s->color, 255);
        const Font *f = d >= 56 ? &font_title : (d >= 36 ? &font_bold : &font_small);
        gfx_text_c(f, x + d / 2, y + (d - f->line_h) / 2, s->shortn, 0xFFFFFF);
    }
}

static void wordmark(int x, int y) {
    text_bold(&font_title, x, y, "KICK", C_ACCENT);
    int w = gfx_text_w(&font_title, "KICK") + 2;
    text_bold(&font_small, x + w + 2, y + 9, "wii", C_TEXT);
}

/* ---------------------------------------------------------------- barra superior + lateral */
enum { TB_NONE = 0, TB_HOME, TB_SEARCH, TB_SETTINGS, TB_BROWSE, TB_ACCOUNT };
static int topbar(void) {
    int act = TB_NONE;
    gfx_rect(0, 0, SCR_W, TOP_H, C_BG2);
    gfx_rect(0, TOP_H - 1, SCR_W, 1, 0x000000);
    wordmark(14, 10);
    if (ui_click(10, 4, 110, 40)) act = TB_HOME;
    if(ui_button(110,10,44,28,"Cat.",0,BS_NORMAL,screen==SCR_BROWSE))act=TB_BROWSE;
    /* buscador */
    int sx = 160, sw = SCR_W-420, sy = 10, sh = 28;
    int hov = ui_hover(sx, sy, sw, sh);
    gfx_rrect(sx, sy, sw, sh, 8, C_PANEL, 255);
    gfx_rframe(sx, sy, sw, sh, 8, 1, hov ? C_ACCENT : C_LINE, 255);
    gfx_text_fit(&font_small, sx + 12, sy + 6, search_q[0] ? search_q : "Buscar canal o slug...", search_q[0] ? C_TEXT : C_FAINT, sw - 60);
    gfx_rrect(sx + sw - 36, sy, 36, sh, 8, C_PANEL2, 255);
    gfx_icon(IC_SEARCH, sx + sw - 18, sy + sh / 2, 14, C_TEXT);
    if (ui_click(sx, sy, sw, sh)) act = TB_SEARCH;
    /* estado del servidor */
    u32 dc = !k_server[0] ? C_WARN : (k_server_ok ? (k_last_mock ? C_WARN : C_ACCENT) : C_LIVE);
    const char *dl = !k_server[0] ? "Directo" : (k_server_ok ? (k_last_mock ? "DEMO" : "LAN") : "Sin servidor");
    gfx_circle(SCR_W-164, 24, 5, dc, 255);
    gfx_text(&font_small, SCR_W-154, 16, dl, C_DIM);
    /* chip de cuenta: avatar + nombre (o "Entrar") */
    {
        int cx = SCR_W - 252, cw = 84, hovc = ui_hover(cx, 8, cw, 32);
        gfx_rrect(cx, 8, cw, 32, 16, hovc ? C_PANEL2 : C_PANEL, 255);
        gfx_rframe(cx, 8, cw, 32, 16, 1, auth_ready() ? C_ACCENT : C_LINE, 255);
        const kimg_t *ai = auth_ready() && auth_avatar()[0] ? thumb_get(auth_avatar()) : NULL;
        if (ai) gfx_blit_scaled_round(ai->px, ai->w, ai->h, cx + 4, 12, 24, 24, 12);
        else { gfx_circle(cx + 16, 24, 12, auth_ready() ? C_ACCENT : C_PANEL2, 255); gfx_icon(IC_USER, cx + 16, 24, 14, auth_ready() ? C_DARK : C_DIM); }
        gfx_text_fit(&font_small, cx + 32, 16, auth_ready() ? auth_login() : (auth_has_session() ? (auth_login()[0] ? auth_login() : "Cuenta") : "Entrar"), auth_ready() ? C_TEXT : C_DIM, cw - 38);
        if (ui_click(cx, 8, cw, 32)) act = TB_ACCOUNT;
    }
    if (ui_button(SCR_W-72, 10, 62, 28, NULL, IC_MENU, BS_NORMAL, 0)) act = TB_SETTINGS;
    gfx_text(&font_small, 0, 0, "", 0);
    return act;
}

static const char *sb_pick = NULL;
static void sidebar(void) {
    int i, y = TOP_H + 8, shown = 0;
    gfx_rect(0, TOP_H, SB_W, SCR_H - TOP_H, C_BG2);
    gfx_rect(SB_W - 1, TOP_H, 1, SCR_H - TOP_H, 0x000000);
    text_bold(&font_small, 10, y, "RECOMENDADOS", C_FAINT);
    y += 22;
    for (i = 0; i < live.n && shown < 12 && y + 30 < SCR_H - 22; i++) {
        const kstream_t *s = &live.v[i];
        if (hidden_by_filter(s)) continue;
        int hov = ui_hover(4, y, SB_W - 8, 30);
        if (hov) gfx_rrect(4, y, SB_W - 8, 30, 6, C_PANEL2, 255);
        avatar(s, s->avatar, 10, y + 3, 24, 0);
        char v[24]; fmt_count(s->viewers, v, sizeof(v));
        int vw = gfx_text_w(&font_small, v);
        gfx_text_fit(&font_small, 40, y + 1, s->name, C_TEXT, SB_W - 52 - vw - 10);
        gfx_text_fit(&font_small, 40, y + 14, s->cat, C_FAINT, SB_W - 50);
        gfx_circle(SB_W - 14 - vw, y + 9, 3, C_LIVE, 255);
        gfx_text(&font_small, SB_W - 8 - vw, y + 1, v, C_DIM);
        if (ui_click(4, y, SB_W - 8, 30)) sb_pick = s->slug;
        y += 31; shown++;
    }
    if (!shown) gfx_text(&font_small, 10, y, live_loaded ? "(sin canales)" : "Cargando...", C_FAINT);
    gfx_text(&font_small, 10, SCR_H - 20, "No oficial \xc2\xb7 v" TV_VERSION, C_FAINT);
}

/* ---------------------------------------------------------------- inicio: grilla */
static int card_pos(int i, int *x, int *y) {
    *x = MX + (i % COLS) * (CW + CGAP_X);
    *y = GRID_Y + (i / COLS) * (CH + CGAP_Y) - scroll;
    return *y + CH >= GRID_Y && *y < SCR_H;
}

static int draw_card(const kstream_t *s, int x, int y, int focus) {
    int hov = ui_hover(x, y, CW, CH);
    int lift = hov || focus;
    int tx = x, ty = y;
    if (lift) { gfx_rrect(x, y, CW, TH, 8, C_ACCENT, 255); tx += 4; ty -= 4; }   /* efecto hover de Kick */
    const kimg_t *im = thumb_get(s->thumb);
    if (im) gfx_blit_scaled_round(im->px, im->w, im->h, tx, ty, CW, TH, 8);
    else placeholder(s, tx, ty, CW, TH, &font_big);
    live_pill(tx + 8, ty + 8);
    viewers_pill(tx + 8, ty + TH - 25, s->viewers, " espectadores");
    if (s->mature) { gfx_rrect(tx + CW - 36, ty + 8, 28, 17, 4, C_LIVE, 255); gfx_text(&font_small, tx + CW - 31, ty + 9, "+18", 0xFFFFFF); }
    int my = y + TH + 8;
    avatar(s, s->avatar, x, my, 28, 0);
    gfx_text_fit(&font_bold, x + 36, my - 2, s->title[0] ? s->title : "(sin t\xc3\xadtulo)", C_TEXT, CW - 38);
    gfx_text_fit(&font_small, x + 36, my + 16, s->name, C_DIM, CW - 38);
    if (s->cat[0]) chip(x + 36, my + 32, s->cat, 0, CW - 40);
    return ui_click(x, y, CW, CH);
}

static int draw_more(int x, int y, int focus) {
    int hov = ui_hover(x, y, CW, TH);
    gfx_rrect(x, y, CW, TH, 8, hov || focus ? C_PANEL2 : C_PANEL, 255);
    gfx_rframe(x, y, CW, TH, 8, focus ? 2 : 1, focus ? C_ACCENT : C_LINE, 255);
    gfx_icon(IC_DOWN, x + CW / 2, y + TH / 2 - 14, 22, C_ACCENT);
    gfx_text_c(&font_bold, x + CW / 2, y + TH / 2 + 4, "Mostrar m\xc3\xa1s", C_TEXT);
    return ui_click(x, y, CW, TH);
}

enum { HA_NONE = 0, HA_OPEN, HA_MORE, HA_LANG, HA_REFRESH, HA_CAT };
static int h_act = HA_NONE, h_arg = 0;

static void ensure_visible(void) {
    int row = sel / COLS;
    int top = row * (CH + CGAP_Y), bot = top + CH;
    if (top < scroll_t) scroll_t = top;
    if (bot > scroll_t + GRID_H - 8) scroll_t = bot - GRID_H + 8;
}

static void draw_home(void) {
    int i;
    gfx_clear(C_BG);
    /* encabezado de la seccion */
    gfx_circle(MX + 5, HDR_Y + 10, 5, C_ACCENT, 255);
    text_bold(&font_bold, MX + 16, HDR_Y + 1, "Canales en vivo", C_TEXT);
    int lx = MX + 16 + gfx_text_w(&font_bold, "Canales en vivo") + 14;
    char lab[40]; snprintf(lab, sizeof(lab), "Idioma: %s", k_lang_names[k_lang]);
    int lw = chip(lx, HDR_Y, lab, 1, 0);
    if (ui_click(lx, HDR_Y, lw, 20)) h_act = HA_LANG;
    if (ui_button(MX + MW - 92, HDR_Y - 2, 92, 24, "Actualizar", 0, BS_NORMAL, 0)) h_act = HA_REFRESH;
    /* chips de categoria */
    int cx = MX;
    if(active_category.id) {
        char label[160];snprintf(label,sizeof(label),"%s - %d directos cargados",active_category.name,live.n);
        gfx_text_fit(&font_small,MX,CHIP_Y,label,C_DIM,MW);
    }
    for (i = 0; !active_category.id && i <= ncats; i++) {
        const char *t = i ? cats[i - 1] : "Todas";
        int w = gfx_text_w(&font_small, t) + 16;
        if (cx + w > MX + MW) break;
        chip(cx, CHIP_Y, t, cat_sel == i, 0);
        if (ui_click(cx, CHIP_Y, w, 20)) { h_act = HA_CAT; h_arg = i; }
        cx += w + 6;
    }
    /* grilla */
    int ncards = nflt + (live_more ? 1 : 0);
    int rows = (ncards + COLS - 1) / COLS;
    int content = rows * (CH + CGAP_Y);
    int maxs = content > GRID_H - 8 ? content - GRID_H + 8 : 0;
    if (scroll_t > maxs) scroll_t = maxs;
    if (scroll_t < 0) scroll_t = 0;
    scroll += (scroll_t - scroll) / 3;
    if (abs(scroll_t - scroll) < 3) scroll = scroll_t;
    gfx_clip(SB_W, GRID_Y - 6, SCR_W - SB_W, GRID_H + 6);
    for (i = 0; i < ncards; i++) {
        int x, y;
        if (!card_pos(i, &x, &y)) continue;
        u32 save = UI.down;
        if (in_ptr.y < GRID_Y - 6) UI.down &= ~WPAD_BUTTON_A;   /* el clic cae en los chips, no en la tarjeta tapada */
        int foc = i == sel && !in_ptr.valid;
        if (i < nflt) { const kstream_t *card = filtered_stream(i); if (card && draw_card(card, x, y, foc)) { h_act = HA_OPEN; h_arg = i; } }
        else if (draw_more(x, y, foc)) h_act = HA_MORE;
        UI.down = save;
    }
    gfx_noclip();
    if (!nflt) {
        const char *a = live_loaded ? (live_err[0] ? "No se pudo cargar la lista" : "No hay directos para este filtro") : "Cargando...";
        gfx_text_c(&font_bold, MX + MW / 2, GRID_Y + 110, a, C_DIM);
        if (live_err[0]) {
            char **ln; int n = ui_wrap(&font_small, live_err, MW - 40, &ln), k;
            for (k = 0; k < n && k < 4; k++) gfx_text_c(&font_small, MX + MW / 2, GRID_Y + 140 + k * 16, ln[k], C_FAINT);
            for (k = 0; k < n; k++) free(ln[k]);
            free(ln);
        }
        if (nhidden) gfx_text_c(&font_small, MX + MW / 2, GRID_Y + 210, "(hay canales ocultos por el filtro +18/apuestas)", C_FAINT);
    }
    if (maxs > 0) {
        int th = GRID_H * GRID_H / (content + 1), ty = GRID_Y + (GRID_H - th) * scroll / maxs;
        gfx_rrect(SCR_W - 6, GRID_Y, 3, GRID_H, 1, C_PANEL, 255);
        gfx_rrect(SCR_W - 6, ty, 3, th, 1, C_ACCENT2, 255);
        if (ui_hover(MX, SCR_H - 22, MW, 22) && scroll_t < maxs) scroll_t += 6;
        if (ui_hover(MX, GRID_Y - 4, MW, 14) && scroll_t > 0) scroll_t -= 6;
    }
    sidebar();
    int tb = topbar();
    if (tb) h_act = 100 + tb;
    if (k_last_mock && k_server_ok) {
        gfx_rrect(MX + 20, SCR_H - 30, MW - 40, 24, 6, C_WARN, 240);
        gfx_text_fit(&font_small, MX + 30, SCR_H - 26, k_notice[0] ? k_notice : "DATOS DEMO (kick.com no respondi\xc3\xb3)", C_DARK, MW - 60);
    }
    ui_draw_toast(SCR_H - 34);
}

/* ---------------------------------------------------------------- busqueda */
static int s_act = 0, s_arg = 0;
static int valid_slug(const char *s) {
    int i;
    if (!s[0] || strlen(s) > 40) return 0;
    for (i = 0; s[i]; i++) if (!isalnum((unsigned char)s[i]) && s[i] != '_' && s[i] != '-') return 0;
    return 1;
}
static void draw_search(void) {
    int i, y0 = TOP_H + 50, rh = 50;
    gfx_clear(C_BG);
    char t[96]; snprintf(t, sizeof(t), "Resultados para \"%s\"", search_q);
    text_bold(&font_bold, MX, TOP_H + 12, t, C_TEXT);
    int extra = valid_slug(search_q) ? 1 : 0;
    int n = results.n + extra, vis = (SCR_H - y0 - 30) / rh;
    if (rsel >= n) rsel = n ? n - 1 : 0;
    if (rsel < rscroll) rscroll = rsel;
    if (rsel >= rscroll + vis) rscroll = rsel - vis + 1;
    for (i = 0; i < vis && rscroll + i < n; i++) {
        int k = rscroll + i, y = y0 + i * rh;
        int foc = k == rsel && !in_ptr.valid, hov = ui_hover(MX, y, MW, rh - 6);
        gfx_rrect(MX, y, MW, rh - 6, 8, hov || foc ? C_PANEL2 : C_PANEL, 255);
        if (foc) gfx_rframe(MX, y, MW, rh - 6, 8, 2, C_ACCENT, 255);
        if (extra && k == 0) {
            gfx_rrect(MX + 10, y + 6, 32, 32, 8, C_ACCENT, 255);
            gfx_icon(IC_PLAY, MX + 26, y + 22, 14, C_DARK);
            char g[96]; snprintf(g, sizeof(g), "Ir directo a kick.com/%s", search_q);
            text_bold(&font_bold, MX + 54, y + 12, g, C_TEXT);
        } else {
            const kstream_t *s = &results.v[k - extra];
            avatar(s, s->avatar, MX + 8, y + 4, 36, s->live > 0);
            gfx_text_fit(&font_bold, MX + 54, y + 4, s->name, C_TEXT, MW - 170);
            if (s->verified) { int w = gfx_text_w(&font_bold, s->name); if (w < MW - 170) { gfx_circle(MX + 62 + w, y + 13, 7, C_ACCENT, 255); gfx_icon(IC_OK, MX + 62 + w, y + 13, 9, C_DARK); } }
            char f[48], v[24]; fmt_count(s->followers, v, sizeof(v)); snprintf(f, sizeof(f), "%s seguidores", v);
            gfx_text(&font_small, MX + 54, y + 23, f, C_FAINT);
            if (s->live > 0) live_pill(MX + MW - 80, y + 13);
            else chip(MX + MW - 80, y + 12, "Offline", 0, 0);
        }
        if (ui_click(MX, y, MW, rh - 6)) { s_act = 1; s_arg = k; }
        if (hov) rsel = k;
    }
    if (!n) gfx_text_c(&font_bold, MX + MW / 2, y0 + 80, search_err[0] ? search_err : "Sin resultados", C_DIM);
    gfx_text(&font_small, MX, SCR_H - 22, "A: abrir canal   B: volver   1: nueva b\xc3\xbasqueda", C_FAINT);
    sidebar();
    int tb = topbar();
    if (tb) s_act = 100 + tb;
    ui_draw_toast(SCR_H - 30);
}

/* Kick's existing channel response has no VOD list. This strip is explicitly LIVE recommendations. */
static void draw_recs(const char *slug,int x,int y,int w,int h,int clickable) {
    int k,tw=(w-3*12)/4;
    for(k=0;k<4;k++) {
        const kstream_t *r=recommendation(slug,k);if(!r)break;
        int xx=x+k*(tw+12);const kimg_t *im=thumb_get(r->thumb);
        if(im)gfx_blit_scaled_round(im->px,im->w,im->h,xx,y,tw,h,4);
        else gfx_rect(xx,y,tw,h,C_PANEL);
        gfx_text_fit(&font_small,xx,y+h+2,r->name,C_TEXT,tw);
        if(clickable&&ui_click(xx,y,tw,h+18))rec_pick=(int)(r-live.v);
    }
}
static void draw_browse(void) {
    browse_sync();browse_edge_scroll();browse_visible();kc_status st;kc_status_get(&st);
    gfx_clear(C_BG);text_bold(&font_title,MX,TOP_H+10,"Explorar categorias",C_TEXT);
    char label[180];
    if(ui_button(MX,84,147,25,"Buscar categoria",0,BS_NORMAL,0))b_act=2;
    kc_group groups[24];int ng=kc_groups(groups,24,k_show_mature);const char *gn="Todos los grupos";
    for(int i=0;i<ng;i++)if(groups[i].id==browse_group)gn=groups[i].name;
    if(ui_button(MX+155,84,160,25,gn,0,BS_NORMAL,0))b_act=3;
    if(ui_button(MX+323,84,110,25,"Actualizar",0,BS_NORMAL,0))b_act=4;
    if(st.running)snprintf(label,sizeof(label),"Cargando: %d categorias / %d - pagina %d",st.count,st.total,st.pages);
    else if(st.error[0]&&!st.limited)snprintf(label,sizeof(label),"Carga parcial: %d de %d. 2 / Actualizar: reintentar",st.count,st.total);
    else if(st.limited)snprintf(label,sizeof(label),"%d de %d: limite Kick %d paginas - %d visibles",st.count,st.total,st.pages,browse_n);
    else snprintf(label,sizeof(label),"%d disponibles - %d visibles - por espectadores",st.count,browse_n);
    gfx_text_fit(&font_small,MX,116,label,C_DIM,MW);
    int first=browse_top*BCOLS,last=first+BCOLS*2;
    for(int i=first;i<browse_n&&i<last;i++) {
        kc_category c;if(!kc_get(browse_ids[i],&c))continue;
        int x=MX+(i%BCOLS)*(BCW+12),y=140+(i/BCOLS-browse_top)*BCH;
        gfx_rrect(x-2,y-2,BCW+4,BCW+4,5,(i==bsel&&!in_ptr.valid)?C_ACCENT:C_PANEL,255);
        const kimg_t *im=thumb_get(c.art);
        if(im)gfx_blit_scaled_round(im->px,im->w,im->h,x,y,BCW,BCW,4);
        else {gfx_rect(x,y,BCW,BCW,C_PANEL2);gfx_text_fit(&font_small,x+6,y+BCW/2,"KICK",C_DIM,BCW-12);}
        gfx_text_fit(&font_small,x,y+BCW+4,c.name,C_TEXT,BCW);
        char count[24],line[64];fmt_count(c.viewers,count,sizeof(count));snprintf(line,sizeof(line),"%s espectadores",count);
        gfx_text_fit(&font_small,x,y+BCW+21,line,C_DIM,BCW);
        if(ui_click(x,y,BCW,BCH-6)){bsel=i;b_act=1;}
    }
    if(!browse_n)gfx_text_fit(&font_small,MX,175,st.running?"Buscando categorias publicas...":(st.error[0]&&!st.limited)?st.error:"Sin resultados. Cambia el grupo, la busqueda o el filtro +18.",C_DIM,MW);
    if(browse_n>BCOLS*2){int rows=(browse_n+BCOLS-1)/BCOLS,h=292*2/rows;if(h<8)h=8;gfx_rect(SCR_W-6,140,3,292,C_PANEL2);gfx_rect(SCR_W-6,140+browse_top*(292-h)/(rows-2),3,h,C_ACCENT);}
    snprintf(label,sizeof(label),"%d-%d de %d  A: canales  B: inicio  1: buscar  -: grupo arriba/abajo: mover",browse_n?first+1:0,last<browse_n?last:browse_n,browse_n);
    gfx_text_fit(&font_small,MX,456,label,C_FAINT,MW);
    sidebar();int tb=topbar();if(tb)b_act=100+tb;
}
static void browse_search(void) {
    char q[64];snprintf(q,sizeof(q),"%s",browse_query);
    if(sui_text_input("Buscar categoria","Busca entre las categorias cargadas. Vacio: todas.",q,sizeof(q))) {
        snprintf(browse_query,sizeof(browse_query),"%s",q);bsel=browse_top=0;browse_gen=-1;browse_sync();
    }
}
static void browse_groups_dialog(void) {
    kc_group groups[24];int n=kc_groups(groups,24,k_show_mature),pick=0;char *labels[25];labels[0]="Todos los grupos";
    for(int i=0;i<n;i++){labels[i+1]=groups[i].name;if(groups[i].id==browse_group)pick=i+1;}
    u32 result=sui_list("Grupos de Kick","Grupos reales del catalogo publico. A: elegir B: volver",labels,n+1,&pick);
    if(result&WPAD_BUTTON_A){browse_group=pick?groups[pick-1].id:0;bsel=browse_top=0;browse_gen=-1;browse_sync();}
}
static void browse_open(void) {
    browse_sync();if(bsel<0||bsel>=browse_n)return;
    kc_category c;if(!kc_get(browse_ids[bsel],&c)||(!k_show_mature&&c.mature))return;
    active_category=c;cat_sel=sel=scroll=scroll_t=0;screen=SCR_HOME;load_live(1);
}
/* ---------------------------------------------------------------- pagina de canal */
static int c_act = 0;
#define PV_X 16
#define PV_Y 212
#define PV_W 256
#define PV_H 144
static void draw_channel(void) {
    const kstream_t *s = &chan;
    char v[24], line[160];
    gfx_clear(C_BG);
    /* banner */
    const kimg_t *bn = thumb_get(s->banner);
    if (bn) gfx_blit_scaled(bn->px, bn->w, bn->h, 0, TOP_H, SCR_W, 96);
    else gfx_vgrad(0, TOP_H, SCR_W, 96, gfx_mix(s->color, C_BG, 60), C_BG);
    gfx_vgrad(0, TOP_H + 56, SCR_W, 40, gfx_mix(C_BG, 0, 0), C_BG);
    /* cabecera */
    gfx_circle(16 + 32, TOP_H + 58 + 32, 35, C_BG, 255);
    avatar(s, s->avatar, 16, TOP_H + 58, 64, s->live > 0);
    int nx = 100, ny = TOP_H + 104;
    text_bold(&font_title, nx, ny, s->name, C_TEXT);
    int nw = gfx_text_w(&font_title, s->name) + 1;
    if (s->verified) { gfx_circle(nx + nw + 12, ny + 13, 8, C_ACCENT, 255); gfx_icon(IC_OK, nx + nw + 12, ny + 13, 10, C_DARK); nw += 24; }
    if (s->live > 0) live_pill(nx + nw + 8, ny + 6);
    else chip(nx + nw + 8, ny + 4, "Offline", 0, 0);
    fmt_count(s->followers, v, sizeof(v));
    snprintf(line, sizeof(line), "%s seguidores%s", v, s->mock ? "   \xc2\xb7  [DEMO]" : "");
    gfx_text(&font_small, nx, ny + 30, line, C_DIM);
    const kimg_t *ca=thumb_get(s->catart);
    if(ca)gfx_blit_scaled_round(ca->px,ca->w,ca->h,568,146,48,48,4);
    /* vista previa */
    const kimg_t *th = thumb_get(s->thumb);
    if (th) gfx_blit_scaled_round(th->px, th->w, th->h, PV_X, PV_Y, PV_W, PV_H, 10);
    else placeholder(s, PV_X, PV_Y, PV_W, PV_H, &font_big);
    if (s->live > 0) {
        int hov = ui_hover(PV_X, PV_Y, PV_W, PV_H);
        gfx_rect_a(PV_X, PV_Y, PV_W, PV_H, 0x000000, hov ? 40 : 90);
        gfx_circle(PV_X + PV_W / 2, PV_Y + PV_H / 2, 30, C_ACCENT, 255);
        gfx_icon(IC_PLAY, PV_X + PV_W / 2 + 3, PV_Y + PV_H / 2, 26, C_DARK);
        live_pill(PV_X + 8, PV_Y + 8);
        viewers_pill(PV_X + 8, PV_Y + PV_H - 25, s->viewers, " espectadores");
        if (ui_click(PV_X, PV_Y, PV_W, PV_H)) c_act = 1;
    } else {
        gfx_rect_a(PV_X, PV_Y, PV_W, PV_H, 0x000000, 150);
        gfx_text_c(&font_bold, PV_X + PV_W / 2, PV_Y + PV_H / 2 - 9, "Sin transmisi\xc3\xb3n ahora", C_DIM);
    }
    /* informacion */
    int ix = PV_X + PV_W + 16, iw = SCR_W - ix - 16, iy = PV_Y;
    gfx_rrect(ix, iy, iw, PV_H, 10, C_PANEL, 255);
    gfx_rframe(ix, iy, iw, PV_H, 10, 1, C_LINE, 255);
    {
        char **ln; int n = ui_wrap(&font_bold, s->live > 0 ? (s->title[0] ? s->title : "(sin t\xc3\xadtulo)") : (chan_err[0] ? chan_err : "El canal no est\xc3\xa1 en vivo."), iw - 24, &ln), k;
        for (k = 0; k < n && k < 2; k++) text_bold(&font_bold, ix + 12, iy + 10 + k * 19, ln[k], C_TEXT);
        int yy = iy + 14 + (n < 2 ? n : 2) * 19;
        for (k = 0; k < n; k++) free(ln[k]);
        free(ln);
        int cx = ix + 12;
        if (s->cat[0]) cx += chip(cx, yy, s->cat, 0, iw - 30) + 6;
        if (s->lang[0] && cx < ix + iw - 60) cx += chip(cx, yy, s->lang, 0, ix + iw - cx - 12) + 6;
        if (s->mature && cx < ix + iw - 40) { gfx_rrect(cx, yy, 34, 20, 10, C_LIVE, 255); gfx_text(&font_small, cx + 7, yy + 2, "+18", 0xFFFFFF); }
        yy += 28;
        if (s->live > 0) {
            char since[40]; fmt_since(s->start, since, sizeof(since));
            fmt_count(s->viewers, v, sizeof(v));
            gfx_circle(ix + 16, yy + 8, 4, C_LIVE, 255);
            snprintf(line, sizeof(line), "%s espectadores%s%s", v, since[0] ? "  \xc2\xb7  " : "", since);
            gfx_text_fit(&font_small, ix + 26, yy, line, C_DIM, iw - 36);
            yy += 20;
        }
        if (s->bio[0]) {
            n = ui_wrap(&font_small, s->bio, iw - 24, &ln);
            for (k = 0; k < n && yy + 15 < iy + PV_H - 76; k++, yy += 15) gfx_text(&font_small, ix + 12, yy, ln[k], C_FAINT);
            for (k = 0; k < n; k++) free(ln[k]);
            free(ln);
        }
    }
    if(ui_button(ix+10,PV_Y+PV_H-64,152,24,social_label(s->slug),IC_HEART,BS_ACCENT,chan_focus==3&&!in_ptr.valid))c_act=6;
    if(ui_button(ix+170,PV_Y+PV_H-64,152,24,"Suscribirse",0,BS_NORMAL,chan_focus==4&&!in_ptr.valid))c_act=7;
    /* Compact controls leave room for real live thumbnails below the player. */
    int by=PV_Y+PV_H-34;
    if(s->live>0&&ui_button(ix+10,by,90,28,"Ver vivo",IC_PLAY,BS_ACCENT,chan_focus==0&&!in_ptr.valid))c_act=1;
    if(ui_button(ix+108,by,110,28,k_server[0]?"Modo":"Directo",0,BS_NORMAL,chan_focus==1&&!in_ptr.valid))c_act=2;
    if(ui_button(ix+226,by,96,28,"Volver",IC_BACK,BS_NORMAL,chan_focus==2&&!in_ptr.valid))c_act=3;
    gfx_text(&font_small,PV_X,370,"Otros directos (no son VODs)",C_DIM);
    draw_recs(s->slug,PV_X,390,SCR_W-32,56,1);
    gfx_text_fit(&font_small,PV_X,SCR_H-16,"A: ver   B: volver   2: actualizar   +: categorias",C_FAINT,SCR_W-32);
    int tb = topbar();
    if (tb) c_act = 100 + tb;
    ui_draw_toast(SCR_H - 30);
}

/* ---------------------------------------------------------------- pantalla completa */
static unsigned char rowsrc[SCR_H];
#define OSD_TOP 60
#define OSD_BOT (SCR_H - 52)
static void draw_vu(int x, int y, int w, int h) {
    float l = tp_audio_level();
    int n = 16, i, bw = (w - (n - 1) * 2) / n;
    for (i = 0; i < n; i++) {
        int on = l * n > i;
        gfx_rrect(x + i * (bw + 2), y, bw, h, 2, on ? (i > 12 ? C_WARN : C_ACCENT) : C_PANEL2, 255);
    }
}
static void draw_osd(void) {
    const kstream_t *c = &pinfo;
    char s[240], v[24];
    gfx_vgrad(0, 0, SCR_W, OSD_TOP, 0x000000, gfx_mix(0x000000, C_BG2, 120));
    gfx_rect(0, OSD_BOT, SCR_W, SCR_H - OSD_BOT, C_BG2);
    gfx_rect(0, OSD_BOT, SCR_W, 1, 0x000000);
    avatar(c, c->avatar, 14, 12, 36, tp_state() == TP_PLAYING);
    text_bold(&font_bold, 62, 10, c->name, C_TEXT);
    int nw = gfx_text_w(&font_bold, c->name) + 1;
    if (tp_state() == TP_PLAYING) live_pill(62 + nw + 10, 11);
    gfx_text_fit(&font_small, 62, 32, c->title, C_DIM, 470);
    fmt_count(c->viewers, v, sizeof(v));
    gfx_circle(SCR_W - 96, 19, 4, C_LIVE, 255);
    gfx_text(&font_small, SCR_W - 88, 11, v, C_TEXT);
    wordmark(SCR_W - 104, 26);
    if (tp_state() == TP_ERROR) snprintf(s, sizeof(s), "Se cort\xc3\xb3: %s", tp_error());
    else snprintf(s, sizeof(s), "%s \xc2\xb7 %s \xc2\xb7 %.0f fps \xc2\xb7 vol %d%%", tp_status(), k_server[0] ? pm_names[k_play_mode] : tp_mode_text(), tp_fps(), tp_volume() * 100 / 255);
    gfx_text_fit(&font_bold, 16, OSD_BOT + 8, s, C_TEXT, SCR_W-40);
    gfx_text_fit(&font_small, 16, OSD_BOT + 30, "B: volver  +: pantalla  1: escribir  abajo: responder/moderar  -: seguir  2: suscribir", C_FAINT, SCR_W-310);
    if(ui_button(SCR_W-280,OSD_BOT+26,132,24,social_label(pinfo.slug),IC_HEART,BS_ACCENT,0))watch_social_pressed=1;
    if(ui_button(SCR_W-140,OSD_BOT+26,132,24,"Suscribirse",0,BS_NORMAL,0))watch_social_pressed=2;
    ui_draw_toast(OSD_BOT - 6);
}

#define CHAT_X (gfx_widescreen()?SCR_W-240:448)
#define CHAT_W (SCR_W-CHAT_X)
static void draw_chat_panel(void) {
    static tchat_t msgs[24];
    int n = chat_get(msgs, 24), i, k;
    chat_anim_begin();   /* rc14: la lista de emotes animados se arma de nuevo */
    gfx_rect(CHAT_X, 0, CHAT_W, SCR_H, C_BG2);
    gfx_rect(CHAT_X, 0, 1, SCR_H, 0x000000);
    text_bold(&font_small, CHAT_X + 10, 10, "CHAT DEL DIRECTO", C_TEXT);
    gfx_rect(CHAT_X, 34, CHAT_W, 1, C_LINE);
    int y = SCR_H - 138;
    const char *me = auth_ready() ? auth_login() : "";
    char at_me[72]; snprintf(at_me, sizeof(at_me), "@%s", me);
    for (i = n - 1; i >= 0 && y > 40; i--) {
        char buf[260];
        int nl_name = strlen(msgs[i].name);
        int rich = !msgs[i].sys && msgs[i].ne;   /* rc13: emotes como imagenes */
        if (msgs[i].sys) snprintf(buf, sizeof(buf), "%s", msgs[i].text);
        else snprintf(buf, sizeof(buf), "%s: %s", msgs[i].name, msgs[i].text);
        char **ln = NULL; int nl = 0;
        int hgt;
        if (rich) hgt = chat_rich(&font_small, CHAT_X + 9, 0, CHAT_W - 18, &msgs[i], msgs[i].color, C_TEXT, 0) + 4;
        else { nl = ui_wrap(&font_small, buf, CHAT_W - 18, &ln); hgt = nl * 15 + 4; }
        if (y - hgt < 40) { for (k = 0; k < nl; k++) free(ln[k]); free(ln); break; }
        y -= hgt;
        /* resaltado: mis mensajes (verde) y menciones a mi (ambar) */
        int mine = me[0] && !msgs[i].sys && !strcasecmp(msgs[i].name, me);
        int ment = me[0] && !msgs[i].sys && !mine && strcasestr(msgs[i].text, at_me);
        if (mine || ment) {
            gfx_rect(CHAT_X + 3, y - 1, CHAT_W - 6, hgt - 1, mine ? 0x173A12 : 0x3A2C0E);
            gfx_rect(CHAT_X + 3, y - 1, 3, hgt - 1, mine ? C_ACCENT : C_WARN);
        }
        if (rich) { chat_rich(&font_small, CHAT_X + 9, y, CHAT_W - 18, &msgs[i], msgs[i].color, C_TEXT, 1); continue; }
        for (k = 0; k < nl; k++) {
            int yy = y + k * 15;
            if (msgs[i].sys) gfx_text(&font_small, CHAT_X + 9, yy, ln[k], msgs[i].color ? msgs[i].color : C_FAINT);
            else if (k == 0 && nl_name && !strncmp(ln[k], msgs[i].name, nl_name)) {
                /* nombre en su color (negrita) + el resto en blanco */
                text_bold(&font_small, CHAT_X + 9, yy, msgs[i].name, msgs[i].color);
                gfx_text(&font_small, CHAT_X + 10 + gfx_text_w(&font_small, msgs[i].name), yy, ln[k] + nl_name, C_TEXT);
            } else gfx_text(&font_small, CHAT_X + 9, yy, ln[k], C_TEXT);
        }
        for (k = 0; k < nl; k++) free(ln[k]);
        free(ln);
    }
    if (!n) {
        char **ln;int nn=ui_wrap(&font_small,chat_status()[0]?chat_status():"Esperando mensajes...",CHAT_W-20,&ln);
        for(i=0;i<nn;i++){gfx_text(&font_small,CHAT_X+10,190+i*16,ln[i],C_FAINT);free(ln[i]);}free(ln);
    }
    if(ui_button(CHAT_X+8,SCR_H-130,CHAT_W-16,26,CHAT_W<240?"Resp. / moderar":"Responder / moderar",IC_CHAT,BS_NORMAL,0))chat_actions_pressed=1;
    if(ui_button(CHAT_X+8,SCR_H-98,CHAT_W-16,26,social_label(pinfo.slug),IC_HEART,BS_ACCENT,0))watch_social_pressed=1;
    if(ui_button(CHAT_X+8,SCR_H-66,CHAT_W-16,26,"Suscribirse",0,BS_NORMAL,0))watch_social_pressed=2;
    if(ui_button(CHAT_X+8,SCR_H-34,CHAT_W-16,26,"Escribir",IC_CHAT,BS_ACCENT,0))chat_write_pressed=1;
}
static int fs_osd_was = -1,fs_details_was=-1,last_chat_gen=-1;
static u64 fs_last_osd = 0, fs_last_full = 0;
static double fs_last_pts = -1;
static const tv_pic_t *fs_last_pic = NULL;
static int watch_frame(int input) {
    const tv_pic_t *pic = curpic;
    int wide = gfx_widescreen();
    if(fs_details_was!=watch_details){fs_details_was=watch_details;fs_last_pic=NULL;fs_last_osd=0;fs_osd_was=-1;input=1;}
    if(watch_details) {

        int vw=CHAT_X,vh=vw*9/16,has=pic&&tp_has_video();u64 now=gettime();
        int ms=ticks_to_millisecs(now-fs_last_osd),g=chat_gen();
        int dirty=input||(g!=last_chat_gen&&ms>=250)||ms>=(has?1000:100);
        if(dirty) {
            gfx_clear(C_BG);gfx_rect(0,0,vw,vh,0);
            if(!has) {
                const kimg_t *im=thumb_get(pinfo.thumb);
                if(im){gfx_blit_scaled(im->px,im->w,im->h,0,0,vw,vh);gfx_rect_a(0,0,vw,vh,0,150);}
                gfx_text_fit(&font_small,12,vh/2,tp_state()==TP_ERROR?tp_error():tp_status(),C_TEXT,vw-24);
            }
            draw_chat_panel();last_chat_gen=g;
            avatar(&pinfo,pinfo.avatar,12,vh+10,36,1);
            gfx_text_fit(&font_bold,60,vh+8,pinfo.name,C_TEXT,CHAT_X-74);
            gfx_text_fit(&font_small,60,vh+30,pinfo.title,C_DIM,CHAT_X-74);
            char st[180];snprintf(st,sizeof(st),"%s / %.0f fps / vol %d%%",tp_status(),tp_fps(),tp_volume()*100/255);
            if(!wide)gfx_text_fit(&font_small,12,vh+62,st,C_DIM,CHAT_X-24);
            /* rc8: sin botones extra en la columna del video (ya estan en el chat).
               Se recupera el espacio de las recomendaciones; el hint corto queda entero. */
            {
                int sy = vh + (wide ? 66 : 108);
                if (!wide) gfx_text(&font_small, 12, sy - 20, "Otros directos", C_DIM);
                draw_recs(pinfo.slug, 12, sy, CHAT_X - 24, wide ? 32 : 56, 0);
                gfx_text_fit(&font_small, 12, SCR_H - 16,
                    "B:canal +/-:vol +:full 1:escribir abajo:responder -:seguir 2:suscribir",
                    C_FAINT, CHAT_X - 24);
            }
            gfx_ui_cache_rows(0,SCR_H);fs_last_osd=now;
        }
        /* rc14: emotes animados: sólo sus filas, sin redibujar el panel (<=12,5 cuadros/s) */
        int anim=!dirty&&chat_anim_tick()>0;
        int newpic=has&&(pic!=fs_last_pic||pic->pts!=fs_last_pts);
        if(!newpic&&!dirty&&!anim)return 0;
        if(has){fs_last_pic=pic;fs_last_pts=pic->pts;}
        memset(rowsrc,1,sizeof(rowsrc));gfx_composite(has?pic:NULL,0,0,vw,vh,rowsrc);return 1;
    }
    int vw = SCR_W, vh = wide ? SCR_H : 360, vy = (SCR_H - vh) / 2;
    chat_anim_reset();   /* rc14: chat oculto: nada que animar */
    int osd = ticks_to_millisecs(gettime() - osd_t) < 5000 || tp_state() != TP_PLAYING;
    u64 now = gettime();
    if (!pic || !tp_has_video()) {
        if (!input && ticks_to_millisecs(now - fs_last_full) < 33) return 0;
        fs_last_full = now;
        gfx_clear(0x000000);
        const kimg_t *th = thumb_get(pinfo.thumb);
        if (th) { gfx_blit_scaled(th->px, th->w, th->h, 0, 60, SCR_W, 360); gfx_rect_a(0, 60, SCR_W, 360, 0, 150); }
        if (tp_state() == TP_PLAYING && !tp_has_video()) {
            gfx_text_c(&font_bold, SCR_W / 2, 230, "Solo audio", C_DIM);
            draw_vu(SCR_W / 2 - 120, 260, 240, 14);
        } else if (tp_state() != TP_ERROR) {
            ui_spinner(SCR_W / 2, 230, 18);
            gfx_text_c(&font_small, SCR_W / 2, 262, tp_status(), C_DIM);
        }
        draw_osd();
        gfx_ui_cache_rows(0, SCR_H);
        gfx_composite(NULL, 0, 0, 0, 0, NULL);
        fs_osd_was = -1;
        return 1;
    }
    int osd_dirty = 0;
    if (osd && (osd != fs_osd_was || input || ticks_to_millisecs(now - fs_last_osd) >= 250)) {
        gfx_rect(0, 0, SCR_W, OSD_TOP, 0); gfx_rect(0, OSD_BOT, SCR_W, SCR_H - OSD_BOT, 0);
        draw_osd();
        gfx_ui_cache_rows(0, OSD_TOP);
        gfx_ui_cache_rows(OSD_BOT, SCR_H);
        fs_last_osd = now; osd_dirty = 1;
    }
    if (osd != fs_osd_was) osd_dirty = 1;
    fs_osd_was = osd;
    int newpic = pic != fs_last_pic || pic->pts != fs_last_pts;
    if (!newpic && !osd_dirty) return 0;
    fs_last_pic = pic; fs_last_pts = pic->pts;
    memset(rowsrc, 0, sizeof(rowsrc));
    if (osd) { memset(rowsrc, wide ? 2 : 1, OSD_TOP); memset(rowsrc + OSD_BOT, wide ? 2 : 1, SCR_H - OSD_BOT); }
    gfx_composite(pic, 0, vy, vw, vh, rowsrc);
    return 1;
}

/* ---------------------------------------------------------------- ajustes */
static const char *help_text =
    "CONTROLES\n"
    "Puntero + A: elegir tarjeta, chip o bot\xc3\xb3n.  Cruceta: moverse por la grilla.\n"
    "1: buscar (teclado en pantalla).  2: actualizar.  -: cambiar idioma.  B: ajustes / volver.\n"
    "Viendo un directo: B vuelve al canal, Izq/Der volumen, A muestra la barra, + cambia pantalla completa/chat, -: seguir, 2: suscribirse.  HOME: salir.\n"
    "Tambi\xc3\xa9n sirven el Classic Controller, el mando de GameCube y un teclado USB.\n\n"
    "EXPLORAR CATEGORIAS\n"
    "+ o Cat.: catalogo de Kick. Cruceta: scroll; A: directos de esa categoria. 1: buscar entre las categorias cargadas; -: grupos reales; 2: actualizar.\n"
    "Las portadas se cargan solo al verlas. El proveedor limita la cantidad de paginas: la barra informa el total disponible y cualquier carga parcial.\n\n"
    "SERVIDOR COMPANION (recomendado)\n"
    "En una PC de tu misma red corr\xc3\xa9: python3 kickwii_server.py  y pon\xc3\xa9 su IP en Ajustes > Servidor "
    "(se guarda en sd:/apps/KickWii/server.txt). Con el servidor ten\xc3\xa9s miniaturas, avatares, "
    "video por HTTP sin TLS (Proxy 360p) o transcodificado a 270p con ffmpeg.\n\n"
    "SIN SERVIDOR\n"
    "La Wii descarga miniaturas, avatares, categorias y banners por HTTPS. Video hasta 360p; chat publico WSS de solo lectura, sin listado VOD. "
    "Tolera minutos de diferencia del reloj; conserva la fecha aproximada correcta.\n\n"
    "FILTRO\n"
    "Por defecto se ocultan los canales +18 y las categor\xc3\xad" "as de apuestas/casino.";

static const char *about_text =
    "KickWii " TV_VERSION " - cliente NO OFICIAL de kick.com hecho por fans.\n\n"
    "NO est\xc3\xa1 afiliado, autorizado ni respaldado por Kick (Kick Streaming Pty Ltd) ni por Nintendo. "
    "\"Kick\" es marca de sus due\xc3\xb1os y se usa solo para decir con qu\xc3\xa9 servicio funciona. "
    "El \xc3\xad" "cono es arte propio.\n\n"
    "Solo muestra transmisiones en vivo p\xc3\xba" "blicas y gratuitas, igual que kick.com sin iniciar sesi\xc3\xb3n. "
    "Tiene login OAuth opcional para escribir; no accede a contenido pago. Los contenidos pertenecen a sus creadores.\n\n"
    "M\xc3\xbasica de la intro: \"Clair de lune\" (Suite bergamasque) de Claude Debussy, piano: Laurens Goedhart (2011), "
    "dominio p\xc3\xbablico (Public Domain Mark), v\xc3\xad" "a Wikimedia Commons. Recortada a 45 s y comprimida.\n\n"
    "Software: FFmpeg/libavcodec/libavformat (LGPL 2.1), libcurl, mbedTLS, zlib, libogc, fuente DejaVu Sans. "
    "Imagen de carga (intro y banner del canal): la eligi\xc3\xb3 el usuario (wordmark KICK pixelado sobre negro).";

static void edit_server(void) {
    char b[80];
    const char *s = k_server;
    if (!strncmp(s, "http://", 7)) s += 7;
    snprintf(b, sizeof(b), "%s", s);
    if (!sui_text_input("Servidor companion", "IP de la PC con kickwii_server.py, ej: 192.168.0.10 8787 (espacio = ':'). Vac\xc3\xad" "o = directo a kick.com", b, sizeof(b) - 1)) return;
    char *p = b; while (*p == ' ') p++;
    int n = strlen(p); while (n && p[n - 1] == ' ') p[--n] = 0;
    char *sp = strchr(p, ' '); if (sp) { *sp = ':'; }
    if (!*p) k_server[0] = 0;
    else snprintf(k_server, sizeof(k_server), "http://%s%s", p, strchr(p, ':') ? "" : ":8787");
    cfg_save();
    thumbs_flush();
    check_server();
    load_live(1);
}


static const char *display_title(void) {
    return k_lang==2?"Screen type":k_lang==3?"Tipo de tela":"Tipo de pantalla";
}
static const char *display_choice(int mode) {
    static const char *es[]={"Automático (según la Wii)","4:3 (pantalla cuadrada)","16:9 (pantalla plana/panorámica)"};
    static const char *en[]={"Automatic (Wii setting)","4:3 (square screen)","16:9 (flat/widescreen)"};
    static const char *pt[]={"Automático (conforme o Wii)","4:3 (tela quadrada)","16:9 (tela plana/panorâmica)"};
    if(mode<0||mode>2)mode=0;
    return k_lang==2?en[mode]:k_lang==3?pt[mode]:es[mode];
}
static void display_settings(void) {
    int choice=gfx_aspect_mode();
    char *items[3]={(char *)display_choice(0),(char *)display_choice(1),(char *)display_choice(2)};
    u32 r=sui_list(display_title(),k_lang==2?"A: apply   B: back":k_lang==3?"A: aplicar   B: voltar":"A: aplicar   B: volver",items,3,&choice);
    if(r==WPAD_BUTTON_HOME){quit=1;return;}
    if(r!=WPAD_BUTTON_A)return;
    gfx_set_aspect(choice);cfg_save();
    sel=scroll=scroll_t=bsel=0;
}

static void settings(void) {
    int sel_i = 0;
    while (!quit) {
        char it[10][160];
        char *items[10];
        int i;
        snprintf(it[0], 160, "Servidor companion\n%s", k_server[0] ? k_server : "ninguno: miniaturas directas (si Kick permite)");
        snprintf(it[1], 160, "Modo de video\n%s", k_server[0] ? pm_names[k_play_mode] : "Directo (sin servidor, 360p HTTPS)");
        snprintf(it[2], 160, "Idioma de la lista\n%s", k_lang_names[k_lang]);
        snprintf(it[3], 160, "Filtro +18 / apuestas\n%s", k_show_mature ? "Desactivado (se muestra todo)" : "Activado (recomendado)");
        snprintf(it[4], 160, "Probar conexi\xc3\xb3n\n%s", !k_server[0] ? "modo directo" : (k_server_ok ? "\xc3\xbaltima prueba: OK" : "\xc3\xbaltima prueba: sin respuesta"));
        snprintf(it[5], 160, "Ayuda");
        snprintf(it[6], 160, "Acerca de (no oficial)");
        snprintf(it[7], 160, "Salir al Homebrew Channel");
        snprintf(it[8], 160, "%s\n%s", display_title(), display_choice(gfx_aspect_mode()));
        snprintf(it[9],160,"Cuenta / Iniciar sesion\n%s",auth_status());
        items[9]=it[9];
        items[0]=it[8];
        for (i=0;i<8;i++)items[i+1]=it[i];
        u32 r = sui_list("Ajustes", "A: elegir   B: volver", items, 10, &sel_i);
        if (r == WPAD_BUTTON_HOME) { quit = 1; return; }
        if (r != WPAD_BUTTON_A) return;
        switch (sel_i==9?9:sel_i==0?8:sel_i-1) {
        case 9: chat_stop();if(auth_has_session())account_screen();else{auth_screen();if(auth_ready())account_screen();}thumbs_pause(0);break;
        case 8: display_settings(); break;
        case 0: edit_server(); break;
        case 1: k_play_mode = (k_play_mode + 1) % PM_N; cfg_save(); break;
        case 2: next_language(); load_live(1); break;
        case 3:
            if (!k_show_mature) {
                if (!sui_confirm("Filtro", "\xc2\xbfMostrar canales +18 y de apuestas/casino? Solo para adultos.")) break;
                k_show_mature = 1;
            } else k_show_mature = 0;
            cat_sel = sel = scroll = scroll_t = 0;
            cfg_save(); rebuild_filter(); break;
        case 4: check_server(); if (k_server_ok) ui_toast("Servidor OK"); break;
        case 5: sui_text_view("Ayuda", "A/B: volver", help_text); break;
        case 6: sui_text_view("Acerca de", "A/B: volver", about_text); break;
        case 7: quit = 1; return;
        }
    }
}

/* ---------------------------------------------------------------- prueba automatica (Dolphin) */
#ifdef AUTOTEST
static u64 at_ts = 0;
static int at_step = 0;
static int at_ms(void) { return (int)ticks_to_millisecs(gettime() - at_ts); }
static void at_stats(void) {
    static u64 last = 0;
    {   /* rc14/rc8: memoria cada 10 s (mallinfo + arenas + imagenes/animaciones) */
        static u64 mlast = 0; static int pc0 = 0; static unsigned r0 = 0, t0a = 0;
        if (atlog && (!mlast || ticks_to_millisecs(gettime() - mlast) >= 10000)) {
            char mm[300]; unsigned rr, tt; thumbs_mem(mm, sizeof mm); chat_anim_stats(&rr, &tt);
            int el = mlast ? (int)ticks_to_millisecs(gettime() - mlast) : 1;
            LOG("mem: t=%us %s | vis_anim=%d anim_ticks/s=%.1f anim_px/s=%.0f presents/s=%.1f video_fps=%.1f screen=%d",
                (unsigned)ticks_to_secs(gettime() - t_start), mm, chat_anim_count(), (tt - t0a) * 1000.0 / el, (rr - r0) * 1000.0 / el,
                (gfx_present_count - pc0) * 1000.0 / el, tp_fps(), screen);
            mlast = gettime(); pc0 = gfx_present_count; r0 = rr; t0a = tt;
        }
    }
    if (!atlog || screen != SCR_WATCH || ticks_to_millisecs(gettime() - last) < 2000) return;
    char st[200]; tp_stats(st, sizeof(st));
    last = gettime();
    tchat_t msgs[1];int n=chat_get(msgs,1);
    LOG("watch: state=%d %s | frames=%d audio=%d video=%d | %s | chat=[%s] %s: %s | err=%s",tp_state(),tp_status(),tp_frames(),tp_has_audio(),tp_has_video(),st,chat_status(),n?msgs[0].name:"",n?msgs[0].text:"",tp_error());
}
static void at_next(void) { at_step++; at_ts = gettime(); LOG("step %d (screen %d)", at_step, screen); }
static void at_ptr(int x, int y) { in_set_ptr(x, y, 1); }
static void at_type(const char *s) { while (*s) in_inject_key(*s++); }
static int at_wait(int scr, int ms) { return screen == scr && at_ms() > ms; }
u32 at_hook(void) {
    u32 b = 0;
    if (!at_ts) at_ts = gettime();
    if (at_block) { at_ts = gettime(); return 0; }   /* el tiempo de espera cuenta desde que termina el pedido */
#ifdef AUTOTEST_LANGUAGE
    static int phase=0, cycles=0, moves=0;
    if (phase==0 && live_loaded && at_ms()>15000) {
        in_set_ptr(MX+16+gfx_text_w(&font_bold,"Canales en vivo")+24,HDR_Y+10,1); phase=1; at_next();
    } else if (phase==1 && at_ms()>600) {
        LOG("LANG click %d before=%s n=%d filter=%d",cycles+1,k_langs[k_lang],live.n,nflt);
        b=(cycles%2) ? WPAD_BUTTON_MINUS : WPAD_BUTTON_A;
        phase=2;at_next();
    } else if (phase==2 && at_ms()>8000) {
        LOG("LANG stable %d language=%s n=%d filter=%d pending=%d error=%s",cycles+1,k_langs[k_lang],live.n,nflt,thumbs_pending(),live_err);
        in_set_ptr(0,0,0);phase=3;moves=0;at_next();
    } else if (phase==3 && at_ms()>320) {
        b=WPAD_BUTTON_DOWN;at_next();if(++moves==8){phase=4;moves=0;}
    } else if (phase==4 && at_ms()>320) {
        b=WPAD_BUTTON_UP;at_next();if(++moves==8){phase=5;}
    } else if (phase==5 && at_ms()>1800) {
        if(++cycles>=12){phase=6;b=WPAD_BUTTON_2;at_next();}
        else {phase=1;in_set_ptr(MX+16+gfx_text_w(&font_bold,"Canales en vivo")+24,HDR_Y+10,1);at_next();}
    } else if (phase==6 && at_ms()>8000) {
        LOG("LANG PASS: 12 changes, 3 complete cycles, 192 scroll inputs, refresh; language=%s n=%d filter=%d",k_langs[k_lang],live.n,nflt);
        quit=1;at_next();
    }
#elif defined(AUTOTEST_PICKER)
    /* rc14: selector de emotes en el teclado del chat; el envio es SEND_DRYRUN (no sale nada) */
    switch(at_step){
    case 0: if(at_wait(SCR_HOME,8000)){at_next();open_channel(NULL,AT_CHAN);}break;
    case 1: if(at_wait(SCR_CHAN,6000)){at_next();start_watch();}break;
    case 2: if(at_wait(SCR_WATCH,25000)){at_next();LOG("picker: abrir teclado");at_ptr(6,474);b=WPAD_BUTTON_1;}break;
    case 3: if(at_ms()>3000){at_next();at_type("hola ");}break;
    case 4: if(at_ms()>1500){at_next();LOG("picker: tecla 2 -> grilla");b=WPAD_BUTTON_2;}break;
    case 5: if(at_ms()>14000){at_next();LOG("picker: shot grilla canal (t=%.0f)",ticks_to_millisecs(gettime()-t_start)/1000.0);}break;
    case 6: if(at_ms()>4000){at_next();b=WPAD_BUTTON_PLUS;LOG("picker: + -> grupo siguiente");}break;
    case 7: if(at_ms()>1500){at_next();b=WPAD_BUTTON_PLUS;}break;
    case 8: if(at_ms()>9000){at_next();LOG("picker: shot grilla globales (t=%.0f)",ticks_to_millisecs(gettime()-t_start)/1000.0);}break;
    case 9: if(at_ms()>3000){at_next();b=WPAD_BUTTON_RIGHT;}break;
    case 10: if(at_ms()>600){at_next();at_ptr((SCR_W-600)/2+20+3*50+24,30+102+50+24);LOG("picker: puntero sobre la celda 4 de la fila 2");}break;
    case 11: if(at_ms()>1200){at_next();b=WPAD_BUTTON_A;LOG("picker: A (puntero) -> insertar");}break;
    case 12: if(at_ms()>2500){at_next();at_ptr(6,474);at_type("y ");}break;
    case 13: if(at_ms()>1500){at_next();b=WPAD_BUTTON_2;}break;
    case 14: if(at_ms()>5000){at_next();b=WPAD_BUTTON_MINUS;}break;   /* grupo anterior: Canal */
    case 15: if(at_ms()>4000){at_next();b=WPAD_BUTTON_A;}break;
    case 16: if(at_ms()>4000){at_next();LOG("picker: shot vista previa (t=%.0f)",ticks_to_millisecs(gettime()-t_start)/1000.0);}break;
    case 17: if(at_ms()>4000){at_next();b=WPAD_BUTTON_PLUS;LOG("picker: + -> Listo");}break;
    case 18: if(at_ms()>2500){at_next();b=WPAD_BUTTON_A;LOG("picker: A -> Enviar (dry run)");}break;
    case 19: if(at_ms()>5000){at_next();LOG("picker end status=%s",auth_status());quit=1;}break;
    }
#elif defined(AUTOTEST_SOAK)
    /* rc14/rc8: soak de memoria: canal con chat real + 3 emotes animados distintos cada 2,5 s */
    switch(at_step){
    case 0: if(at_wait(SCR_HOME,8000)){at_next();open_channel(NULL,AT_CHAN);}break;
    case 1: if(at_wait(SCR_CHAN,6000)){at_next();start_watch();}break;
    case 2: if(at_wait(SCR_WATCH,2500)){static int k=0;static u64 s0=0;if(!s0)s0=gettime();at_ts=gettime();chat_test_soak(k++);
             if(ticks_to_millisecs(gettime()-s0)>=(u64)AT_SOAK_MIN*60000){LOG("soak end k=%d",k);at_next();quit=1;}}break;
    }
#elif defined(AUTOTEST_EMOTES)
    switch(at_step){
    case 0: if(at_wait(SCR_HOME,8000)){at_next();open_channel(NULL,AT_CHAN);}break;
    case 1: if(at_wait(SCR_CHAN,6000)){at_next();start_watch();}break;
    case 2: if(at_wait(SCR_WATCH,6000)){at_next();chat_test_inject();LOG("emotes: mensajes de prueba inyectados");}break;
    case 3: if(at_wait(SCR_WATCH,12000)){static int k=0;at_ts=gettime();chat_test_inject();if(++k>=12){at_next();quit=1;}}break;
    }
#else
    switch(at_step){
    case 0: if(at_wait(SCR_HOME,10000)){at_next();open_channel(NULL,"samulx");}break;
    case 1: if(at_wait(SCR_CHAN,9000)){at_next();start_watch();}break;
    case 2: if(at_wait(SCR_WATCH,35000)){b=WPAD_BUTTON_PLUS;at_next();}break;
    case 3: if(at_wait(SCR_WATCH,3000)){b=WPAD_BUTTON_PLUS;at_next();}break;
    case 4: if(at_wait(SCR_WATCH,35000)){LOG("autotest rc3 end (chat reconectado)");at_next();quit=1;}break;
    }
#endif
    return b;
}
#endif

/* ---------------------------------------------------------------- principal */
static int last_px = -1, last_py = -1, last_pv = 0, last_screen = -1;
static u64 last_full = 0;

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    kickwii_mem2_init();
    gfx_init();
    input_init();
    fat_ok = fatInitDefault();
    tp_init();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (fat_ok) cfg_load();
    t_start = gettime();
    dlog_init(fat_ok);
    dlog("KickWii " TV_VERSION " | IOS %d | pantalla %s | SD %s", IOS_GetVersion(), gfx_widescreen() ? "16:9" : "4:3", fat_ok ? "ok" : "no");
    {
        extern unsigned char __bss_end[];
        u32 a1 = (u32)SYS_GetArena1Hi() - (u32)SYS_GetArena1Lo();
        u32 a2 = (u32)SYS_GetArena2Hi() - (u32)SYS_GetArena2Lo();
        dlog("MEM1 __bss_end %p arena1 %lu KB | MEM2 TX %08lx-%08lx (%lu KB) canvas %p arena2 %lu KB",
             (void *)__bss_end, (unsigned long)(a1/1024),
             (unsigned long)kickwii_mem2_bss_start(), (unsigned long)kickwii_mem2_bss_end(),
             (unsigned long)(kickwii_mem2_bss_size()/1024),
             (void *)canvas, (unsigned long)(a2/1024));
    }
#ifdef AUTOTEST
    mkdir("sd:/kickwii-test", 0777);
    atlog = fopen("sd:/kickwii-test/autotest.log", "w");
    LOG("autotest start server=%s", k_server);
#endif
    auth_init();
    thumbs_init();
    ui_set_backdrop(draw_home);
    rebuild_filter();
    int net = intro_run();          /* intro con la imagen de carga + Clair de lune; la red se inicia en paralelo */
    if (!net) ui_toast("La red no est\xc3\xa1 lista; toc\xc3\xa1 Actualizar para reintentar.");
    else {
        check_server();
        if(auth_due())auth_ensure();
        load_live(1);
    }
    while (SYS_MainLoop() && !quit) {
        dlog_flush(0);
        if(auth_due()){chat_stop();thumbs_pause(1);auth_ensure();if(screen==SCR_WATCH&&watch_chat_started)chat_start(&pinfo);if(screen!=SCR_WATCH)thumbs_pause(0);}

        u32 d = in_poll();
        int k;
        while ((k = in_key()) != 0) {
            if (k == 13) d |= WPAD_BUTTON_A;
            else if (k == 27) d |= WPAD_BUTTON_B;
            else if (k == 's' || k == '/') d |= WPAD_BUTTON_1;
        }
        if (d & WPAD_BUTTON_HOME) break;
        intro_music_tick();
        ui_begin(d);
        h_act = HA_NONE; s_act = 0; c_act = 0; b_act=0;rec_pick=-1;sb_pick = NULL;

        if (screen == SCR_WATCH) {
#ifdef AUTOTEST
            at_stats();
#endif
            ui_set_backdrop(NULL);
            thumbs_pause(1);

            if (d) osd_t = gettime();
            if(d&WPAD_BUTTON_PLUS){watch_details = !watch_details;
                /* + changes presentation, never tears down a running worker. */
                if (watch_details && !watch_chat_started) {
                    watch_chat_started = 1; chat_start(&pinfo);
                }}
            curpic = tp_picture();
            int wrote = watch_frame(d != 0);
            if(watch_social_pressed||(d&(WPAD_BUTTON_MINUS|WPAD_BUTTON_2))){int sub=watch_social_pressed?watch_social_pressed==2:!!(d&WPAD_BUTTON_2);watch_social_pressed=0;chat_stop();social_screen(pinfo.slug,sub);if(watch_chat_started)chat_start(&pinfo);thumbs_pause(1);fs_details_was=fs_osd_was=-1;fs_last_pic=NULL;osd_t=gettime();curpic=NULL;continue;}
            if(chat_actions_pressed||(d&WPAD_BUTTON_DOWN)){chat_actions_pressed=0;if(!watch_chat_started){watch_chat_started=1;chat_start(&pinfo);ui_toast("Abriendo el chat: espera unos mensajes y volve a pulsar abajo.");}else if(!auth_ready()){chat_stop();auth_screen();if(watch_chat_started)chat_start(&pinfo);}else chat_actions(&pinfo);thumbs_pause(1);fs_details_was=fs_osd_was=-1;fs_last_pic=NULL;osd_t=gettime();curpic=NULL;continue;}
            if(chat_write_pressed||(d&WPAD_BUTTON_1)){chat_write_pressed=0;if(!auth_ready()){chat_stop();auth_screen();if(watch_chat_started)chat_start(&pinfo);}if(auth_ready()&&!watch_chat_started){watch_chat_started=1;chat_start(&pinfo);}if(auth_ready())auth_compose(pinfo.slug,pinfo.broadcaster_user_id);thumbs_pause(1);fs_details_was=fs_osd_was=-1;fs_last_pic=NULL;osd_t=gettime();curpic=NULL;continue;}

            if (!wrote) VIDEO_WaitVSync();
            if (d & WPAD_BUTTON_B) { stop_watch(); screen = SCR_CHAN; }
            if (d & WPAD_BUTTON_LEFT) { tp_set_volume(tp_volume() - 24); osd_t = gettime(); }
            if (d & WPAD_BUTTON_RIGHT) { tp_set_volume(tp_volume() + 24); osd_t = gettime(); }
            if (d & WPAD_BUTTON_A) osd_t = ticks_to_millisecs(gettime() - osd_t) < 5000 ? 0 : gettime();
            continue;
        }

        /* pantallas con interfaz: redibujar solo si hace falta */
        u64 now = gettime();
        int moved = abs(in_ptr.x - last_px) + abs(in_ptr.y - last_py) >= 2 || in_ptr.valid != last_pv;
        int ms = ticks_to_millisecs(now - last_full);
        int dirty = d || moved || scroll != scroll_t || screen != last_screen || ms >= 250;
        if (!dirty) { VIDEO_WaitVSync(); continue; }
        thumbs_frame();
        if (screen == SCR_HOME) { ui_set_backdrop(draw_home); draw_home(); }
        else if (screen == SCR_SEARCH) { ui_set_backdrop(draw_search); draw_search(); }
        else if(screen==SCR_BROWSE){ui_set_backdrop(draw_browse);draw_browse();}
        else { ui_set_backdrop(draw_channel); draw_channel(); }
        ui_present();
        last_full = now; last_px = in_ptr.x; last_py = in_ptr.y; last_pv = in_ptr.valid; last_screen = screen;
        if (UI.consumed) d &= ~WPAD_BUTTON_A;

        /* acciones comunes: barra superior / lateral */
        int tb = 0;
        if (h_act >= 100) tb = h_act - 100;
        if (s_act >= 100) tb = s_act - 100;
        if (c_act >= 100) tb = c_act - 100;
        if(b_act>=100)tb=b_act-100;
        if(tb==TB_BROWSE || (d&WPAD_BUTTON_PLUS)){browse_enter();continue;}
        if(rec_pick>=0){kstream_t r=live.v[rec_pick];open_channel(&r,r.slug);continue;}
        if (tb == TB_HOME) { screen = SCR_HOME; if(active_category.id){memset(&active_category,0,sizeof(active_category));load_live(1);} continue; }
        if (tb == TB_SEARCH || ((d & WPAD_BUTTON_1)&&screen!=SCR_BROWSE)) { do_search(); continue; }
        if (tb == TB_SETTINGS) { settings(); continue; }
        if (tb == TB_ACCOUNT) { chat_stop(); if (auth_has_session()) account_screen(); else { auth_screen(); if (auth_ready()) account_screen(); } thumbs_pause(0); continue; }
        if (sb_pick) { char sl[48]; snprintf(sl, sizeof(sl), "%s", sb_pick); int i; const kstream_t *f = NULL;
            for (i = 0; i < live.n; i++) if (!strcmp(live.v[i].slug, sl)) f = &live.v[i];
            open_channel(f, sl); continue; }

        if (screen == SCR_HOME) {
            int ncards = nflt + (live_more ? 1 : 0);
            switch (h_act) {
            case HA_OPEN: { const kstream_t *card = filtered_stream(h_arg); sel = h_arg; if (card) open_channel(card, card->slug); continue; }
            case HA_MORE: load_live(0); continue;
            case HA_LANG: next_language(); load_live(1); continue;
            case HA_REFRESH: load_live(1); continue;
            case HA_CAT: cat_sel = h_arg; sel = 0; scroll_t = 0; rebuild_filter(); continue;
            }
            if (ncards) {
                int os = sel;
                if (d & WPAD_BUTTON_LEFT)  sel = sel > 0 ? sel - 1 : 0;
                if (d & WPAD_BUTTON_RIGHT) sel = sel < ncards - 1 ? sel + 1 : sel;
                if (d & WPAD_BUTTON_UP)    sel = sel >= COLS ? sel - COLS : sel;
                if (d & WPAD_BUTTON_DOWN)  sel = sel + COLS < ncards ? sel + COLS : ncards - 1;
                if (os != sel || (d & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT | WPAD_BUTTON_UP | WPAD_BUTTON_DOWN))) { ensure_visible(); if (in_ptr.virt) in_ptr.valid = 0; }
                if ((d & WPAD_BUTTON_A) && !in_ptr.valid) {
                    if (sel < nflt) { const kstream_t *card = filtered_stream(sel); if (card) open_channel(card, card->slug); continue; }
                    load_live(0); continue;
                }
            }
            if (d & WPAD_BUTTON_2) load_live(1);
            if (d & WPAD_BUTTON_MINUS) { next_language(); load_live(1); }
            if (d & WPAD_BUTTON_B) settings();
        } else if(screen==SCR_BROWSE) {
            browse_move(d);
            if(b_act==1 || ((d&WPAD_BUTTON_A)&&!in_ptr.valid&&browse_n)){browse_open();continue;}
            if(b_act==2 || (d&WPAD_BUTTON_1)){browse_search();continue;}
            if(b_act==3 || (d&WPAD_BUTTON_MINUS)){browse_groups_dialog();continue;}
            if(b_act==4 || (d&WPAD_BUTTON_2)){kc_stop();browse_gen=-1;bsel=browse_top=0;browse_enter();continue;}
            if(d&WPAD_BUTTON_B)screen=SCR_HOME;
        } else if (screen == SCR_SEARCH) {
            int extra = valid_slug(search_q) ? 1 : 0, n = results.n + extra;
            if (d & WPAD_BUTTON_UP) { if (rsel > 0) rsel--; if (in_ptr.virt) in_ptr.valid = 0; }
            if (d & WPAD_BUTTON_DOWN) { if (rsel < n - 1) rsel++; if (in_ptr.virt) in_ptr.valid = 0; }
            if (!s_act && (d & WPAD_BUTTON_A) && !in_ptr.valid && n) { s_act = 1; s_arg = rsel; }
            if (s_act == 1) {
                if (extra && s_arg == 0) open_channel(NULL, search_q);
                else { kstream_t r = results.v[s_arg - extra]; open_channel(&r, r.slug); }
                continue;
            }
            if (d & WPAD_BUTTON_B) screen = SCR_HOME;
        } else if (screen == SCR_CHAN) {
            if (d & WPAD_BUTTON_LEFT) { chan_focus = chan_focus > (chan.live > 0 ? 0 : 1) ? chan_focus - 1 : chan_focus; if (in_ptr.virt) in_ptr.valid = 0; }
            if (d & WPAD_BUTTON_RIGHT) { chan_focus = chan_focus < 4 ? chan_focus + 1 : 4; if (in_ptr.virt) in_ptr.valid = 0; }
            if (!c_act && (d & WPAD_BUTTON_A) && !in_ptr.valid) c_act = chan_focus<3?chan_focus+1:chan_focus+3;
            if(c_act==6||c_act==7){chat_stop();social_screen(chan.slug,c_act==7);thumbs_pause(0);continue;}
            if (c_act == 1 && chan.live > 0) { start_watch(); continue; }
            if (c_act == 2) { if (k_server[0]) { k_play_mode = (k_play_mode + 1) % PM_N; cfg_save(); } else ui_toast("Configur\xc3\xa1 un servidor companion para elegir el modo."); }
            if (c_act == 3 || (d & WPAD_BUTTON_B)) { screen = prev_screen == SCR_SEARCH ? SCR_SEARCH : (prev_screen==SCR_BROWSE?SCR_BROWSE:SCR_HOME); continue; }
            if (d & WPAD_BUTTON_2) { kstream_t c = chan; open_channel(&c, c.slug); }
        }
    }
    kc_stop();chat_stop();tp_stop();dlog("salida");dlog_flush(1);
    if (atlog) fclose(atlog);
    gfx_clear(C_BG);
    wordmark(SCR_W / 2 - 50, 200);
    gfx_text_c(&font_bold, SCR_W / 2, 250, "Saliendo...", C_DIM);
    gfx_present();
    return 0;
}
