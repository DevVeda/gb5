// RetroAchievements for the title (rcheevos rc_client), through RA Hub on the same console:
//   GET  127.0.0.1:8045/session    the signed-in user and Connect token (RA Hub signs in once)
//   POST 127.0.0.1:8045/dorequest  RA Hub forwards rcheevos' calls to RetroAchievements over HTTPS,
//                                  so the title needs no TLS of its own
// Progress and unlocks show as console notifications (top right).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ra.h"
#include "card.h"
#include "rc_client.h"
#include "rhash/md5.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void say(const char *fmt, ...);
uint32_t gba_ra_read(const void *m, uint32_t addr, uint8_t *out, uint32_t n);

typedef struct { char useless[45]; char message[3075]; } notify_request_t;  // as Payload Manager / RA Hub
int sceKernelSendNotificationRequest(int device, notify_request_t *req, size_t size, int unused);
int sceSystemServiceParamGetInt(int param_id, int *value);

static rc_client_t *client;
static const void *machine;
static int es;  // console in Spanish

static void notify(const char *fmt, ...) {
  notify_request_t nr = {0};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(nr.message, sizeof nr.message, fmt, ap);
  va_end(ap);
  say("ra: %s", nr.message);
  sceKernelSendNotificationRequest(0, &nr, sizeof nr, 0);
}

// One HTTP/1.0 exchange with RA Hub. Returns the malloc'd body (NUL-terminated, caller frees) and sets
// *status and *len, or NULL when RA Hub isn't running.
static char *hub(const char *method, const char *path, const char *body, int *status, size_t *len) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(8045), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
    if (s >= 0) close(s);
    return NULL;
  }
  size_t blen = body ? strlen(body) : 0;
  char head[256];
  int hl = snprintf(head, sizeof head,
                    "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\nX-RAHub: 1\r\n"  // X-RAHub: RA Hub's CSRF guard on POST
                    "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n\r\n",
                    method, path, blen);
  int ok = write(s, head, (size_t)hl) == hl && (!blen || write(s, body, blen) == (ssize_t)blen);
  size_t n = 0, cap = 16384;
  char *buf = ok ? malloc(cap + 1) : NULL;
  for (ssize_t r; buf && (r = read(s, buf + n, cap - n)) > 0;) {
    n += (size_t)r;
    if (n == cap) {
      char *bigger = cap < (8u << 20) ? realloc(buf, cap * 2 + 1) : NULL;  // ponytail: 8 MB cap per answer
      if (!bigger) { free(buf); buf = NULL; break; }
      buf = bigger;
      cap *= 2;
    }
  }
  close(s);
  if (!buf) return NULL;
  buf[n] = 0;
  char *sep = strstr(buf, "\r\n\r\n");
  *status = 0;
  sscanf(buf, "HTTP/%*s %d", status);
  if (!sep) { free(buf); return NULL; }
  *len = n - (size_t)(sep + 4 - buf);
  memmove(buf, sep + 4, *len + 1);
  return buf;
}

// GET from RA Hub, the body only when it answered 200 (binary-safe: badges are PNG).
char *ra_hub_get(const char *path, size_t *len) {
  int status = 0;
  char *b = hub("GET", path, NULL, &status, len);
  if (b && status != 200) { free(b); b = NULL; }
  return b;
}

// "key":"value" from RA Hub's flat /session JSON (plain ASCII: user names and tokens).
static int json_str(const char *js, const char *key, char *out, size_t n) {
  char pat[32];
  snprintf(pat, sizeof pat, "\"%s\":\"", key);
  const char *p = strstr(js, pat), *e;
  if (!p || !(e = strchr(p += strlen(pat), '"')) || (size_t)(e - p) >= n) return 0;
  memcpy(out, p, (size_t)(e - p));
  out[e - p] = 0;
  return 1;
}

// ---------------- rc_client plumbing ----------------
struct call {
  char *post;
  rc_client_server_callback_t cb;
  void *cb_data;
};

static void *call_main(void *arg) {
  struct call *c = arg;
  int status = 0;
  size_t len = 0;
  char *body = hub("POST", "/dorequest", c->post, &status, &len);
  rc_api_server_response_t resp = {0};
  resp.body = body ? body : "";
  resp.body_length = body ? len : 0;
  resp.http_status_code = body ? status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;  // hub gone: retry later
  c->cb(&resp, c->cb_data);
  free(body);
  free(c->post);
  free(c);
  return NULL;
}

// Every request runs on its own thread, so an unlock never stalls a frame.
// ponytail: thread per request; a queue if RA traffic ever gets chatty
static void server_call(const rc_api_request_t *req, rc_client_server_callback_t cb, void *cb_data, rc_client_t *c) {
  (void)c;
  struct call *job = malloc(sizeof *job);
  pthread_t t;
  if (job && strstr(req->url, "/dorequest.php") && (job->post = strdup(req->post_data ? req->post_data : ""))) {
    job->cb = cb;
    job->cb_data = cb_data;
    if (pthread_create(&t, NULL, call_main, job) == 0) {
      pthread_detach(t);
      return;
    }
    free(job->post);
  }
  free(job);
  rc_api_server_response_t resp = {0};
  resp.body = "";
  resp.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
  cb(&resp, cb_data);
}

static uint32_t read_memory(uint32_t addr, uint8_t *buf, uint32_t n, rc_client_t *c) {
  (void)c;
  return gba_ra_read(machine, addr, buf, n);
}

static void on_log(const char *msg, const rc_client_t *c) {
  (void)c;
  say("rcheevos: %s", msg);
}

static void on_event(const rc_client_event_t *e, rc_client_t *c) {
  (void)c;
  // RA's warnings ("Unknown Emulator", "Unsupported Game Version") arrive as pseudo-achievements that
  // trigger at once; rcheevos numbers them from 101000001 (RC_CLIENT_ACHIEVEMENT_WARNING_ID).
  if (e->type == RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED && e->achievement->id >= 101000001u)
    notify("RetroAchievements: %s", e->achievement->title);
  else if (e->type == RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED) {
    say("ra: unlocked %u \"%s\" (%u pts)", e->achievement->id, e->achievement->title, e->achievement->points);
    card_unlock(e->achievement->title, e->achievement->points, e->achievement->badge_name, es);  // in-game card, not a notification
  }
  else if (e->type == RC_CLIENT_EVENT_GAME_COMPLETED)
    notify(es ? "¡Juego completado en RetroAchievements!" : "Game completed on RetroAchievements!");
  else if (e->type == RC_CLIENT_EVENT_SERVER_ERROR)
    say("ra: server error %s: %s", e->server_error->api, e->server_error->error_message);
}

static void on_load(int result, const char *err, rc_client_t *c, void *ud) {
  (void)ud;
  if (result == RC_NO_GAME_LOADED) {
    notify(es ? "RetroAchievements no reconoce este ROM" : "RetroAchievements doesn't know this ROM");
    return;
  }
  if (result != RC_OK) {
    notify(es ? "RetroAchievements: no se pudo cargar el juego (%s)" : "RetroAchievements: could not load the game (%s)", err ? err : "?");
    return;
  }
  rc_client_user_game_summary_t s;
  rc_client_get_user_game_summary(c, &s);
  notify(es ? "Conectado a RetroAchievements\n%u de %u logros conseguidos" : "Connected to RetroAchievements\n%u of %u achievements unlocked",
         s.num_unlocked_achievements, s.num_core_achievements);
}

static char hash[33];

static void on_login(int result, const char *err, rc_client_t *c, void *ud) {
  (void)ud;
  if (result != RC_OK) {
    notify(es ? "RetroAchievements: sesión rechazada (%s). Vuelve a entrar en RA Hub" : "RetroAchievements: session refused (%s). Sign in again in RA Hub", err ? err : "?");
    return;
  }
  rc_client_begin_load_game(c, hash, on_load, NULL);
}

void ra_start(const void *m, const uint8_t *rom, size_t len) {
  int lang = -1, status = 0;
  char user[64], token[64];
  es = sceSystemServiceParamGetInt(1, &lang) == 0 && (lang == 3 || lang == 20);  // es-ES, es-419
  size_t slen;
  char *s = hub("GET", "/session", NULL, &status, &slen);
  if (!s) { say("ra: RA Hub is not running, no achievements"); return; }
  int ok = status == 200 && json_str(s, "user", user, sizeof user) && json_str(s, "token", token, sizeof token);
  memset(s, 0, strlen(s));  // the token lives no longer than needed
  free(s);
  if (!ok) {
    notify(es ? "RetroAchievements: inicia sesión en RA Hub" : "RetroAchievements: sign in to RA Hub");
    return;
  }

  // RA identifies a GBA game by the MD5 of the whole ROM.
  md5_state_t md5;
  md5_byte_t d[16];
  md5_init(&md5);
  md5_append(&md5, rom, (int)len);
  md5_finish(&md5, d);
  for (int i = 0; i < 16; i++) snprintf(hash + i * 2, 3, "%02x", d[i]);
  say("ra: ROM MD5 %s (must be one of the game's hashes on retroachievements.org)", hash);

  machine = m;
  client = rc_client_create(read_memory, server_call);
  rc_client_enable_logging(client, RC_CLIENT_LOG_LEVEL_WARN, on_log);
  rc_client_set_event_handler(client, on_event);
  // ponytail: softcore. Hardcore needs a client RetroAchievements has approved; add a toggle once it is
  rc_client_set_hardcore_enabled(client, 0);
  rc_client_begin_login_with_token(client, user, token, on_login, NULL);
  memset(token, 0, sizeof token);
}

void ra_frame(void) {
  if (client) rc_client_do_frame(client);
}

void ra_reset(const void *m) {
  machine = m;
  if (client) rc_client_reset(client);
}

size_t ra_state_size(void) { return client ? rc_client_progress_size(client) : 0; }

int ra_state_save(uint8_t *buf, size_t n) { return client ? rc_client_serialize_progress_sized(client, buf, n) : -1; }

void ra_state_load(const uint8_t *buf, size_t n) {
  if (client) rc_client_deserialize_progress_sized(client, n ? buf : NULL, n);
}

// ---------------- the menu's list ----------------
static rc_client_achievement_list_t *alist;
static const rc_client_achievement_t *items[1024];
static int nitems;

int ra_list_refresh(void) {
  if (alist) rc_client_destroy_achievement_list(alist);
  alist = NULL;
  nitems = 0;
  if (!client || !rc_client_is_game_loaded(client)) return -1;
  alist = rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
  for (uint32_t b = 0; alist && b < alist->num_buckets; b++)
    for (uint32_t i = 0; i < alist->buckets[b].num_achievements && nitems < (int)(sizeof items / sizeof *items); i++)
      items[nitems++] = alist->buckets[b].achievements[i];
  return nitems;
}

const char *ra_list_title(int i) { return items[i]->title ? items[i]->title : ""; }
const char *ra_list_desc(int i) { return items[i]->description ? items[i]->description : ""; }
const char *ra_list_progress(int i) { return items[i]->measured_progress; }
unsigned ra_list_points(int i) { return items[i]->points; }
int ra_list_unlocked(int i) { return items[i]->unlocked != 0; }

void ra_list_totals(unsigned *got, unsigned *all, unsigned *pts_got, unsigned *pts_all) {
  *got = *all = *pts_got = *pts_all = 0;
  for (int i = 0; i < nitems; i++) {
    *all += 1;
    *pts_all += items[i]->points;
    if (items[i]->unlocked) { *got += 1; *pts_got += items[i]->points; }
  }
}
