/* Publisher configuration. Public IDs only; NEVER put client secrets here.
 * Empty = friendly unavailable screen. Configure once, rebuild for all users. */
#ifndef OAUTH_CONFIG_H
#define OAUTH_CONFIG_H
#define OAUTH_TWITCH_CLIENT_ID "" /* TODO: registered Twitch PUBLIC app Client ID */
#define OAUTH_KICK_CLIENT_ID "01M6YPWH29S2RVC47XXSV5CFV9" /* KickWii app (public Client ID) */
#define OAUTH_KICK_RELAY_URL "https://kickwii-relay.kickwii.workers.dev" /* Cloudflare Worker relay */
#define DEFAULT_TWITCH_CLIENT_ID OAUTH_TWITCH_CLIENT_ID
#define DEFAULT_KICK_RELAY_URL OAUTH_KICK_RELAY_URL
#endif
