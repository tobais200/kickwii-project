/* rc4d: background native public catalog. No login, cookies, proxy or identity
 * changes. Metadata is capped at 2048; only visible images enter the 24-slot LRU.
 * Main thread receives copies/indices, never pointers into worker-owned memory.
 * Public legacy API currently clamps requested page 21 to page 20: stop and
 * explain the limit rather than looping/repeating or claiming all 18k entries. */
#include "tv.h"
#include "catalog.h"
#include <gccore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ogc/mutex.h>
#include <unistd.h>
#include <ctype.h>
static kc_category *items;
static kc_status state;
static mutex_t mutex;
static int initialized,paused;
static volatile int cancelled;
static lwp_t worker=LWP_THREAD_NULL;
static int contains(const char *s,const char *q) {
    if(!q||!*q)return 1;
    for(;*s;s++) {const char *a=s,*b=q;while(*a&&*b&&tolower((unsigned char)*a)==tolower((unsigned char)*b)){a++;b++;}if(!*b)return 1;}
    return 0;
}
static void *scan(void *unused) {
    (void)unused;kc_category *page=calloc(KC_PAGE_SIZE,sizeof(*page));
    if(!page){LWP_MutexLock(mutex);snprintf(state.error,sizeof(state.error),"Sin memoria para categorias.");state.done=1;state.running=0;state.gen++;LWP_MutexUnlock(mutex);return NULL;}
    unsigned long previous_hash=0;int previous_n=0;
    for(int p=1;p<=128&&!cancelled;p++) {
        while(!cancelled){LWP_MutexLock(mutex);int wait=paused;LWP_MutexUnlock(mutex);if(!wait)break;usleep(100000);}
        if(cancelled)break;
        kc_page meta={0};char err[200]="";
        int n=kw_categories_page(p,page,KC_PAGE_SIZE,&meta,err,sizeof(err),&cancelled);
        /* One bounded retry for transport failures only; NEVER retry HTTP
         * 403/429, certificate failures, or change headers/identity/verification. */
        if(n<0 && meta.retryable && !cancelled) {
            usleep(1000000);
            if(!cancelled)n=kw_categories_page(p,page,KC_PAGE_SIZE,&meta,err,sizeof(err),&cancelled);
        }
        if(cancelled)break;
        unsigned long hash=2166136261UL;
        for(int i=0;i<n;i++)hash=(hash^page[i].id)*16777619UL;
        LWP_MutexLock(mutex);
        if(n<0) {snprintf(state.error,sizeof(state.error),"%s",err);state.done=1;}
        else if(meta.page!=p || (p>1&&n&&n==previous_n&&hash==previous_hash)) {
            state.limited=state.done=1;
            snprintf(state.error,sizeof(state.error),"Limite de Kick: %d paginas disponibles (sin repetir).",state.pages);
        } else {
            state.total=meta.total;state.pages=p;int added=0;
            for(int i=0;i<n;i++) {
                int j;for(j=0;j<state.count;j++)if(items[j].id==page[i].id)break;
                if(j<state.count)continue;
                if(state.count>=KC_CAP){state.limited=state.done=1;snprintf(state.error,sizeof(state.error),"Limite de memoria: %d categorias.",KC_CAP);break;}
                items[state.count++]=page[i];added++;
            }
            if(!n || p>=meta.last)state.done=1;
            if(n&&!added){state.limited=state.done=1;snprintf(state.error,sizeof(state.error),"Kick repitio la pagina; se detuvo la carga.");}
            previous_hash=hash;previous_n=n;
        }
        state.gen++;int done=state.done;LWP_MutexUnlock(mutex);
        if(done)break;
        usleep(100000); /* bounded cooperative incremental requests */
    }
    free(page);LWP_MutexLock(mutex);state.running=0;state.done=1;state.gen++;LWP_MutexUnlock(mutex);return NULL;
}
int kc_start(void) {
    if(!initialized){if(LWP_MutexInit(&mutex,false))return 0;initialized=1;}
    LWP_MutexLock(mutex);if(state.started){LWP_MutexUnlock(mutex);return 1;}
    items=calloc(KC_CAP,sizeof(*items));memset(&state,0,sizeof(state));
    if(!items){snprintf(state.error,sizeof(state.error),"Sin memoria para categorias.");LWP_MutexUnlock(mutex);return 0;}
    state.started=state.running=1;state.gen=1;cancelled=paused=0;LWP_MutexUnlock(mutex);
    if(LWP_CreateThread(&worker,scan,NULL,NULL,192*1024,30)){LWP_MutexLock(mutex);free(items);items=NULL;state.started=state.running=0;snprintf(state.error,sizeof(state.error),"No se pudo iniciar la carga de categorias.");LWP_MutexUnlock(mutex);return 0;}
    return 1;
}
void kc_stop(void) {
    if(!initialized)return;
    cancelled=1;if(worker!=LWP_THREAD_NULL){LWP_JoinThread(worker,NULL);worker=LWP_THREAD_NULL;}
    LWP_MutexLock(mutex);free(items);items=NULL;memset(&state,0,sizeof(state));LWP_MutexUnlock(mutex);
}
void kc_pause(int p){if(initialized){LWP_MutexLock(mutex);paused=p;LWP_MutexUnlock(mutex);}}
void kc_status_get(kc_status *out){memset(out,0,sizeof(*out));if(initialized){LWP_MutexLock(mutex);*out=state;LWP_MutexUnlock(mutex);}}
static int compare(const void *aa,const void *bb){int a=*(const int*)aa,b=*(const int*)bb;if(items[a].viewers!=items[b].viewers)return items[a].viewers>items[b].viewers?-1:1;int r=strcasecmp(items[a].name,items[b].name);return r?r:(items[a].id>items[b].id)-(items[a].id<items[b].id);}
int kc_view(int *out,int cap,int mature,unsigned long group,const char *q){
    if(!initialized)return 0;
    int n=0;LWP_MutexLock(mutex);
    for(int i=0;i<state.count&&n<cap;i++)if((mature||!items[i].mature)&&(!group||group==items[i].group_id)&&contains(items[i].name,q))out[n++]=i;
    qsort(out,n,sizeof(*out),compare);LWP_MutexUnlock(mutex);return n;
}
int kc_get(int index,kc_category *out){int ok=0;if(!initialized)return 0;LWP_MutexLock(mutex);if(items&&index>=0&&index<state.count){*out=items[index];ok=1;}LWP_MutexUnlock(mutex);return ok;}
int kc_groups(kc_group *out,int cap,int mature){int n=0;if(!initialized)return 0;LWP_MutexLock(mutex);
    for(int i=0;i<state.count&&n<cap;i++){if(!items[i].group_id||!items[i].group[0]||(!mature&&items[i].mature))continue;int j;for(j=0;j<n;j++)if(out[j].id==items[i].group_id)break;if(j<n)continue;out[n].id=items[i].group_id;snprintf(out[n++].name,sizeof(out[0].name),"%s",items[i].group);}
    LWP_MutexUnlock(mutex);return n;
}
