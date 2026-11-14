/* Public Kick chat (read side). Negotiate the provider (Centrifugo since Oct 2026),
   retaining Pusher support if Kick offers it again. Posting goes through the official API (auth.c).
   RC5: keeps message/sender ids + badges (reply/moderation), emotes shown as :name:, own echo
   merged with the real message, and live follow/sub notices polled from the developer relay. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gccore.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/lwp_watchdog.h>
#include "tv.h"
#include "chat_net.h"
#include <strings.h>
#define NMSG 24
static tchat_t ring[NMSG];static int head,count,mtx_ok;
static volatile int run,gen;static mutex_t mtx;static lwp_t th=LWP_THREAD_NULL;
static unsigned long channel_id,room_id;static long last_http;static char status[96];
static void clear_messages(void){LWP_MutexLock(mtx);head=count=0;gen++;LWP_MutexUnlock(mtx);}
#define NEMO 48
static unsigned long emo_id[NEMO];static char emo_name[NEMO][32];static int emo_n;
static char me[64];static volatile int my_badges;
static void emote_seen(unsigned long id,const char*name){if(!id||!name[0])return;for(int i=0;i<emo_n;i++)if(emo_id[i]==id)return;int k=emo_n<NEMO?emo_n++:(int)(id%NEMO);emo_id[k]=id;snprintf(emo_name[k],32,"%s",name);}
/* "[emote:123:KEKW]" -> ":KEKW:" (la fuente de la Wii no dibuja imagenes en el chat). */
void emote_display(const char*in,char*out,int cap){int j=0;for(int i=0;in[i]&&j<cap-1;){if(!strncmp(in+i,"[emote:",7)){const char*a=in+i+7,*c1=strchr(a,':'),*e=c1?strchr(c1+1,']'):NULL;if(c1&&e&&e-c1<34&&c1>a&&c1-a<16){char nm[34];int n=(int)(e-c1-1);memcpy(nm,c1+1,n);nm[n]=0;if(mtx_ok)emote_seen(strtoul(a,NULL,10),nm);int k=snprintf(out+j,cap-j,":%s:",nm);if(k<0||k>=cap-j)break;j+=k;i=(int)(e-in)+1;continue;}}out[j++]=in[i++];}out[j]=0;}
static void push_full(const char *name,const char *text,const char *color,const char*id,unsigned long uid,int badge,int echo,int sys) {
 char shown[200];LWP_MutexLock(mtx);emote_display(text,shown,sizeof(shown));
 if(!echo&&!sys&&id&&id[0])for(int k=0;k<count&&k<8;k++){tchat_t*o=&ring[(head+count-1-k)%NMSG];if(o->echo&&!strcasecmp(o->name,name)&&!strcmp(o->text,shown)){snprintf(o->id,sizeof(o->id),"%s",id);o->uid=uid;o->badge=badge;o->echo=0;gen++;LWP_MutexUnlock(mtx);return;}}
 if(count==NMSG){head=(head+1)%NMSG;count--;}
 tchat_t *m=&ring[(head+count++)%NMSG];memset(m,0,sizeof(*m));snprintf(m->name,sizeof(m->name),"%s",name);snprintf(m->text,sizeof(m->text),"%s",shown);m->sys=sys;
 if(id)snprintf(m->id,sizeof(m->id),"%s",id);m->uid=uid;m->badge=badge;m->echo=echo;
 m->color=color[0]=='#'?strtoul(color+1,NULL,16)&0xffffff:C_ACCENT;
 if(((m->color>>16)*3+((m->color>>8)&255)*6+(m->color&255))<600)m->color=gfx_mix(m->color,0xffffff,110);
 gen++;LWP_MutexUnlock(mtx);
}
static void push(const char *name,const char *text,const char *color){push_full(name,text,color,NULL,0,0,0,0);}
void chat_notice(const char*text){if(mtx_ok)push_full("",text,"#53FC18",NULL,0,0,0,1);}
int chat_seen_emotes(unsigned long*ids,char (*names)[32],int max){if(!mtx_ok)return 0;LWP_MutexLock(mtx);int n=emo_n<max?emo_n:max;for(int i=0;i<n;i++){ids[i]=emo_id[i];snprintf(names[i],32,"%s",emo_name[i]);}LWP_MutexUnlock(mtx);return n;}
int chat_my_badges(void){return my_badges;}
static int badge_bits(const char*identity){int b=0;const char*arr=identity?jget(identity,"badges"):NULL;for(const char*e=arr?jarr_first(arr):NULL;e;e=jarr_next(e)){char t[32];const char*v=jget(e,"type");t[0]=0;if(v&&!jis_null(v))jstr(v,t,sizeof(t));if(!strcmp(t,"moderator"))b|=CB_MOD;else if(!strcmp(t,"broadcaster"))b|=CB_OWNER;else if(!strcmp(t,"subscriber")||!strcmp(t,"founder"))b|=CB_SUB;else if(!strcmp(t,"vip"))b|=CB_VIP;}return b;}
typedef struct {char *data;size_t len,cap;} response;
static size_t write_response(char *p,size_t s,size_t n,void *u) {
 response *r=u;size_t k=s*n;if(k>r->cap-r->len)return 0;memcpy(r->data+r->len,p,k);r->len+=k;r->data[r->len]=0;return k;
}
static int public_post(const char *url,const char *body,char *out,int size) {
 CURL *c=curl_easy_init();if(!c)return 0;char err[CURL_ERROR_SIZE];response r={out,0,(size_t)size-1};out[0]=0;
 sh_curl_common(c,err);curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_POSTFIELDS,body);
 curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,write_response);curl_easy_setopt(c,CURLOPT_WRITEDATA,&r);
 curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,5L);curl_easy_setopt(c,CURLOPT_TIMEOUT,8L);curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
 chat_net progress={.running=&run};curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,cn_progress);curl_easy_setopt(c,CURLOPT_XFERINFODATA,&progress);
 struct curl_slist *h=NULL;h=curl_slist_append(h,"Content-Type: application/json");h=curl_slist_append(h,"Origin: https://kick.com");h=curl_slist_append(h,"Accept: application/json");curl_easy_setopt(c,CURLOPT_HTTPHEADER,h);
 CURLcode rc=curl_easy_perform(c);long code=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&code);curl_slist_free_all(h);curl_easy_cleanup(c);last_http=code;
 if(rc!=CURLE_OK||code!=200){snprintf(status,sizeof(status),"Chat Kick: HTTP %ld / curl %d",code,rc);return 0;}return 1;
}
static void copy_field(const char *o,const char *key,char *out,int size){const char *v=o?jget(o,key):NULL;out[0]=0;if(v&&!jis_null(v))jstr(v,out,size);}
/* Quita del chat local un mensaje (id) o todos los de un usuario (who); usado por eventos de Kick y por la moderación desde la Wii. */
void chat_forget(const char*mid,const char*who){if(!mtx_ok)return;if(!mid)mid="";if(!who)who="";if(!mid[0]&&!who[0])return;LWP_MutexLock(mtx);int w=0;for(int k=0;k<count;k++){tchat_t m=ring[(head+k)%NMSG];if((mid[0]&&!strcmp(m.id,mid))||(who[0]&&!m.sys&&!strcasecmp(m.name,who)))continue;ring[(head+w)%NMSG]=m;w++;}count=w;gen++;LWP_MutexUnlock(mtx);}
static int process_event(chat_net *n,char *json,int provider,int *subscribed) {
 const char *err=jget(json,"error");if(err&&!jis_null(err)){snprintf(status,sizeof(status),"Kick rechazo chat publico; reintento con espera");return -2;}
 if(!strcmp(jws(json),"{}")){cn_send(n,"{}");return 1;}
 char event[128];const char *env=json;
 if(provider==1) {
  const char *connect=jget(json,"connect");
  if(connect){char sub[160];snprintf(sub,sizeof(sub),"{\"id\":2,\"subscribe\":{\"channel\":\"chatrooms.%lu.v2\"}}",room_id);return cn_send(n,sub);}
  if(jget(json,"subscribe")){*subscribed=1;snprintf(status,sizeof(status),"Kick WSS: conectado (solo lectura)");}
  env=jpath(json,"push.pub.data");if(!env)return 1;
 }
 copy_field(env,"event",event,sizeof(event));
 if(!strcmp(event,"pusher:error")){snprintf(status,sizeof(status),"Kick retiro este servidor Pusher");return -2;}
 if(!strcmp(event,"pusher:ping"))return cn_send(n,"{\"event\":\"pusher:pong\",\"data\":{}}");
 if(!strcmp(event,"pusher:connection_established")){char sub[200];snprintf(sub,sizeof(sub),"{\"event\":\"pusher:subscribe\",\"data\":{\"auth\":\"\",\"channel\":\"chatrooms.%lu.v2\"}}",room_id);return cn_send(n,sub);}
 if(!strcmp(event,"pusher_internal:subscription_succeeded")){*subscribed=1;snprintf(status,sizeof(status),"Kick Pusher: conectado");}
 if(strstr(event,"ChatroomClearEvent")){clear_messages();return 1;}
 if(strstr(event,"MessageDeletedEvent")||strstr(event,"UserBannedEvent")){
  const char *dd=jget(env,"data");char dec[4096];if(dd&&*jws(dd)=='"'){jstr(dd,dec,sizeof(dec));dd=dec;}
  char mid[48]="",who[48]="";if(dd){copy_field(jget(dd,"message"),"id",mid,sizeof(mid));copy_field(jget(dd,"user"),"username",who,sizeof(who));}
  if(!mid[0]&&!who[0]){clear_messages();return 1;}
  chat_forget(mid,who);return 1;
 }
 if(!strstr(event,"ChatMessageEvent"))return 1;
 const char *data=jget(env,"data");if(!data)return 1;
 char decoded[16384];if(*jws(data)=='"'){jstr(data,decoded,sizeof(decoded));data=decoded;}
 char name[48],text[512],color[16],id[48];copy_field(jget(data,"sender"),"username",name,sizeof(name));copy_field(data,"content",text,sizeof(text));copy_field(jpath(data,"sender.identity"),"color",color,sizeof(color));copy_field(data,"id",id,sizeof(id));
 const char*sid=jpath(data,"sender.id");unsigned long uid=sid&&!jis_null(sid)?(unsigned long)jnum(sid):0;int badge=badge_bits(jpath(data,"sender.identity"));
 for(char*q=id;*q;q++)if(!((*q>='0'&&*q<='9')||(*q>='a'&&*q<='f')||(*q>='A'&&*q<='F')||*q=='-')){id[0]=0;break;}
 if(me[0]&&!strcasecmp(name,me))my_badges=badge;
 if(name[0]&&text[0])push_full(name,text,color,id,uid,badge,0,0);
 return 1;
}
static int provider_url(const char *url,int provider) {
 if(strncmp(url,"wss://",6))return 0;
 const char *host=url+6,*slash=strchr(host,'/');if(!slash)return 0;
 const char *suffix=provider==1?".platform.kick.com":".pusher.com";
 size_t n=strlen(suffix),len=slash-host;if(len<=n||memcmp(slash-n,suffix,n))return 0;
 const char *c;for(c=host;c<slash;c++)if(!((*c>='a'&&*c<='z')||(*c>='0'&&*c<='9')||*c=='.'||*c=='-'))return 0;
 return provider==1?!strncmp(host,"realtime.",9)&&!strcmp(slash,"/connection/websocket"):!strncmp(host,"ws-",3)&&!strncmp(slash,"/app/",5);
}
static void *chat_thread(void *unused) {
 (void)unused;chat_net *net=calloc(1,sizeof(*net));char *response=malloc(16384);if(!net||!response){free(net);free(response);return NULL;}
 int failures=0;
 while(run) {
  char uuid[64],url[768],body[512],token[4096];unsigned long t=(unsigned long)gettime();
  snprintf(uuid,sizeof(uuid),"%08lx-%04x-4%03x-%04x-%08x%04x",t,rand()&65535,rand()&4095,0x8000|(rand()&16383),rand(),rand()&65535);
  snprintf(url,sizeof(url),"https://web.kick.com/api/v1/realtime/channels/%lu/chat/connection",channel_id);
  snprintf(body,sizeof(body),"{\"client\":{\"id\":\"%s\",\"type\":\"web\"},\"capabilities\":{\"accepted_providers\":[{\"provider\":\"centrifugo\"},{\"provider\":\"pusher\"}]}}",uuid);
  snprintf(status,sizeof(status),"Conectando chat publico Kick...");int ok=public_post(url,body,response,16384),provider=0,terminal=0,subscribed=0;
  if(ok){const char *arr=jpath(response,"data.connections"),*c;
   for(c=arr?jarr_first(arr):NULL;c;c=jarr_next(c)) {
    char p[32];copy_field(c,"provider",p,sizeof(p));
    if(!strcmp(p,"centrifugo")){provider=1;copy_field(jget(c,"credentials"),"url",url,sizeof(url));break;}
    if(!strcmp(p,"pusher")){char key[80],cluster[24];copy_field(jget(c,"credentials"),"app_key",key,sizeof(key));copy_field(jget(c,"credentials"),"cluster",cluster,sizeof(cluster));snprintf(url,sizeof(url),"wss://ws-%s.pusher.com/app/%s?protocol=7&client=js&version=8.4.0&flash=false",cluster,key);provider=2;break;}
   }
  }
  if(!ok&&(last_http==401||last_http==403||last_http==429))terminal=1;
  if(ok&&provider==1){snprintf(body,sizeof(body),"{\"client_id\":\"%s\"}",uuid);ok=public_post("https://web.kick.com/api/v1/realtime/auth/connection",body,response,16384);copy_field(jget(response,"data"),"token",token,sizeof(token));ok=ok&&token[0];if(!ok&&(last_http==401||last_http==403||last_http==429))terminal=1;}
  /* Only the public provider hosts are eligible; never follow arbitrary API targets. */
  int allowed=provider&&provider_url(url,provider);
  if(ok&&allowed&&cn_open(net,url,1,"https://kick.com",&run)) {
   if(provider==1){char intro[4608];snprintf(intro,sizeof(intro),"{\"id\":1,\"connect\":{\"token\":\"%s\",\"name\":\"js\"}}",token);ok=cn_send(net,intro);memset(token,0,sizeof(token));}
   u64 started=gettime(),last=started;unsigned batch=0;
   while(run&&ok) {
    int rc=cn_message(net);if(rc<0)break;
    if(rc>0){last=gettime();char *save,*line=strtok_r(net->msg,"\n",&save);while(line){int result=process_event(net,line,provider,&subscribed);if(result<=0){ok=0;terminal=result==-2;break;}line=strtok_r(NULL,"\n",&save);}}
    else usleep(20000);
    if(++batch%8==0)usleep(5000);
    u64 now=gettime();if(ticks_to_millisecs(now-last)>90000)break;
    if(!subscribed&&ticks_to_millisecs(now-started)>15000)break;
    if(ticks_to_millisecs(now-started)>35*60*1000)break; /* renew anonymous guest session before expiry */
   }
   cn_close(net);
  }
  if(!run)break;
  if(subscribed)failures=0;else if(failures<5)failures++;
  int wait=terminal?300:(1<<failures);snprintf(status,sizeof(status),"Chat desconectado; reintento en %d s",wait);
  int i;for(i=0;i<wait*10&&run;i++)usleep(100000);
 }
 cn_close(net);free(net);free(response);return NULL;
}
/* ---- avisos en vivo (seguidores/suscripciones) desde el relay del desarrollador ---- */
static lwp_t nth=LWP_THREAD_NULL;static volatile int nrun;static unsigned long nb;static char nrelay[400];
static int relay_rc;
static long relay_call(const char*url,const char*post,char*out,int size){
 CURL*c=curl_easy_init();if(!c)return 0;char err[CURL_ERROR_SIZE];response r={out,0,(size_t)size-1};out[0]=0;sh_curl_common(c,err);
 curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,0L);
#ifdef AUTH_MOCK
 curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"HTTP,HTTPS");
#else
 curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"HTTPS");
#endif
 if(post)curl_easy_setopt(c,CURLOPT_POSTFIELDS,post);curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,write_response);curl_easy_setopt(c,CURLOPT_WRITEDATA,&r);
 curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,20L);curl_easy_setopt(c,CURLOPT_TIMEOUT,40L);curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
 /* chat_net lleva un buffer de 32 KB: no va en la pila del hilo de avisos (rompía el TLS en la Wii real). */
 static chat_net progress;progress.running=&nrun;curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,cn_progress);curl_easy_setopt(c,CURLOPT_XFERINFODATA,&progress);
 struct curl_slist*h=curl_slist_append(NULL,"Accept: application/json");curl_easy_setopt(c,CURLOPT_HTTPHEADER,h);
 CURLcode rc=curl_easy_perform(c);long code=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&code);curl_slist_free_all(h);curl_easy_cleanup(c);relay_rc=(int)rc;return rc==CURLE_OK?code:0;
}
static void nsleep(int sec){for(int i=0;i<sec*10&&nrun;i++)usleep(100000);}
static void notice_line(const char*ev){
 char kind[16],user[40],line[200];copy_field(ev,"kind",kind,sizeof(kind));copy_field(ev,"user",user,sizeof(user));if(!user[0])return;
 for(char*q=user;*q;q++)if((unsigned char)*q<32)*q=' ';
 long n=(long)jnum(jget(ev,"count")),months=(long)jnum(jget(ev,"months"));
 if(!strcmp(kind,"follow"))snprintf(line,sizeof(line),"\xc2\xbb %s empez\xc3\xb3 a seguir el canal",user);
 else if(!strcmp(kind,"sub"))snprintf(line,sizeof(line),"\xc2\xbb %s se suscribi\xc3\xb3",user);
 else if(!strcmp(kind,"resub")){if(months>1)snprintf(line,sizeof(line),"\xc2\xbb %s renov\xc3\xb3 su suscripci\xc3\xb3n (%ld meses)",user,months);else snprintf(line,sizeof(line),"\xc2\xbb %s renov\xc3\xb3 su suscripci\xc3\xb3n",user);}
 else if(!strcmp(kind,"gift"))snprintf(line,sizeof(line),"\xc2\xbb %s regal\xc3\xb3 %ld suscripci%s",user,n>0?n:1,n==1?"\xc3\xb3n":"ones");
 else return;
 push_full("",line,"#53FC18",NULL,0,0,0,1);
}
static void *notice_thread(void*unused){
 (void)unused;char *buf=malloc(8192);if(!buf)return NULL;long seq=-1;u64 watched=0;int said=0;
 while(nrun){
  char url[512],body[64];u64 now=gettime();
  if(!watched||ticks_to_millisecs(now-watched)>25*60*1000){
   snprintf(url,sizeof(url),"%s/events/watch",nrelay);snprintf(body,sizeof(body),"broadcaster_user_id=%lu",nb);long c=relay_call(url,body,buf,8192);
   if(!nrun)break;
   if(c==200){watched=gettime();if(seq<0)seq=(long)jnum(jget(buf,"seq"));if(!said){said=1;push_full("","\xc2\xbb Avisos de seguidores y suscripciones activados","#7f8a8f",NULL,0,0,0,1);}}
   else{if(!said){said=1;char t[160];snprintf(t,sizeof(t),c==503?"Avisos de seguidores/suscripciones no disponibles por ahora (lo activa el desarrollador).":c?"Avisos de seguidores/suscripciones: el relay respondió HTTP %ld; reintento en un minuto.":"Avisos de seguidores/suscripciones: sin conexión con el relay (red %ld); reintento en un minuto.",c?c:(long)relay_rc);push_full("",t,"#7f8a8f",NULL,0,0,0,1);}nsleep(c==503?600:60);continue;}
  }
  snprintf(url,sizeof(url),"%s/events/poll?b=%lu&after=%ld",nrelay,nb,seq<0?0:seq);long c=relay_call(url,NULL,buf,8192);
  if(c==200){long ns=(long)jnum(jget(buf,"seq"));const char*arr=jget(buf,"events");if(seq>=0)for(const char*e=arr?jarr_first(arr):NULL;e;e=jarr_next(e))notice_line(e);seq=ns;const char*act=jget(buf,"active");if(act&&!strncmp(jws(act),"false",5))watched=0;}
  else if(c==503)watched=0;
  nsleep(c==200?15:45);
 }
 free(buf);return NULL;
}
static void notices_stop(void){if(nth!=LWP_THREAD_NULL){nrun=0;LWP_JoinThread(nth,NULL);nth=LWP_THREAD_NULL;}}
static void notices_start(unsigned long b){notices_stop();const char*r=auth_relay();if(!b||!r||!r[0]||strlen(r)>=sizeof(nrelay))return;snprintf(nrelay,sizeof(nrelay),"%s",r);nb=b;nrun=1;if(LWP_CreateThread(&nth,notice_thread,NULL,NULL,160*1024,50)<0){nrun=0;nth=LWP_THREAD_NULL;}}
void chat_stop(void){notices_stop();if(th!=LWP_THREAD_NULL){run=0;LWP_JoinThread(th,NULL);th=LWP_THREAD_NULL;}}
void chat_start(const kstream_t *s){chat_stop();if(!mtx_ok){LWP_MutexInit(&mtx,false);mtx_ok=1;}clear_messages();my_badges=0;LWP_MutexLock(mtx);emo_n=0;LWP_MutexUnlock(mtx);snprintf(me,sizeof(me),"%s",auth_login());notices_start(s->broadcaster_user_id);channel_id=s->channel_id;room_id=s->chatroom_id;if(!channel_id||!room_id){snprintf(status,sizeof(status),"Chatroom no disponible");return;}run=1;if(LWP_CreateThread(&th,chat_thread,NULL,NULL,192*1024,48)<0){run=0;th=LWP_THREAD_NULL;}}
int chat_gen(void){return gen;}
const char *chat_status(void){return status;}
int chat_get(tchat_t *out,int max){if(!mtx_ok)return 0;LWP_MutexLock(mtx);int i,n=count<max?count:max;for(i=0;i<n;i++)out[i]=ring[(head+count-n+i)%NMSG];LWP_MutexUnlock(mtx);return n;}

void chat_local_echo(const char*name,const char*text){if(mtx_ok)push_full(name,text,"#53FC18",NULL,0,my_badges,1,0);}
int chat_send_text(const char*text){(void)text;return 0;}
