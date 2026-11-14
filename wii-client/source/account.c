/* KickWii 1.3: funciones con sesion usando SOLO la API publica oficial de Kick
 * (api.kick.com/public): perfil, mi canal (titulo/categoria), respuestas, menciones,
 * emotes y moderacion. Cada accion pide confirmacion; un permiso faltante o un 403 se
 * explica en castellano y nunca rompe la app. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include "tv.h"
#include "auth.h"
#define OX ((SCR_W-640)/2)
#define APIBUF 32768

typedef struct { const char *scope, *label; } perm_t;
static const perm_t perms[] = {
    { "user:read", "Ver tu perfil (nombre y avatar)" },
    { "chat:write", "Escribir y responder en el chat" },
    { "channel:read", "Ver tu canal (título y categoría)" },
    { "channel:write", "Cambiar título y categoría" },
    { "moderation:chat_message:manage", "Borrar mensajes (si sos moderador)" },
    { "moderation:ban", "Expulsar o banear (si sos moderador)" },
};
#define NPERM ((int)(sizeof(perms) / sizeof(perms[0])))
static const char *perm_label(const char *scope) { for (int i = 0; i < NPERM; i++) if (!strcmp(perms[i].scope, scope)) return perms[i].label; return scope; }

static int need_scope(const char *title, const char *scope) {
    if (auth_scope(scope)) return 1;
    char t[420];
    snprintf(t, sizeof(t), "Tu sesión no tiene el permiso \"%s\" (%s).\n\nEl desarrollador tiene que habilitarlo en la app KickWii de Kick. Después entra a Mi cuenta, Cerrar sesión, y volvé a iniciar sesión para aprobarlo.\n\nNo se hizo ningún cambio.", perm_label(scope), scope);
    sui_message(title, t);
    return 0;
}
static void api_message(long code, const char *json, const char *what, char *out, int cap) {
    char msg[160] = "";
    const char *m = json && json[0] == '{' ? jget(json, "message") : NULL;
    if (m && !jis_null(m)) jstr(m, msg, sizeof(msg));
    if (code == -1) snprintf(out, cap, "Primero iniciá sesión (Ajustes > Cuenta).");
    else if (code == 0) snprintf(out, cap, "%s: Kick no respondió (red). No se hizo ningún cambio.", what);
    else if (code == 401) snprintf(out, cap, "%s: Kick cerró tu sesión. Iniciá sesión otra vez.", what);
    else if (code == 403) snprintf(out, cap, "%s: Kick no lo permitió (403). Puede faltar un permiso o no sos moderador/dueño de este canal.%s%s", what, msg[0] ? " " : "", msg);
    else if (code == 404) snprintf(out, cap, "%s: Kick no encontró el mensaje o usuario (404).", what);
    else if (code == 429) snprintf(out, cap, "%s: demasiados pedidos (429). Esperá un momento.", what);
    else snprintf(out, cap, "%s: Kick respondió HTTP %ld. %s", what, code, msg);
}

/* ---------------------------------------------------------------- Mi cuenta */
void account_screen(void) {
    if (!auth_has_session()) { auth_screen(); return; }
    ui_set_backdrop(NULL);
    thumbs_pause(0);
    if (auth_due()) auth_ensure();
    int foc = 0;
    static const char *labels[4] = { "Mi canal", "Renovar", "Cerrar sesión", "Volver" };
    static const int icons[4] = { IC_USER, IC_NEXT, IC_STOP, IC_BACK };
    while (SYS_MainLoop()) {
        u32 d = in_poll();
        ui_begin(d);
        if (d & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) break;
        if (d & (WPAD_BUTTON_LEFT | WPAD_BUTTON_UP)) { foc = (foc + 3) % 4; if (in_ptr.virt) in_ptr.valid = 0; }
        if (d & (WPAD_BUTTON_RIGHT | WPAD_BUTTON_DOWN)) { foc = (foc + 1) % 4; if (in_ptr.virt) in_ptr.valid = 0; }
        thumbs_frame();
        gfx_clear(C_BG);
        ui_panel(20 + OX, 20, 600, 440, "Mi cuenta de Kick");
#ifdef AUTH_MOCK
        gfx_text_c(&font_bold, SCR_W / 2, 70, "SIMULACION LOCAL - CUENTA Y SERVIDORES FALSOS", C_WARN);
#endif
        const kimg_t *im = auth_avatar()[0] ? thumb_get(auth_avatar()) : NULL;
        gfx_circle(44 + OX + 48, 96 + 48, 51, C_ACCENT, 255);
        if (im) gfx_blit_scaled_round(im->px, im->w, im->h, 44 + OX, 96, 96, 96, 48);
        else { gfx_circle(44 + OX + 48, 96 + 48, 48, C_PANEL2, 255); gfx_icon(IC_USER, 44 + OX + 48, 96 + 48, 48, C_ACCENT); }
        gfx_text_fit(&font_title, 164 + OX, 96, auth_login()[0] ? auth_login() : "(sin verificar)", C_ACCENT, 430);
        char t[200];
        if (auth_uid()) snprintf(t, sizeof(t), "ID de usuario en Kick: %lu", auth_uid()); else snprintf(t, sizeof(t), "ID de usuario: sin dato");
        gfx_text_fit(&font_small, 164 + OX, 132, t, C_DIM, 430);
        gfx_text_fit(&font_small, 164 + OX, 152, auth_status(), C_DIM, 430);
        gfx_text_fit(&font_bold, 44 + OX, 208, "Permisos aprobados en esta sesión", C_TEXT, 552);
        for (int i = 0; i < NPERM; i++) {
            int ok = auth_scope(perms[i].scope), y = 232 + i * 21;
            gfx_rrect(48 + OX, y + 1, 34, 16, 6, ok ? C_ACCENT : C_PANEL2, 255);
            gfx_text_c(&font_small, 65 + OX, y + 1, ok ? "Sí" : "No", ok ? C_DARK : C_WARN);
            gfx_text_fit(&font_small, 92 + OX, y, perms[i].label, ok ? C_TEXT : C_FAINT, 500);
        }
        gfx_text_fit(&font_small, 44 + OX, 362, "Si falta uno: el desarrollador lo habilita en Kick y vos cerrás sesión y volvés a entrar.", C_FAINT, 552);
        int act = -1;
        for (int k = 0; k < 4; k++)
            if (ui_button(42 + OX + k * 142, 400, 132, 34, labels[k], icons[k], k == 0 ? BS_ACCENT : BS_NORMAL, foc == k && !in_ptr.valid)) act = k;
        ui_present();
        if (act < 0 && (d & WPAD_BUTTON_A) && !in_ptr.valid && !UI.consumed) act = foc;
        if (act == 0) { mychannel_screen(); ui_set_backdrop(NULL); thumbs_pause(0); }
        else if (act == 1) { auth_refresh_now(); ui_toast(auth_status()); }
        else if (act == 2) {
            char q[200];
            snprintf(q, sizeof(q), "¿Cerrar la sesión de %s en esta Wii? Se borran los tokens guardados en la SD.", auth_login());
            if (sui_confirm("Cerrar sesión", q)) { auth_logout(); ui_toast("Sesión cerrada. Tokens borrados de la SD."); break; }
        } else if (act == 3) break;
        if (!auth_has_session()) break;
    }
    thumbs_pause(1);
}

/* ---------------------------------------------------------------- Mi canal */
typedef struct { char slug[48], title[200], cat[64]; long cat_id; int live; long viewers, subs; int ok; } mych_t;
static long load_channel(mych_t *c, char *buf) {
    memset(c, 0, sizeof(*c));
    sui_busy("Mi canal", "Leyendo tu canal en Kick...");
    long code = auth_api("GET", "/v1/channels", NULL, buf, APIBUF);
    sui_busy_end();
    if (code != 200) return code;
    const char *arr = jget(buf, "data"), *e = arr ? jarr_first(arr) : NULL, *v;
    if (!e) return 404;
    if ((v = jget(e, "slug")) && !jis_null(v)) jstr(v, c->slug, sizeof(c->slug));
    if ((v = jget(e, "stream_title")) && !jis_null(v)) jstr(v, c->title, sizeof(c->title));
    const char *cat = jget(e, "category");
    if (cat && !jis_null(cat)) { if ((v = jget(cat, "name")) && !jis_null(v)) jstr(v, c->cat, sizeof(c->cat)); c->cat_id = (long)jnum(jget(cat, "id")); }
    const char *st = jget(e, "stream");
    if (st && !jis_null(st)) { v = jget(st, "is_live"); c->live = v && !strncmp(jws(v), "true", 4); c->viewers = (long)jnum(jget(st, "viewer_count")); }
    c->subs = (long)jnum(jget(e, "active_subscribers_count"));
    c->ok = 1;
    return code;
}
static int utf8_ok_title(const char *s) { return s && s[0] && auth_message_valid(s) && strlen(s) <= 140; }
static void change_title(mych_t *c, char *buf) {
    if (!need_scope("Cambiar título", "channel:write")) return;
    char t[141];
    snprintf(t, sizeof(t), "%s", c->title);
    char hint[160];
    snprintf(hint, sizeof(hint), "Se publica en kick.com/%s. Máx. 140 bytes.", c->slug);
    if (!sui_text_input_ex("Nuevo título del directo", hint, t, 140, "Guardar", "Título del directo")) return;
    if (!utf8_ok_title(t)) { ui_toast("Título vacío, muy largo o con caracteres inválidos. Sin cambios."); return; }
    char q[260];
    snprintf(q, sizeof(q), "¿Cambiar el título de tu canal a:\n\"%s\"?", t);
    if (!sui_confirm("Cambiar título", q)) return;
    char esc[900], body[960];
    if (!auth_json_escape(t, esc, sizeof(esc))) return;
    snprintf(body, sizeof(body), "{\"stream_title\":\"%s\"}", esc);
    sui_busy("Mi canal", "Guardando título en Kick...");
    long code = auth_api("PATCH", "/v1/channels", body, buf, APIBUF);
    sui_busy_end();
    if (code == 204 || code == 200) { ui_toast("Título actualizado en Kick"); snprintf(c->title, sizeof(c->title), "%s", t); }
    else { char m[300]; api_message(code, buf, "Cambiar título", m, sizeof(m)); sui_message("Cambiar título", m); }
}
static int search_categories(const char *q, char *buf, long *ids, char (*names)[64], int max) {
    char *e = sh_urlencode(q), path[300];
    int n = 0;
    snprintf(path, sizeof(path), "/v2/categories?name=%s&limit=20", e);
    sui_busy("Categoría", "Buscando categorías en Kick...");
    long code = auth_api("GET", path, NULL, buf, APIBUF);
    if (code == 200) for (const char *it = jarr_first(jget(buf, "data") ? jget(buf, "data") : "[]"); it && n < max; it = jarr_next(it)) {
        ids[n] = (long)jnum(jget(it, "id")); const char *v = jget(it, "name"); names[n][0] = 0; if (v && !jis_null(v)) jstr(v, names[n], 64); if (ids[n] > 0 && names[n][0]) n++;
    }
    if (!n && code != 401 && code != -1) { /* v1 (deprecado) busca por texto libre */
        snprintf(path, sizeof(path), "/v1/categories?q=%s", e);
        code = auth_api("GET", path, NULL, buf, APIBUF);
        if (code == 200) for (const char *it = jarr_first(jget(buf, "data") ? jget(buf, "data") : "[]"); it && n < max; it = jarr_next(it)) {
            ids[n] = (long)jnum(jget(it, "id")); const char *v = jget(it, "name"); names[n][0] = 0; if (v && !jis_null(v)) jstr(v, names[n], 64); if (ids[n] > 0 && names[n][0]) n++;
        }
    }
    sui_busy_end();
    free(e);
    return n ? n : (code == 200 ? 0 : -(int)(code < 0 ? 1 : code ? code : 1));
}
static void change_category(mych_t *c, char *buf) {
    if (!need_scope("Cambiar categoría", "channel:write")) return;
    char q[64] = "";
    if (!sui_text_input_ex("Buscar categoría", "Escribí al menos 3 letras (ej. Just Chatting, Fortnite)", q, 60, "Buscar", "Categoría")) return;
    if (strlen(q) < 3) { ui_toast("Escribí al menos 3 letras."); return; }
    long ids[20]; char names[20][64];
    int n = search_categories(q, buf, ids, names, 20);
    if (n <= 0) { if (n == 0) sui_message("Categoría", "Kick no encontró categorías con ese nombre."); else { char m[300]; api_message(-n == 1 ? 0 : -n, buf, "Buscar categoría", m, sizeof(m)); sui_message("Categoría", m); } return; }
    char *items[20]; for (int i = 0; i < n; i++) items[i] = names[i];
    int sel = 0;
    if (sui_list("Elegí la categoría", "A: elegir   B: cancelar", items, n, &sel) != WPAD_BUTTON_A) return;
    char cq[200];
    snprintf(cq, sizeof(cq), "¿Cambiar la categoría de tu canal a \"%s\"?", names[sel]);
    if (!sui_confirm("Cambiar categoría", cq)) return;
    char body[64];
    snprintf(body, sizeof(body), "{\"category_id\":%ld}", ids[sel]);
    sui_busy("Mi canal", "Guardando categoría en Kick...");
    long code = auth_api("PATCH", "/v1/channels", body, buf, APIBUF);
    sui_busy_end();
    if (code == 204 || code == 200) { ui_toast("Categoría actualizada en Kick"); snprintf(c->cat, sizeof(c->cat), "%s", names[sel]); c->cat_id = ids[sel]; }
    else { char m[300]; api_message(code, buf, "Cambiar categoría", m, sizeof(m)); sui_message("Cambiar categoría", m); }
}
void mychannel_screen(void) {
    if (!auth_has_session()) { sui_message("Mi canal", "Primero iniciá sesión (Ajustes > Cuenta)."); return; }
    if (!need_scope("Mi canal", "channel:read")) return;
    char *buf = malloc(APIBUF);
    if (!buf) return;
    mych_t c;
    long code = load_channel(&c, buf);
    if (code != 200) { char m[300]; api_message(code, buf, "Mi canal", m, sizeof(m)); sui_message("Mi canal", m); free(buf); return; }
    int sel = 0;
    while (!ui_quit) {
        char it[4][260], *items[4], title[96];
        snprintf(it[0], sizeof(it[0]), "Título del directo%s\n%s", auth_scope("channel:write") ? "  (A: cambiar)" : "", c.title[0] ? c.title : "(sin título)");
        snprintf(it[1], sizeof(it[1]), "Categoría%s\n%s", auth_scope("channel:write") ? "  (A: cambiar)" : "", c.cat[0] ? c.cat : "(sin categoría)");
        if (c.live) snprintf(it[2], sizeof(it[2]), "Estado\nEN VIVO - %ld espectadores - %ld suscriptores", c.viewers, c.subs);
        else snprintf(it[2], sizeof(it[2]), "Estado\nSin transmitir - %ld suscriptores activos", c.subs);
        snprintf(it[3], sizeof(it[3]), "Actualizar\nVolver a leer tu canal desde Kick");
        for (int i = 0; i < 4; i++) items[i] = it[i];
        snprintf(title, sizeof(title), "Mi canal: kick.com/%s", c.slug);
        u32 r = sui_list(title, "A: elegir   B: volver", items, 4, &sel);
        if (r != WPAD_BUTTON_A) break;
        if (sel == 0) change_title(&c, buf);
        else if (sel == 1) change_category(&c, buf);
        else if (sel == 3) { code = load_channel(&c, buf); if (code != 200) { char m[300]; api_message(code, buf, "Mi canal", m, sizeof(m)); sui_message("Mi canal", m); break; } }
    }
    free(buf);
}

/* ---------------------------------------------------------------- emotes */
#define NEMOTE 100
static char em_slug[48];
static int em_n;
static unsigned long em_id[NEMOTE];
static char em_name[NEMOTE][32];
static unsigned char em_kind[NEMOTE], em_sub[NEMOTE]; /* kind: 0 canal, 1 visto en chat, 2 global, 3 emoji */
static int em_has(unsigned long id) { for (int i = 0; i < em_n; i++) if (em_id[i] == id) return 1; return 0; }
static void em_add(unsigned long id, const char *name, int kind, int sub) {
    if (em_n >= NEMOTE || !id || !name[0] || strlen(name) > 30 || em_has(id)) return;
    for (const char *p = name; *p; p++) if (*p == ']' || *p == '[' || *p == ':' || (unsigned char)*p < 33) return;
    em_id[em_n] = id; snprintf(em_name[em_n], 32, "%s", name); em_kind[em_n] = kind; em_sub[em_n] = sub; em_n++;
}
/* La API oficial no lista emotes: se usan los del endpoint publico de kick.com (el mismo
 * que usa su web, sin sesion) y los vistos en el chat. Se envian como [emote:ID:NOMBRE]. */
static void em_load(const char *slug) {
    em_n = 0;
    snprintf(em_slug, sizeof(em_slug), "%s", slug ? slug : "");
    char url[200];
#ifdef AUTH_MOCK
    snprintf(url, sizeof(url), "http://127.0.0.1:8989/emotes/%s", em_slug);
#else
    snprintf(url, sizeof(url), "https://kick.com/emotes/%s", em_slug);
#endif
    sh_resp_t r;
    sui_busy("Emotes", "Cargando emotes del canal...");
    sh_request("GET", url, NULL, NULL, NULL, &r);
    sui_busy_end();
    const char *groups = sh_ok(&r) && r.data ? jws(r.data) : NULL;
    const char *g;
    int pass;
    for (pass = 0; pass < 3; pass++) {
        if (pass == 1) { unsigned long ids[48]; char names[48][32]; int n = chat_seen_emotes(ids, names, 48); for (int i = 0; i < n; i++) em_add(ids[i], names[i], 1, 0); }
        for (g = groups && *groups == '[' ? jarr_first(groups) : NULL; g; g = jarr_next(g)) {
            char gid[24] = ""; const char *v = jget(g, "id"); if (v && *jws(v) == '"') jstr(v, gid, sizeof(gid));
            int kind = jget(g, "slug") ? 0 : !strcmp(gid, "Emoji") ? 3 : 2;
            if ((pass == 0 && kind != 0) || (pass == 2 && kind == 0) || pass == 1) continue;
            const char *arr = jget(g, "emotes");
            for (const char *e = arr ? jarr_first(arr) : NULL; e; e = jarr_next(e)) {
                char nm[40] = ""; const char *nv = jget(e, "name"); if (nv && !jis_null(nv)) jstr(nv, nm, sizeof(nm));
                const char *so = jget(e, "subscribers_only");
                em_add((unsigned long)jnum(jget(e, "id")), nm, kind, so && !strncmp(jws(so), "true", 4));
            }
        }
    }
    sh_resp_free(&r);
}
int emote_pick(const char *slug, char *out, int cap) {
    if (!em_n || strcmp(em_slug, slug ? slug : "")) em_load(slug);
    else { unsigned long ids[48]; char names[48][32]; int n = chat_seen_emotes(ids, names, 48); for (int i = 0; i < n; i++) em_add(ids[i], names[i], 1, 0); }
    if (!em_n) { sui_message("Emotes", "No se pudieron cargar emotes de este canal y todavía no apareció ninguno en el chat."); return 0; }
    static char lines[NEMOTE][80];
    char *items[NEMOTE];
    static const char *kinds[4] = { "Emote del canal", "Visto en el chat", "Emote global de Kick", "Emoji de Kick" };
    for (int i = 0; i < em_n; i++) { snprintf(lines[i], sizeof(lines[i]), ":%s:\n%s%s", em_name[i], kinds[em_kind[i]], em_sub[i] ? " (solo suscriptores)" : ""); items[i] = lines[i]; }
    static int sel = 0;
    if (sel >= em_n) sel = 0;
    if (sui_list("Elegí un emote", "A: insertar   B: volver", items, em_n, &sel) != WPAD_BUTTON_A) return 0;
    snprintf(out, cap, "[emote:%lu:%s] ", em_id[sel], em_name[sel]);
    return 1;
}

/* ---------------------------------------------------------------- acciones sobre un mensaje */
static int is_owner_here(const kstream_t *s) { return auth_uid() && s && auth_uid() == s->broadcaster_user_id; }
static void moderate(const kstream_t *s, const tchat_t *m, int kind) {
    static const char *names[3] = { "Borrar mensaje", "Expulsar 10 minutos", "Banear del chat" };
    const char *scope = kind == 0 ? "moderation:chat_message:manage" : "moderation:ban";
    if (!need_scope(names[kind], scope)) return;
    if (kind == 0 && !m->id[0]) { sui_message(names[kind], "Este mensaje no trae ID de Kick; no se puede borrar desde la Wii."); return; }
    if (kind > 0 && !m->uid) { sui_message(names[kind], "Este mensaje no trae el ID del usuario; no se puede moderar desde la Wii."); return; }
    char q[320];
    if (kind == 0) snprintf(q, sizeof(q), "¿Borrar este mensaje de %s en #%s?\n\"%.120s\"", m->name, s->slug, m->text);
    else if (kind == 1) snprintf(q, sizeof(q), "¿Expulsar a %s del chat de #%s por 10 minutos?", m->name, s->slug);
    else snprintf(q, sizeof(q), "¿BANEAR a %s del chat de #%s? (permanente hasta que lo desbanees en kick.com)", m->name, s->slug);
    if (!sui_confirm(names[kind], q)) return;
    if (kind == 2 && !sui_confirm("Confirmar baneo", "¿Seguro? Esta acción es visible para el canal.")) return;
    char *buf = malloc(APIBUF), path[96], body[200];
    if (!buf) return;
    long code;
    sui_busy("Moderación", "Enviando la acción a Kick...");
    if (kind == 0) { snprintf(path, sizeof(path), "/v1/chat/%s", m->id); code = auth_api("DELETE", path, NULL, buf, APIBUF); }
    else {
        if (kind == 1) snprintf(body, sizeof(body), "{\"broadcaster_user_id\":%lu,\"user_id\":%lu,\"duration\":10,\"reason\":\"Moderado desde KickWii\"}", s->broadcaster_user_id, m->uid);
        else snprintf(body, sizeof(body), "{\"broadcaster_user_id\":%lu,\"user_id\":%lu,\"reason\":\"Moderado desde KickWii\"}", s->broadcaster_user_id, m->uid);
        code = auth_api("POST", "/v1/moderation/bans", body, buf, APIBUF);
    }
    sui_busy_end();
    if (code == 200 || code == 204) { if (kind == 0) chat_forget(m->id, NULL); else chat_forget(NULL, m->name); ui_toast(kind == 0 ? "Kick borró el mensaje" : kind == 1 ? "Usuario expulsado 10 minutos" : "Usuario baneado"); }
    else { char t[320]; api_message(code, buf, names[kind], t, sizeof(t)); sui_message(names[kind], t); }
    free(buf);
}
void chat_actions(const kstream_t *s) {
    static tchat_t msgs[24];
    int n = chat_get(msgs, 24), idx[24], k = 0;
    for (int i = n - 1; i >= 0; i--) if (!msgs[i].sys) idx[k++] = i;
    if (!k) { sui_message("Mensajes del chat", "Todavía no hay mensajes para responder o moderar. Esperá a que aparezcan en el chat."); return; }
    static char lines[24][300];
    char *items[24];
    for (int i = 0; i < k; i++) { const tchat_t *m = &msgs[idx[i]]; snprintf(lines[i], sizeof(lines[i]), "%s%s\n%s", m->name, m->badge & CB_OWNER ? " (canal)" : m->badge & CB_MOD ? " (mod)" : "", m->text); items[i] = lines[i]; }
    int sel = 0;
    if (sui_list("Elegí un mensaje", "A: elegir   B: volver", items, k, &sel) != WPAD_BUTTON_A) return;
    tchat_t m = msgs[idx[sel]];
    int mod = auth_scope("moderation:chat_message:manage") || auth_scope("moderation:ban");
    int am = (chat_my_badges() & (CB_MOD | CB_OWNER)) || is_owner_here(s);
    char it[6][160], *acts[6];
    int na = 0, map[6];
    snprintf(it[na], 160, "Responder a %s\nTu mensaje queda enlazado a este", m.name); map[na++] = 0;
    snprintf(it[na], 160, "Mencionar a @%s\nEmpieza un mensaje con @%s", m.name, m.name); map[na++] = 1;
    if (mod) {
        const char *who = am ? "Sos moderador/dueño aquí" : "Solo funciona si sos moderador de este canal";
        snprintf(it[na], 160, "Borrar este mensaje\n%s", who); map[na++] = 2;
        snprintf(it[na], 160, "Expulsar 10 minutos\n%s", who); map[na++] = 3;
        snprintf(it[na], 160, "Banear del chat\n%s", who); map[na++] = 4;
    } else { snprintf(it[na], 160, "Moderación no disponible\nTu sesión no tiene permisos de moderador"); map[na++] = 5; }
    for (int i = 0; i < na; i++) acts[i] = it[i];
    int a = 0;
    char title[80];
    snprintf(title, sizeof(title), "Mensaje de %s", m.name);
    if (sui_list(title, "A: elegir   B: volver", acts, na, &a) != WPAD_BUTTON_A) return;
    switch (map[a]) {
    case 0:
        if (!m.id[0]) { sui_message("Responder", "Este mensaje todavía no tiene ID de Kick (por ejemplo, tu propio eco local). Probá de nuevo en unos segundos."); break; }
        auth_compose_ex(s->slug, s->broadcaster_user_id, m.id, m.name, NULL); break;
    case 1: { char pre[40]; snprintf(pre, sizeof(pre), "@%s ", m.name); auth_compose_ex(s->slug, s->broadcaster_user_id, NULL, NULL, pre); break; }
    case 2: moderate(s, &m, 0); break;
    case 3: moderate(s, &m, 1); break;
    case 4: moderate(s, &m, 2); break;
    default: need_scope("Moderación", "moderation:chat_message:manage"); break;
    }
}
