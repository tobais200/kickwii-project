#ifndef WII_AUTH_H
#define WII_AUTH_H
#include <stddef.h>
void auth_init(void);
int auth_configured(void);
const char *auth_client_id(void);
int auth_ready(void);
int auth_has_session(void);
const char *auth_login(void);
const char *auth_status(void);
const char *auth_url(void);
const char *auth_code(void);
int auth_begin(void);
int auth_poll(void);
int auth_import_callback(const char *url);
int auth_import_file(void);
int auth_ensure(void);
int auth_due(void);
int auth_refresh_now(void);
void auth_logout(void);
int auth_irc_snapshot(char *token,int ts,char *login,int ls);
int auth_kick_send(unsigned long broadcaster,const char *text);
int auth_kick_send_reply(unsigned long broadcaster,const char *text,const char *reply_to);
long auth_api(const char *method,const char *path,const char *json,char *out,size_t cap);
int auth_json_escape(const char *in,char *out,size_t cap);
int auth_scope(const char *scope);
const char *auth_scopes(void);
const char *auth_avatar(void);
unsigned long auth_uid(void);
const char *auth_relay(void);
void auth_compose_ex(const char *channel,unsigned long broadcaster,const char *reply_id,const char *reply_name,const char *prefill);
void account_screen(void);
void mychannel_screen(void);
int auth_message_valid(const char *text);
void auth_screen(void);
void auth_compose(const char *channel,unsigned long broadcaster);
int chat_send_text(const char *text);
void chat_local_echo(const char *name,const char *text);
#ifdef AUTH_MOCK
extern int auth_test_stage;
int auth_mock_complete(void);
int auth_mock_prepare(void);
#endif
#endif
