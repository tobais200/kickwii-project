/*
 * kickapi.c - KickWii: modelo de datos y los dos "backends":
 *   1) servidor companion en la LAN (recomendado): JSON compacto, miniaturas RGB,
 *      video por HTTP plano (proxy 360p) o transcodificado (270p) -> menos trabajo para la Wii
 *   2) directo a kick.com (sin PC): API web publica, HTTPS en la Wii, sin miniaturas
 * Solo lectura de datos publicos. Cada pedido lo dispara una accion del usuario.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "tv.h"
#include "catalog.h"
#include <curl/curl.h>

char k_server[128] = "";
int  k_server_ok = 0;
int k_chat_on=1;
int  k_play_mode = PM_PROXY;
int  k_lang = 0;
int  ui_lang_explicit = 0;   /* interface language chosen by the user (or loaded from SD/USB) */
/* Stream (broadcast) language filter. Not the interface language (see i18n.c). */
const char *k_langs[] = { "es", "all", "en", "pt", "fr", "de", "it", "nl", "ja", "ko", "zh" };
static const char *k_lang_native[] = { "Espa\xc3\xb1ol", NULL, "English", "Portugu\xc3\xaas", "Fran\xc3\xa7" "ais", "Deutsch", "Italiano", "Nederlands", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4", "\xe4\xb8\xad\xe6\x96\x87" };
const char *k_lang_name(int i) {
    if (i < 0 || i >= (int)(sizeof(k_langs) / sizeof(k_langs[0]))) i = 0;
    return k_lang_native[i] ? k_lang_native[i] : T("Todos");
}
/* First run (no idioma.txt yet): follow the interface language. */
void k_lang_default_from_ui(void) {
    switch (ui_lang) {
    case LANG_ES: k_lang = 0; break;
    case LANG_PT: k_lang = 3; break;
    case LANG_FR: k_lang = 4; break;
    case LANG_DE: k_lang = 5; break;
    case LANG_IT: k_lang = 6; break;
    case LANG_NL: k_lang = 7; break;
    case LANG_JA: k_lang = 8; break;
    case LANG_KO: k_lang = 9; break;
    case LANG_ZH: case LANG_ZT: k_lang = 10; break;
    default:      k_lang = 2; break;   /* English */
    }
}
int  k_nlangs = sizeof(k_langs) / sizeof(k_langs[0]);
_Static_assert(sizeof(k_langs) == sizeof(k_lang_native), "language tables must match");
int  k_show_mature = 0;
int  k_last_mock = 0;
char k_notice[200] = "";
const char *pm_name(int i) {
    static const char *const n[PM_N] = { N_("Proxy 360p"), N_("Transcodificado 270p"), N_("Directo (HTTPS)") };
    return T(n[(i >= 0 && i < PM_N) ? i : 0]);
}

/* ------------------------------------------------------------------ listas */
void klist_clear(klist_t *l) { free(l->v); l->v = NULL; l->n = l->cap = 0; }
kstream_t *klist_add(klist_t *l) {
    if (l->n >= l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        kstream_t *nv = realloc(l->v, nc * sizeof(kstream_t));
        if (!nv) return NULL;
        l->v = nv; l->cap = nc;
    }
    kstream_t *s = &l->v[l->n++];
    memset(s, 0, sizeof(*s));
    s->live = -1;
    return s;
}

int kick_is_gambling(const char *name, const char *slug) {
    static const char *bad[] = { "slot", "casino", "gambl", "poker", "roulette", "ruleta", "blackjack",
                                 "betting", "apuesta", "plinko", "crash", "dice", "baccarat", "sportsbook", NULL };
    char a[96]; int i, k;
    for (k = 0; k < 2; k++) {
        const char *s = k ? slug : name;
        if (!s) continue;
        snprintf(a, sizeof(a), "%s", s);
        for (i = 0; a[i]; i++) a[i] = tolower((unsigned char)a[i]);
        for (i = 0; bad[i]; i++) if (strstr(a, bad[i])) return 1;
    }
    return 0;
}

void kstream_style(kstream_t *s) {
    static const u32 pal[] = { 0x1F7A0A, 0x1565C0, 0x6A1B9A, 0xC62828, 0x00838F, 0xEF6C00, 0x4527A0, 0xAD1457, 0x558B2F, 0x283593 };
    unsigned h = 5381; const char *p;
    for (p = s->slug; *p; p++) h = h * 33 + (unsigned char)*p;
    s->color = pal[h % 10];
    const char *n = s->name[0] ? s->name : s->slug;
    int o = 0;
    for (p = n; *p && o < 2; p++) if (isalnum((unsigned char)*p)) s->shortn[o++] = toupper((unsigned char)*p);
    s->shortn[o] = 0;
    if (!o) snprintf(s->shortn, sizeof(s->shortn), "?");
}

static int is_true(const char *v) { char b[8] = ""; if (!v) return 0; v = jws(v); if (*v == 't') return 1; jstr(v, b, sizeof(b)); return !strcmp(b, "true") || !strcmp(b, "1"); }
static void jcopy(const char *obj, const char *key, char *out, int sz) {
    const char *v = obj ? jget(obj, key) : NULL;
    out[0] = 0;
    if (v && !jis_null(v)) jstr(v, out, sz);
}
static int valid_slug(const char *s) {
    int i;
    if (!s[0] || strlen(s) > 40) return 0;
    for (i = 0; s[i]; i++) if (!isalnum((unsigned char)s[i]) && s[i] != '_' && s[i] != '-') return 0;
    return 1;
}

/* ------------------------------------------------------------------ ajustes en la SD */
void cfg_load(void) {
    char b[160];
    if (sh_cfg_read("aspecto.txt", b, sizeof(b))) gfx_set_aspect(!strcmp(b,"4:3")?1:!strcmp(b,"16:9")?2:0);
    if (sh_cfg_read("server.txt", b, sizeof(b))) {
        if (strncmp(b, "http://", 7) && strncmp(b, "https://", 8)) snprintf(k_server, sizeof(k_server), "http://%s", b);
        else snprintf(k_server, sizeof(k_server), "%s", b);
        int n = strlen(k_server);
        while (n > 0 && k_server[n - 1] == '/') k_server[--n] = 0;
        if (!strcmp(k_server, "http://") || !strcasecmp(b, "directo")) k_server[0] = 0;
    }
    if(sh_cfg_read("chat.txt",b,sizeof(b)))k_chat_on=strcmp(b,"no")!=0;
    if (sh_cfg_read("modo.txt", b, sizeof(b))) {
        if (!strcmp(b, "transcode")) k_play_mode = PM_TRANSCODE;
        else if (!strcmp(b, "direct")) k_play_mode = PM_DIRECT;
        else k_play_mode = PM_PROXY;
    }
    if (sh_cfg_read("idioma-interfaz.txt", b, sizeof(b))) {
        int l = lang_from_code(b);
        if (l >= 0) { i18n_set(l); ui_lang_explicit = 1; }
    }
    if (!sh_cfg_read("idioma.txt", b, sizeof(b))) k_lang_default_from_ui();   /* no saved stream language: follow the interface language */
    else {
        int i;
        for (i = 0; i < k_nlangs; i++) if (!strcmp(b, k_langs[i])) k_lang = i;
    }
    if (sh_cfg_read("filtro.txt", b, sizeof(b))) k_show_mature = !strcmp(b, "mostrar-todo");
}
void cfg_save(void) {
    if (k_play_mode < 0 || k_play_mode >= PM_N) k_play_mode = PM_PROXY;
    sh_cfg_write("aspecto.txt", gfx_aspect_mode()==1?"4:3":gfx_aspect_mode()==2?"16:9":"auto");
    if (k_lang < 0 || k_lang >= k_nlangs) k_lang = 0;
    static const char *pm[PM_N] = { "proxy", "transcode", "direct" };
    sh_cfg_write("chat.txt",k_chat_on?"si":"no");
    sh_cfg_write("server.txt", k_server[0] ? k_server : "directo");
    sh_cfg_write("modo.txt", pm[k_play_mode]);
    sh_cfg_write("idioma.txt", k_langs[k_lang]);
    if (ui_lang_explicit) sh_cfg_write("idioma-interfaz.txt", lang_code(ui_lang));
    sh_cfg_write("filtro.txt", k_show_mature ? "mostrar-todo" : "ocultar-18-y-apuestas");
}

/* ------------------------------------------------------------------ HTTP */
static int kget_impl(const char *url, sh_resp_t *r, char *err, int esz, int bg, const volatile int *cancel) {
    static const char *const al[LANG_COUNT] = { "Accept-Language: en-US,en;q=0.9", "Accept-Language: es-AR,es;q=0.9", "Accept-Language: fr-FR,fr;q=0.9", "Accept-Language: de-DE,de;q=0.9", "Accept-Language: it-IT,it;q=0.9", "Accept-Language: nl-NL,nl;q=0.9", "Accept-Language: pt-BR,pt;q=0.9", "Accept-Language: ja-JP,ja;q=0.9", "Accept-Language: zh-CN,zh;q=0.9", "Accept-Language: zh-TW,zh;q=0.9", "Accept-Language: ko-KR,ko;q=0.9" };
    const char *hdr[] = { al[ui_lang], NULL };
    int srv = k_server[0] && !strncmp(url, k_server, strlen(k_server));
    if(bg) sh_request_bg(url,hdr,r,cancel); else sh_request("GET", url, hdr, NULL, NULL, r);
    if (r->res != 0) {
        if (srv) snprintf(err, esz, T("No se pudo conectar al servidor %s (%s). Revis\xc3\xa1 la IP en Ajustes."), k_server, r->err);
        else snprintf(err, esz, T("Error de red: %s (revis\xc3\xa1 Internet y la fecha/hora de la Wii)"), r->err);
        return 0;
    }
    if (r->code == 404) { snprintf(err, esz, T("No existe ese canal.")); return 0; }
    if (r->code == 429) { snprintf(err, esz, T("Demasiados pedidos: esper\xc3\xa1 unos segundos.")); return 0; }
    if (r->code == 403 || r->code == 503) { snprintf(err, esz, T("kick.com rechaz\xc3\xb3 la conexi\xc3\xb3n (HTTP %ld, Cloudflare). Us\xc3\xa1 el servidor companion."), r->code); return 0; }
    if (r->code < 200 || r->code >= 300) {
        char m[120] = "";
        if (r->data && *jws(r->data) == '{') jcopy(jws(r->data), "error", m, sizeof(m));
        snprintf(err, esz, T("%s respondi\xc3\xb3 HTTP %ld. %s"), srv ? T("El servidor") : "kick.com", r->code, m);
        return 0;
    }
    if (!r->data || (*jws(r->data) != '{' && *jws(r->data) != '[')) { snprintf(err, esz, T("Respuesta inesperada (no es JSON).")); return 0; }
    return 1;
}

static int kget(const char *url,sh_resp_t *r,char *err,int esz) {
    return kget_impl(url,r,err,esz,0,NULL);
}

int kw_health(char *err, int esz) {
    char url[200];
    sh_resp_t r;
    k_server_ok = 0;
    if (!k_server[0]) { snprintf(err, esz, T("Sin servidor: modo directo a kick.com.")); return 0; }
    snprintf(url, sizeof(url), "%s/wii/health", k_server);
    if (!kget(url, &r, err, esz)) { sh_resp_free(&r); return 0; }
    k_server_ok = is_true(jget(jws(r.data), "ok"));
    if (!k_server_ok) snprintf(err, esz, T("El servidor respondi\xc3\xb3 algo raro."));
    sh_resp_free(&r);
    return k_server_ok;
}

/* ------------------------------------------------------------------ lista en vivo */
static void parse_compact(const char *e, kstream_t *s) {
    const char *v;
    s->channel_id=(unsigned long)jnum(jget(e,"channel_id"));s->chatroom_id=(unsigned long)jnum(jget(e,"chatroom_id"));
    jcopy(e, "slug", s->slug, sizeof(s->slug));
    jcopy(e, "name", s->name, sizeof(s->name));
    jcopy(e, "title", s->title, sizeof(s->title));
    jcopy(e, "category", s->cat, sizeof(s->cat));
    jcopy(e, "lang", s->lang, sizeof(s->lang));
    jcopy(e, "start_time", s->start, sizeof(s->start));
    jcopy(e, "thumb_rgb", s->thumb, sizeof(s->thumb));
    jcopy(e, "avatar_rgb", s->avatar, sizeof(s->avatar));
    jcopy(e, "banner_rgb", s->banner, sizeof(s->banner));
    jcopy(e, "bio", s->bio, sizeof(s->bio));
    if ((v = jget(e, "viewers"))) s->viewers = (int)jnum(v);
    if ((v = jget(e, "followers"))) s->followers = (int)jnum(v);
    s->verified = is_true(jget(e, "verified"));
    s->mature = is_true(jget(e, "mature"));
    s->gambling = is_true(jget(e, "gambling"));
    s->mock = is_true(jget(e, "mock"));
    v = jget(e, "live");
    s->live = v ? is_true(v) : 1;
    if (!s->name[0]) snprintf(s->name, sizeof(s->name), "%s", s->slug);
    kstream_style(s);
}

/* Select a published small srcset URL without inventing CDN variants. */
static void small_thumb(const char *obj, char *out, int sz) {
    char set[3072]; jcopy(obj, "src", out, sz); jcopy(obj, "srcset", set, sizeof(set));
    if(!out[0])jcopy(obj,"url",out,sz);
    if(!set[0])jcopy(obj,"responsive",set,sizeof(set));
    int best = 0x7fffffff;
    char *save = NULL, *item = strtok_r(set, ",", &save);
    while (item) {
        char url[768]; int width = 0;
        if (sscanf(item, " %767s %dw", url, &width) == 2 && width > 0 && width < best && !strncmp(url, "https://", 8)) {
            best = width; snprintf(out, sz, "%s", url);
        }
        item = strtok_r(NULL, ",", &save);
    }
}

static void parse_kick_live(const char *e, kstream_t *s) {
    const char *ch = jget(e, "channel"), *cat = jget(e, "category"), *v;
    char cs[64] = "";
    jcopy(ch, "slug", s->slug, sizeof(s->slug));
    jcopy(ch, "username", s->name, sizeof(s->name));
    jcopy(e, "title", s->title, sizeof(s->title));
    jcopy(cat, "name", s->cat, sizeof(s->cat));
    jcopy(cat, "slug", cs, sizeof(cs));
    jcopy(e, "language", s->lang, sizeof(s->lang));
    jcopy(e, "start_time", s->start, sizeof(s->start));
    small_thumb(jget(e, "thumbnail"), s->thumb, sizeof(s->thumb));
    jcopy(ch, "profile_pic", s->avatar, sizeof(s->avatar));
    if ((v = jget(e, "viewer_count"))) s->viewers = (int)jnum(v);
    s->mature = is_true(jget(e, "is_mature"));
    s->gambling = kick_is_gambling(s->cat, cs);
    s->live = 1;
    if (!s->name[0]) snprintf(s->name, sizeof(s->name), "%s", s->slug);
    kstream_style(s);
}

/* Category art comes from the existing read-only search response, not guessed CDN paths. */
static char cat_names[16][80],cat_images[16][768];static int cat_n;
static int category_art(const char *name,const char *slug,char *out,int size) {
    int i;out[0]=0;if(!name[0]||k_server[0])return 0;
    for(i=0;i<cat_n;i++)if(!strcmp(name,cat_names[i])) {snprintf(out,size,"%s",cat_images[i]);return 1;}
    char url[512],err[160];char *esc=sh_urlencode(name);sh_resp_t r;
    snprintf(url,sizeof(url),"https://kick.com/api/search?searched_word=%s",esc);free(esc);
    if(!kget(url,&r,err,sizeof(err))) {sh_resp_free(&r);return -1;}
    const char *cats=jget(jws(r.data),"categories"),*c;int no_variant=1;
    for(c=cats?jarr_first(cats):NULL;c;c=jarr_next(c)) {
        char cn[80];jcopy(c,"name",cn,sizeof(cn));
        if(!strcasecmp(name,cn)) {const char *b=jget(c,"banner");char set[32];jcopy(b,"srcset",set,sizeof(set));no_variant=!set[0];small_thumb(b,out,size);break;}
    }
    sh_resp_free(&r);
    /* Some search responses publish a nonexistent /conversion/*.webp (HTTP 403).
       The existing channel response also publishes the ORIGINAL JPEG URL. */
    if(no_variant && valid_slug(slug)) {
        snprintf(url,sizeof(url),"https://kick.com/api/v2/channels/%s",slug);
        if(kget(url,&r,err,sizeof(err))) {
            const char *recent=jget(jws(r.data),"recent_categories");
            for(c=recent?jarr_first(recent):NULL;c;c=jarr_next(c)) {
                char cn[80];jcopy(c,"name",cn,sizeof(cn));
                if(!strcasecmp(name,cn)) {small_thumb(jget(c,"banner"),out,size);break;}
            }
        }
        sh_resp_free(&r);
    }
    if(cat_n<16) {snprintf(cat_names[cat_n],sizeof(cat_names[0]),"%s",name);snprintf(cat_images[cat_n],sizeof(cat_images[0]),"%s",out);cat_n++;}
    return 1;
}
static int live_impl(unsigned long category_id, char *after, int asz, klist_t *out, int *more, char *err, int esz) {
    char url[512];
    sh_resp_t r;
    const char *d, *arr, *e;
    int n = 0;
    *more = 0;
    k_notice[0] = 0;
    char *ea = sh_urlencode(after);
    if (category_id) {
        char lp[24]="";
        if(k_lang != 1) snprintf(lp,sizeof(lp),"&language=%s",k_langs[k_lang]);
        snprintf(url,sizeof(url),"https://web.kick.com/api/v1/livestreams?limit=24&sort=viewer_count_desc&category_id=%lu%s%s%s",category_id,lp,after[0]?"&after=":"",ea);
    } else if (k_server[0])
        snprintf(url, sizeof(url), "%s/wii/live?limit=24&lang=%s&tw=224&th=126%s%s", k_server, k_langs[k_lang], after[0] ? "&after=" : "", ea);
    else {
        char lp[24] = "";
        if (k_lang != 1) snprintf(lp, sizeof(lp), "&language=%s", k_langs[k_lang]);   /* 1 = todos */
        snprintf(url, sizeof(url), "https://web.kick.com/api/v1/livestreams?limit=24&sort=viewer_count_desc%s%s%s",
                 lp, after[0] ? "&after=" : "", ea);
    }
    free(ea);
    if (!kget(url, &r, err, esz)) { sh_resp_free(&r); return -1; }
    d = jws(r.data);
    if (k_server[0] && !category_id) {
        char src[16] = "";
        jcopy(d, "source", src, sizeof(src));
        k_last_mock = strcmp(src, "kick") != 0;
        jcopy(d, "notice", k_notice, sizeof(k_notice));
        jcopy(d, "next", after, asz);
        arr = jget(d, "items");
        for (e = arr ? jarr_first(arr) : NULL; e; e = jarr_next(e)) {
            kstream_t *s = klist_add(out);
            if (!s) break;
            parse_compact(e, s);
            if (!s->slug[0]) { out->n--; continue; }
            n++;
        }
    } else {
        k_last_mock = 0;
        jcopy(jpath(d, "data.pagination"), "next_cursor", after, asz);
        arr = jpath(d, "data.livestreams");
        for (e = arr ? jarr_first(arr) : NULL; e; e = jarr_next(e)) {
            if(category_id && (unsigned long)jnum(jpath(e,"category.id"))!=category_id) continue;
            char slug[48]; jcopy(jget(e,"channel"),"slug",slug,sizeof(slug));
            int di; for(di=0;di<out->n;di++)if(!strcmp(out->v[di].slug,slug))break;
            if(di<out->n)continue; /* live ranking may move between cursor pages */
            kstream_t *s = klist_add(out);
            if (!s) {snprintf(err,esz,T("Sin memoria al cargar directos."));sh_resp_free(&r);return -1;}
            parse_kick_live(e, s);
            if (!s->slug[0]) { out->n--; continue; }
            n++;
        }
    }
    *more = after[0] != 0 && n > 0;
    sh_resp_free(&r);
    int ci;for(ci=0;!category_id && ci<out->n;ci++) {
        if(category_art(out->v[ci].cat,out->v[ci].slug,out->v[ci].catart,sizeof(out->v[ci].catart))<0)break;
    }
    return n;
}

int kw_live(char *after,int asz,klist_t *out,int *more,char *err,int esz) {
    return live_impl(0,after,asz,out,more,err,esz);
}
int kw_category_live(unsigned long id,char *after,int asz,klist_t *out,int *more,char *err,int esz) {
    return live_impl(id,after,asz,out,more,err,esz);
}
/* Native public category response: names, parent groups, mature flag, actual
 * viewer totals, and published small cover URLs. Never invent CDN variants. */
int kw_categories_page(int page,kc_category *out,int cap,kc_page *meta,char *err,int esz,const volatile int *cancel) {
    char url[256]; sh_resp_t r; int n=0;
    memset(meta,0,sizeof(*meta));
    snprintf(url,sizeof(url),"https://kick.com/api/v1/subcategories?limit=32&page=%d",page);
    if(!kget_impl(url,&r,err,esz,1,cancel)) {
        meta->retryable=r.res==CURLE_OPERATION_TIMEDOUT || r.res==CURLE_COULDNT_CONNECT ||
            r.res==CURLE_COULDNT_RESOLVE_HOST || r.res==CURLE_SEND_ERROR || r.res==CURLE_RECV_ERROR || r.res==CURLE_GOT_NOTHING;
        sh_resp_free(&r);return -1;
    }
    const char *d=jws(r.data),*arr=jget(d,"data"),*e;
    meta->page=(int)jnum(jget(d,"current_page"));meta->last=(int)jnum(jget(d,"last_page"));
    meta->total=(int)jnum(jget(d,"total"));
    for(e=arr?jarr_first(arr):NULL;e && n<cap;e=jarr_next(e)) {
        kc_category c;memset(&c,0,sizeof(c));const char *group=jget(e,"category");
        c.id=(unsigned long)jnum(jget(e,"id"));c.group_id=(unsigned long)jnum(jget(group,"id"));
        jcopy(e,"name",c.name,sizeof(c.name));jcopy(e,"slug",c.slug,sizeof(c.slug));
        jcopy(group,"name",c.group,sizeof(c.group));
        c.viewers=(int)jnum(jget(e,"viewers"));if(c.viewers<0)c.viewers=0;
        char group_slug[48];jcopy(group,"slug",group_slug,sizeof(group_slug));
        /* Prefer authoritative parent metadata: "Crash Bandicoot" is a game,
         * not the legacy keyword "crash" gambling category. */
        c.gambling=c.group_id ? !strcasecmp(group_slug,"gambling") : kick_is_gambling(c.name,c.slug);
        c.mature=is_true(jget(e,"is_mature")) || c.gambling;
        small_thumb(jget(e,"banner"),c.art,sizeof(c.art));
        if(c.id && c.name[0])out[n++]=c;
    }
    sh_resp_free(&r);
    if(!meta->page || !arr) {snprintf(err,esz,T("Respuesta de categorias no reconocida."));return -1;}
    return n;
}

/* ------------------------------------------------------------------ busqueda */
int kw_search(const char *q, klist_t *out, char *err, int esz) {
    char url[300];
    sh_resp_t r;
    const char *d, *arr, *e;
    int n = 0;
    char *eq = sh_urlencode(q);
    if (k_server[0]) snprintf(url, sizeof(url), "%s/wii/search?q=%s", k_server, eq);
    else snprintf(url, sizeof(url), "https://kick.com/api/search?searched_word=%s", eq);
    free(eq);
    if (!kget(url, &r, err, esz)) { sh_resp_free(&r); return -1; }
    d = jws(r.data);
    arr = jget(d, "channels");
    for (e = arr ? jarr_first(arr) : NULL; e && n < 30; e = jarr_next(e)) {
        kstream_t *s = klist_add(out);
        if (!s) break;
        if (k_server[0]) {
            parse_compact(e, s);
            jcopy(e, "avatar_rgb", s->avatar, sizeof(s->avatar));
        } else {
            const char *v;
            jcopy(e, "slug", s->slug, sizeof(s->slug));
            jcopy(jget(e, "user"), "username", s->name, sizeof(s->name));
            jcopy(jget(e,"user"),"profile_pic",s->avatar,sizeof(s->avatar));
            if(!s->avatar[0])jcopy(jget(e,"user"),"profilePic",s->avatar,sizeof(s->avatar));
            s->live = is_true(jget(e, "isLive"));
            if ((v = jget(e, "followersCount"))) s->followers = (int)jnum(v);
            s->verified = (v = jget(e, "verified")) && !jis_null(v);
            if (!s->name[0]) snprintf(s->name, sizeof(s->name), "%s", s->slug);
            kstream_style(s);
        }
        if (!s->slug[0]) { out->n--; continue; }
        snprintf(s->title, sizeof(s->title), "%s", s->live ? T("En vivo ahora") : T("Sin transmisi\xc3\xb3n"));
        n++;
    }
    /* en vivo primero */
    int i, j;
    for (i = 1; i < out->n; i++) for (j = i; j > 0 && out->v[j].live > out->v[j - 1].live; j--) {
        kstream_t t = out->v[j]; out->v[j] = out->v[j - 1]; out->v[j - 1] = t;
    }
    sh_resp_free(&r);
    return n;
}

/* ------------------------------------------------------------------ canal */
static void read_categories(const char *arr, kstream_t *s) {
    const char *c;
    for (c = jarr_first(arr); c; c = jarr_next(c)) {
        char nm[64] = "", sl[64] = "";
        jcopy(c, "name", nm, sizeof(nm));
        jcopy(c, "slug", sl, sizeof(sl));
        if (!s->cat[0]) snprintf(s->cat, sizeof(s->cat), "%s", nm);
        if (kick_is_gambling(nm, sl)) s->gambling = 1;
        if (is_true(jget(c, "is_mature"))) s->mature = 1;
    }
}

/* forma de kick.com/api/v2/channels/{slug} (el companion /api/channels/ responde igual) */
int kick_parse_channel(const char *json, kstream_t *s, char *url, int usz) {
    const char *d = jws(json), *v, *ls;
    url[0] = 0;
    if (!d || *d != '{') return -1;
    jcopy(d, "slug", s->slug, sizeof(s->slug));
    jcopy(jget(d, "user"), "username", s->name, sizeof(s->name));
    jcopy(jget(d, "user"), "bio", s->bio, sizeof(s->bio));
    jcopy(jget(d, "user"), "profile_pic", s->avatar, sizeof(s->avatar));
    s->broadcaster_user_id=(unsigned long)jnum(jget(d,"user_id"));if(!s->broadcaster_user_id)s->broadcaster_user_id=(unsigned long)jnum(jpath(d,"user.id"));
    s->channel_id=(unsigned long)jnum(jget(d,"id"));s->chatroom_id=(unsigned long)jnum(jpath(d,"chatroom.id"));
    jcopy(d, "playback_url", url, usz);
    small_thumb(jget(d,"banner_image"),s->banner,sizeof(s->banner));
    if ((v = jget(d, "followers_count"))) s->followers = (int)jnum(v);
    s->verified = (v = jget(d, "verified")) && !jis_null(v) && *jws(v) != 'f';
    ls = jget(d, "livestream");
    s->live = 0;
    if (ls && !jis_null(ls)) {
        s->live = is_true(jget(ls, "is_live"));
        jcopy(ls, "session_title", s->title, sizeof(s->title));
        jcopy(jget(ls, "thumbnail"), "url", s->thumb, sizeof(s->thumb));
        jcopy(ls, "start_time", s->start, sizeof(s->start));
        if ((v = jget(ls, "viewer_count"))) s->viewers = (int)jnum(v);
        jcopy(ls, "language", s->lang, sizeof(s->lang));
        if (is_true(jget(ls, "is_mature"))) s->mature = 1;
        s->cat[0] = 0;
        if ((v = jget(ls, "categories"))) read_categories(v, s);
    }
    if(!s->live) {
        small_thumb(jget(d,"offline_banner_image"),s->thumb,sizeof(s->thumb));
        if(!s->thumb[0])snprintf(s->thumb,sizeof(s->thumb),"%s",s->banner);
    }
    const char *recent=jget(d,"recent_categories"),*rc;
    for(rc=recent?jarr_first(recent):NULL;rc;rc=jarr_next(rc)) {
        char name[80];jcopy(rc,"name",name,sizeof(name));
        if(!s->cat[0]||!strcmp(name,s->cat)) {small_thumb(jget(rc,"banner"),s->catart,sizeof(s->catart));break;}
    }
    if (!s->name[0]) snprintf(s->name, sizeof(s->name), "%s", s->slug);
    kstream_style(s);
    return s->live;
}

int kw_channel(const char *slug, kstream_t *s, char *err, int esz) {
    char url[300];
    sh_resp_t r;
    if (!valid_slug(slug)) { snprintf(err, esz, T("Nombre de canal inv\xc3\xa1lido: \"%s\"."), slug); return -1; }
    if (k_server[0]) snprintf(url, sizeof(url), "%s/wii/channel/%s", k_server, slug);
    else snprintf(url, sizeof(url), "https://kick.com/api/v2/channels/%s", slug);
    if (!kget(url, &r, err, esz)) { sh_resp_free(&r); return -1; }
    kstream_t keep = *s;
    memset(s, 0, sizeof(*s));
    if (k_server[0]) {
        const char *d = jws(r.data);
        parse_compact(d, s);
        s->live = is_true(jget(d, "live"));
    } else {
        static char pu[4096];
        kick_parse_channel(r.data, s, pu, sizeof(pu));
    }
    if (!s->slug[0]) snprintf(s->slug, sizeof(s->slug), "%s", slug);
    /* sin miniatura propia: conservar la de la tarjeta */
    if ((!s->thumb[0] || (!k_server[0] && s->live>0 && keep.thumb[0])) && !strcmp(keep.slug, s->slug)) snprintf(s->thumb, sizeof(s->thumb), "%s", keep.thumb);
    if (!s->avatar[0] && !strcmp(keep.slug, s->slug)) snprintf(s->avatar, sizeof(s->avatar), "%s", keep.avatar);
    if(!s->catart[0] && !strcmp(keep.slug,s->slug))snprintf(s->catart,sizeof(s->catart),"%s",keep.catart);
    kstream_style(s);
    sh_resp_free(&r);
    if(k_server[0]&&s->live&&(!s->channel_id||!s->chatroom_id||!s->broadcaster_user_id)) {
        /* Older companions already expose the full channel JSON at this route. */
        char ce[180];sh_resp_t cr;snprintf(url,sizeof(url),"%s/api/channels/%s",k_server,slug);
        if(kget(url,&cr,ce,sizeof(ce))){s->broadcaster_user_id=(unsigned long)jnum(jget(cr.data,"user_id"));if(!s->broadcaster_user_id)s->broadcaster_user_id=(unsigned long)jnum(jpath(cr.data,"user.id"));s->channel_id=(unsigned long)jnum(jget(cr.data,"id"));s->chatroom_id=(unsigned long)jnum(jpath(cr.data,"chatroom.id"));}
        sh_resp_free(&cr);
    }
    /* /channels may publish obsolete stream.kick.com URLs (403). The public
       /livestream response supplies current responsive images, even after search. */
    if(!k_server[0]&&s->live&&(!s->thumb[0]||strstr(s->thumb,"https://stream.kick.com/"))) {
        char ie[180];sh_resp_t ir;snprintf(url,sizeof(url),"https://kick.com/api/v2/channels/%s/livestream",slug);
        if(kget(url,&ir,ie,sizeof(ie))) {
            const char *d=jws(ir.data),*v=jget(d,"data");if(v&&!jis_null(v))d=v;
            char image[768];small_thumb(jget(d,"thumbnail"),image,sizeof(image));
            if(image[0])snprintf(s->thumb,sizeof(s->thumb),"%s",image);
        }
        sh_resp_free(&ir);
    }
    return s->live;
}

/* ------------------------------------------------------------------ video */
int kw_play_url(const kstream_t *s, char *url, int usz, char *refresh, int rsz, int *max_h, char *err, int esz) {
    static const char *pm[PM_N] = { "proxy", "transcode", "direct" };
    refresh[0] = 0;
    *max_h = 360;
    if (!s->live) { snprintf(err, esz, T("%s no est\xc3\xa1 transmitiendo ahora."), s->name); return 0; }
    if (!k_show_mature && (s->mature || s->gambling)) {
        snprintf(err, esz, T("Oculto por el filtro: %s. Se cambia en Ajustes."), s->gambling ? T("categor\xc3\xad" "a de apuestas/casino") : T("contenido +18"));
        return 0;
    }
    if (k_server[0]) {
        snprintf(url, usz, "%s/play/%s.m3u8?mode=%s&max_h=360", k_server, s->slug, pm[k_play_mode]);
        /* en modo directo el token de Kick vence: se renueva pidiendo el canal al companion */
        if (k_play_mode == PM_DIRECT) snprintf(refresh, rsz, "%s/api/channels/%s", k_server, s->slug);
        return 1;
    }
    /* sin servidor: pedir playback_url a kick.com */
    char api[160];
    sh_resp_t r;
    kstream_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    snprintf(api, sizeof(api), "https://kick.com/api/v2/channels/%s", s->slug);
    if (!kget(api, &r, err, esz)) { sh_resp_free(&r); return 0; }
    int live = kick_parse_channel(r.data, &tmp, url, usz);
    sh_resp_free(&r);
    if (live != 1 || strncmp(url, "https://", 8)) { snprintf(err, esz, T("%s no est\xc3\xa1 transmitiendo ahora."), s->name); return 0; }
    snprintf(refresh, rsz, "%s", api);
    return 1;
}
