/* shottour.c - screenshot tour for Dolphin tests (built only with -DSHOTTOUR; never in a release). */
#ifdef SHOTTOUR
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>
#include "gfx.h"
#include "tv.h"
#include "i18n.h"
void gfx_dump_xfb(const char *path);
void in_hide_ptr(void);
enum { P, H, B, S, E };
typedef struct { int ms, op, a, b, acc; const char *name; } st_t;
#define W(ms) {ms,E,0,0,0,0}
static const st_t tour[] = {
    {31000,H,0,0,0,0},{1000,S,0,0,0,"01-home"},
    {500,P,268,173,0,0},{1200,B,WPAD_BUTTON_A,0,0,0},{300,H,0,0,0,0},{16000,S,0,0,0,"02-channel"},
    {500,P,341,334,0,0},{1200,B,WPAD_BUTTON_A,0,0,0},{300,H,0,0,0,0},{30000,S,0,0,0,"03-watch"},
    {500,H,0,0,0,0},{500,B,WPAD_BUTTON_B,0,0,0},{6000,P,60,24,0,0},{1200,B,WPAD_BUTTON_A,0,0,0},{300,H,0,0,0,0},
    {6000,B,WPAD_BUTTON_PLUS,0,0,0},{16000,S,0,0,0,"04-browse"},
    {500,P,211,190,0,0},{1200,B,WPAD_BUTTON_A,0,0,0},{300,H,0,0,0,0},{15000,S,0,0,0,"05-category"},
    {500,P,132,24,0,0},{1200,B,WPAD_BUTTON_A,0,0,0},{300,H,0,0,0,0},{8000,H,0,0,0,0},{500,B,WPAD_BUTTON_MINUS,0,0,0},{4000,S,0,0,0,"06-groups"},
    {500,B,WPAD_BUTTON_B,0,0,0},{3000,P,60,24,0,0},{1200,B,WPAD_BUTTON_A,0,0,0},{300,H,0,0,0,0},{7000,H,0,0,0,0},
    {500,B,WPAD_BUTTON_1,0,0,0},{4000,S,0,0,0,"07-keyboard"},
    {400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},
    {400,B,WPAD_BUTTON_RIGHT,0,1,0},{400,B,WPAD_BUTTON_RIGHT,0,1,0},{400,B,WPAD_BUTTON_RIGHT,0,1,0},{400,B,WPAD_BUTTON_RIGHT,0,1,0},
    {400,B,WPAD_BUTTON_A,0,1,0},{1500,S,0,0,1,"08-keyboard-accent-on"},
    {400,B,WPAD_BUTTON_UP,0,1,0},{400,B,WPAD_BUTTON_UP,0,1,0},{400,B,WPAD_BUTTON_UP,0,1,0},{400,B,WPAD_BUTTON_LEFT,0,1,0},{400,B,WPAD_BUTTON_LEFT,0,1,0},
    {400,B,WPAD_BUTTON_A,0,1,0},{1500,S,0,0,1,"09-keyboard-accent-typed"},
    {500,B,WPAD_BUTTON_MINUS,0,0,0},{3000,P,-210,24,0,0},{1200,B,WPAD_BUTTON_A,0,0,0},{300,H,0,0,0,0},{10000,S,0,0,0,"10-signin"},
    {500,H,0,0,0,0},{500,B,WPAD_BUTTON_B,0,0,0},{4000,B,WPAD_BUTTON_B,0,0,0},{3000,S,0,0,0,"11-settings"},
    {500,B,WPAD_BUTTON_A,0,0,0},{3000,S,0,0,0,"12-language"},
    {500,B,WPAD_BUTTON_B,0,0,0},{2000,B,WPAD_BUTTON_DOWN,0,0,0},{600,B,WPAD_BUTTON_A,0,0,0},{3000,S,0,0,0,"13-display"},
    {500,B,WPAD_BUTTON_B,0,0,0},{2000,B,WPAD_BUTTON_DOWN,0,0,0},{500,B,WPAD_BUTTON_DOWN,0,0,0},{500,B,WPAD_BUTTON_DOWN,0,0,0},{500,B,WPAD_BUTTON_DOWN,0,0,0},{500,B,WPAD_BUTTON_DOWN,0,0,0},{500,B,WPAD_BUTTON_DOWN,0,0,0},
    {600,B,WPAD_BUTTON_A,0,0,0},{3000,S,0,0,0,"14-help"},
    {400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},
    {1500,S,0,0,0,"15-help-end"},
    {500,B,WPAD_BUTTON_B,0,0,0},{2000,B,WPAD_BUTTON_DOWN,0,0,0},{600,B,WPAD_BUTTON_A,0,0,0},{3000,S,0,0,0,"16-about"},
    {400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},{400,B,WPAD_BUTTON_DOWN,0,0,0},
    {1500,S,0,0,0,"17-about-end"},
    {500,B,WPAD_BUTTON_B,0,0,0},{2000,B,WPAD_BUTTON_DOWN,0,0,0},{500,B,WPAD_BUTTON_DOWN,0,0,0},{1500,S,0,0,0,"18-settings-bottom"},
    {500,B,WPAD_BUTTON_A,0,0,0},{6000,S,0,0,0,"19-account"},
    {500,B,WPAD_BUTTON_B,0,0,0},{3000,B,WPAD_BUTTON_B,0,0,0},{3000,S,0,0,0,"20-home-end"},
    {2000,S,0,0,0,"done"},
};
static int idx = 0; static u64 t_prev = 0; static int pending = 0; static char pname[64];
static int has_accent(void) { return ui_lang==LANG_ES||ui_lang==LANG_FR||ui_lang==LANG_DE||ui_lang==LANG_IT||ui_lang==LANG_NL||ui_lang==LANG_PT; }
void shottour_present(void);
void shottour_tick(void) {
    u64 now = gettime();
    if (!t_prev) t_prev = now;
    while (idx < (int)(sizeof tour / sizeof tour[0])) {
        const st_t *s = &tour[idx];
        if (s->acc && !has_accent()) { idx++; continue; }
        if ((int)ticks_to_millisecs(now - t_prev) < s->ms) return;
        t_prev = now; idx++;
        switch (s->op) {
        case P: {
            int x = s->a, y = s->b;
            if (gfx_widescreen()) {   /* 16:9 home/channel/browse layouts */
                if (x == 268 && y == 173) { x = 261; y = 157; }
                else if (x == 341 && y == 334) { x = 346; y = 334; }
                else if (x == 211 && y == 190) { x = 208; y = 184; }
            }
            in_set_ptr(x < 0 ? SCR_W + x : x, y, 1); break; }
        case H: in_hide_ptr(); break;
        case B: in_inject_btn(s->a); break;
        case S: snprintf(pname, sizeof pname, "%s", s->name); pending = 1; shottour_present(); break;
        }
        return;
    }
}
void shottour_present(void) {
    if (!pending) return;
    pending = 0;
    char p[96];
    mkdir("sd:/kshots", 0777);
    snprintf(p, sizeof p, "sd:/kshots/%s.yz", pname);
    gfx_dump_xfb(p);
}
#endif
