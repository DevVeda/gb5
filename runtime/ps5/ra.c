// RetroAchievements for the title (rcheevos rc_client), through RA Hub on the same console:
//   GET  127.0.0.1:8045/session    the signed-in user and Connect token (RA Hub signs in once)
//   POST 127.0.0.1:8045/dorequest  RA Hub forwards rcheevos' calls to RetroAchievements over HTTPS,
//                                  so the title needs no TLS of its own
// Unlocks show as an in-game card (card.c); measured progress, active challenges and leaderboard trackers
// as the in-game indicators (ra_hud); the rest as console notifications (top right).
// Offline: login, the game's data and the player's unlocks come from the last online start, and unlocks /
// leaderboard entries RA hasn't confirmed wait in ra_pending.txt until it answers (next minute or session).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ra.h"
#include "card.h"
#include "rc_client.h"
#include "rhash/md5.h"
#include "rc_version.h"
#include "rc_api_runtime.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void say(const char *fmt, ...);
uint32_t gba_ra_read(const void *m, uint32_t addr, uint8_t *out, uint32_t n);

typedef struct { char useless[45]; char message[3075]; } notify_request_t;  // as Payload Manager / RA Hub
int sceKernelSendNotificationRequest(int device, notify_request_t *req, size_t size, int unused);
int sceSystemServiceParamGetInt(int param_id, int *value);

static rc_client_t *client;
static const void *machine;
static int es;  // console in Spanish
static char user[64], token[64];  // RA Hub's session: the token only until login completes
static int offline;               // this session started without RetroAchievements (cached data)
static int loaded;                // the game is loaded: earlier sessions' records can be replayed
static int lb_popups;             // leaderboard notifications and trackers; off by default (RA lets players hide them)
static time_t session_start;
static char hash[33];             // the ROM's MD5: how RA knows the game
static char dir[160] = ".";  // the PS5 user's folder in the title (main.c): queue and cache live there
static char pending_path[192];
#ifndef GB5_VERSION
#define GB5_VERSION "0.0.0"  // build.sh passes the last git tag
#endif
#ifndef CORE_VERSION
#define CORE_VERSION "0.0.0"  // build.sh passes vendor/gba-core/Cargo.toml's version
#endif
// RetroAchievements identifies the emulator by this (unique, numeric version); RA Hub forwards it.
#define USER_AGENT "GB5/" GB5_VERSION " (PS5) gba-core/" CORE_VERSION " rcheevos/" RCHEEVOS_VERSION_STRING

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
  char head[384];
  int hl = snprintf(head, sizeof head,
                    "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\nX-RAHub: 1\r\nUser-Agent: " USER_AGENT "\r\n"  // X-RAHub: RA Hub's CSRF guard on POST
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

static time_t now(void) {
  struct timespec t;
  return clock_gettime(CLOCK_REALTIME, &t) ? 0 : t.tv_sec;
}

// One field of rcheevos' form body (its values here are plain: digits, names, hashes, tokens).
static int form(const char *post, const char *key, char *out, size_t n) {
  size_t k = strlen(key);
  for (const char *p = post; p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL)
    if (!strncmp(p, key, k) && p[k] == '=') {
      size_t l = strcspn(p += k + 1, "&");
      if (l >= n) return 0;
      memcpy(out, p, l);
      out[l] = 0;
      return 1;
    }
  return 0;
}

// RA got the request and answered (done, even if it refused); else it's worth sending again.
static int answered(int status, const char *body) { return body && body[0] && status >= 200 && status < 500 && status != 429; }

static void write_file(const char *path, const char *d, size_t n) {
  char tmp[96];
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  int ok = f && fwrite(d, 1, n, f) == n;
  if (f) ok &= fclose(f) == 0;
  if (ok && rename(tmp, path) == 0) chmod(path, 0666);  // FTP (another process) must be able to read it
  else say("ra: cannot write %s", path);
}

static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *d = n > 0 ? malloc((size_t)n + 1) : NULL;
  if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
  fclose(f);
  if (d) d[*len = (size_t)n] = 0;
  return d;
}

// ---------------- offline: RA's last answers ----------------
// The game's data and the player's unlocks, as RA sent them at the last online start (never the token).
static void cache_path(char *out, size_t n, const char *r) { snprintf(out, n, "%s/ra_%s_%s.json", dir, r, user); }

// login2 without a connection: RA Hub's session is the proof; achievementsets / startsession: the cache.
static char *offline_answer(const char *r, size_t *len) {
  char path[256];
  if (!strcmp(r, "login2")) {
    char *b = malloc(256);
    if (b) *len = (size_t)snprintf(b, 256, "{\"Success\":true,\"User\":\"%s\",\"Token\":\"%s\"}", user, token);
    return b;
  }
  if (strcmp(r, "achievementsets") && strcmp(r, "startsession")) return NULL;
  cache_path(path, sizeof path, r);
  return read_file(path, len);
}

// ---------------- offline: unlocks RA hasn't confirmed ----------------
// One line each: "A <achievement> <hardcore> <unix time> <user> <mac>" or "L <leaderboard> <score> <unix time>
// <user> <mac>", mac = md5(token|the rest): only the signed-in player's token makes or replays a record, which
// is what the API itself asks for. A record leaves once RA has answered it.
static pthread_mutex_t pending_mx = PTHREAD_MUTEX_INITIALIZER;

static void record_mac(char out[33], const char *tok, const char *rest) {
  md5_state_t s;
  md5_byte_t d[16];
  md5_init(&s);
  md5_append(&s, (const md5_byte_t *)tok, (int)strlen(tok));
  md5_append(&s, (const md5_byte_t *)"|", 1);
  md5_append(&s, (const md5_byte_t *)rest, (int)strlen(rest));
  md5_finish(&s, d);
  for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
}

// "A 123 1" for an award, "L 45 -300" for a leaderboard entry; 0 for any other call.
static int record_key(const char *post, char *key, size_t n) {
  char r[24], id[16], v[16];
  if (!form(post, "r", r, sizeof r)) return 0;
  if (!strcmp(r, "awardachievement") && form(post, "a", id, sizeof id) && form(post, "h", v, sizeof v)) return snprintf(key, n, "A %s %s", id, v) > 0;
  if (!strcmp(r, "submitlbentry") && form(post, "i", id, sizeof id) && form(post, "s", v, sizeof v)) return snprintf(key, n, "L %s %s", id, v) > 0;
  return 0;
}

// add: append `add` unless a record with `key` is there. Else drop that record.
// ponytail: 16 KB of records (~200); an older line falls off past that
static void pending_edit(const char *key, const char *add) {
  static char in[16384], out[16384 + 256];
  size_t k = strlen(key), o = 0, n = 0;
  int had = 0;
  pthread_mutex_lock(&pending_mx);
  char *d = read_file(pending_path, &n);
  const char *keep = d ? d + (n >= sizeof in ? n - sizeof in + 1 : 0) : "";
  if (d && keep > d && strchr(keep, '\n')) keep = strchr(keep, '\n') + 1;  // never half a line
  snprintf(in, sizeof in, "%s", keep);
  free(d);
  for (char *l = in, *e; *l; l = e) {
    e = strchr(l, '\n') ? strchr(l, '\n') + 1 : l + strlen(l);
    if (!strncmp(l, key, k) && l[k] == ' ') {
      had = 1;
      if (!add) continue;
    }
    memcpy(out + o, l, (size_t)(e - l));
    o += (size_t)(e - l);
  }
  if (add && !had) o += (size_t)snprintf(out + o, sizeof out - o, "%s\n", add);
  if (add ? !had : had) write_file(pending_path, out, o);
  pthread_mutex_unlock(&pending_mx);
}

// An award / leaderboard request is going out: keep it until RA answers. Retries carry o= (seconds since).
static void pending_note(const char *post) {
  char key[48], u[64], t[64], o[16], rest[160], mac[33], line[200];
  if (!record_key(post, key, sizeof key) || !form(post, "u", u, sizeof u) || !form(post, "t", t, sizeof t)) return;
  snprintf(rest, sizeof rest, "%s %lld %s", key, (long long)(now() - (form(post, "o", o, sizeof o) ? atol(o) : 0)), u);
  record_mac(mac, t, rest);
  snprintf(line, sizeof line, "%s %s", rest, mac);
  pending_edit(key, line);
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
  char *body = hub("POST", "/dorequest", c->post, &status, &len), key[48], r[24] = "";
  form(c->post, "r", r, sizeof r);
  if (answered(status, body)) {
    if (record_key(c->post, key, sizeof key)) pending_edit(key, NULL);
    if (status == 200 && strstr(body, "\"Success\":true") && (!strcmp(r, "achievementsets") || !strcmp(r, "startsession"))) {
      char path[256];
      cache_path(path, sizeof path, r);
      write_file(path, body, len);
    }
  } else {
    char *cached = offline_answer(r, &len);  // no connection: start from what RA said last time
    free(body);
    body = cached;  // else none: rcheevos sees no answer and retries (RA Hub's 502 JSON would read as done)
    status = 200;
    offline |= cached != NULL;
  }
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
    pending_note(job->post);
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

// ---------------- in-game indicators ----------------
// Active challenges, leaderboard trackers and the measured-progress line, drawn bottom right (card_hud).
// rcheevos raises them from rc_client_do_frame (main thread), a few from server callbacks: one lock.
#define HUD_MAX 6
struct hud_item { uint32_t id; char text[96], badge[32]; };
static struct hud_item chal[HUD_MAX], trk[HUD_MAX];
static int nchal, ntrk, hud_dirty;
static char prog[96], prog_badge[32];
static pthread_mutex_t hud_mx = PTHREAD_MUTEX_INITIALIZER;

// Put (text != NULL) or remove the entry `id` in list l of *n.
static void hud_set(struct hud_item *l, int *n, uint32_t id, const char *text, const char *badge) {
  int i = 0;
  while (i < *n && l[i].id != id) i++;
  if (!text) {
    if (i < *n) memmove(&l[i], &l[i + 1], sizeof *l * (size_t)(--*n - i));
  } else if (i < *n || *n < HUD_MAX) {
    if (i == *n) (*n)++;
    l[i].id = id;
    snprintf(l[i].text, sizeof l[i].text, "%s", text);
    snprintf(l[i].badge, sizeof l[i].badge, "%s", badge ? badge : "");
  }
  hud_dirty = 1;
}

const uint32_t *ra_hud(int *h) {
  static const uint32_t *px;
  static int ph;
  static unsigned badges;
  pthread_mutex_lock(&hud_mx);
  if (hud_dirty || badges != card_badges_loaded()) {  // or a badge arrived
    const char *lines[2 * HUD_MAX + 1], *bs[2 * HUD_MAX + 1];
    int n = 0;
    badges = card_badges_loaded();
    for (int i = 0; lb_popups && i < ntrk; i++) bs[n] = NULL, lines[n++] = trk[i].text;  // a whole-game timer would never leave
    for (int i = 0; i < nchal; i++) bs[n] = chal[i].badge, lines[n++] = chal[i].text;
    if (prog[0]) bs[n] = prog_badge, lines[n++] = prog;
    px = n ? card_hud(lines, bs, n, &ph) : NULL;
    hud_dirty = 0;
  }
  pthread_mutex_unlock(&hud_mx);
  *h = ph;
  return px;
}

static void indicators(const rc_client_event_t *e) {
  char t[96];
  pthread_mutex_lock(&hud_mx);
  switch (e->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
      snprintf(t, sizeof t, "%s %s", es ? "Reto:" : "Challenge:", e->achievement->title);
      hud_set(chal, &nchal, e->achievement->id, t, e->achievement->badge_name);
      break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE: hud_set(chal, &nchal, e->achievement->id, NULL, NULL); break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:  // rcheevos hides it again after 2 s
      snprintf(prog, sizeof prog, "%s  %s", e->achievement->measured_progress, e->achievement->title);
      snprintf(prog_badge, sizeof prog_badge, "%s", e->achievement->badge_name);
      hud_dirty = 1;
      break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE: prog[0] = 0; hud_dirty = 1; break;
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE: hud_set(trk, &ntrk, e->leaderboard_tracker->id, e->leaderboard_tracker->display, NULL); break;
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE: hud_set(trk, &ntrk, e->leaderboard_tracker->id, NULL, NULL); break;
  }
  pthread_mutex_unlock(&hud_mx);
}

static void on_event(const rc_client_event_t *e, rc_client_t *c) {
  (void)c;
  indicators(e);
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
  else if (!lb_popups && (e->type == RC_CLIENT_EVENT_LEADERBOARD_STARTED || e->type == RC_CLIENT_EVENT_LEADERBOARD_FAILED ||
                          e->type == RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED || e->type == RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD))
    ;  // turned off in OPTIONS (leaderboards still run and submit)
  else if (e->type == RC_CLIENT_EVENT_LEADERBOARD_STARTED)
    notify(es ? "Clasificación: intento iniciado\n%s" : "Leaderboard attempt started\n%s", e->leaderboard->title);
  else if (e->type == RC_CLIENT_EVENT_LEADERBOARD_FAILED)
    notify(es ? "Clasificación: intento fallido\n%s" : "Leaderboard attempt failed\n%s", e->leaderboard->title);
  else if (e->type == RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED)
    notify(es ? "Clasificación: %s enviado\n%s" : "Leaderboard: %s submitted\n%s", e->leaderboard->tracker_value, e->leaderboard->title);
  else if (e->type == RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD)
    notify(es ? "%s\nPuesto %u de %u (mejor: %s)" : "%s\nRank %u of %u (best: %s)", e->leaderboard->title,
           e->leaderboard_scoreboard->new_rank, e->leaderboard_scoreboard->num_entries, e->leaderboard_scoreboard->best_score);
  else if (e->type == RC_CLIENT_EVENT_DISCONNECTED)
    notify(es ? "RetroAchievements: sin conexión. Los logros se guardan y se envían después" : "RetroAchievements: offline. Unlocks are kept and sent later");
  else if (e->type == RC_CLIENT_EVENT_RECONNECTED)
    notify(es ? "RetroAchievements: conectado de nuevo, logros enviados" : "RetroAchievements: back online, unlocks sent");
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
  int hc = rc_client_get_hardcore_enabled(c);  // RA's rule: say which mode the game started in
  notify(es ? "%s RetroAchievements (%s)\n%u de %u logros conseguidos" : "%s RetroAchievements (%s)\n%u of %u achievements unlocked",
         offline ? (es ? "Sin conexión con" : "Offline from") : (es ? "Conectado a" : "Connected to"),
         hc ? "hardcore" : "casual", s.num_unlocked_achievements, s.num_core_achievements);
  loaded = 1;
}

static void replay_done(const rc_api_server_response_t *r, void *ud) { (void)r, (void)ud; }  // call_main drops the record

// Send again what earlier sessions left unconfirmed (this session's own, rcheevos retries itself). Main thread.
static void replay(void) {
  const rc_client_user_t *me = rc_client_get_user_info(client);
  size_t n = 0;
  pthread_mutex_lock(&pending_mx);
  char *d = read_file(pending_path, &n);
  pthread_mutex_unlock(&pending_mx);
  for (char *l = d, *e; me && l && *l; l = e) {
    char type, who[64], mac[33], want[33], key[48];
    unsigned id;
    int v;
    long long when;
    if ((e = strchr(l, '\n'))) *e++ = 0;
    else e = l + strlen(l);
    if (sscanf(l, "%c %u %d %lld %63s %32s", &type, &id, &v, &when, who, mac) != 6) continue;
    snprintf(key, sizeof key, "%c %u %d", type, id, v);
    *strrchr(l, ' ') = 0;  // the record without its mac
    record_mac(want, me->token, l);
    if (strcmp(who, me->username)) continue;  // another RA account's: kept for when it signs in here again
    if (strcmp(mac, want)) {  // edited
      say("ra: dropping pending record \"%s\"", l);
      pending_edit(key, NULL);
      continue;
    }
    if (when >= session_start) continue;
    long long ago = now() - when;
    rc_api_request_t req;
    int rc = -1;
    if (type == 'A') {
      const rc_client_achievement_t *a = rc_client_get_achievement_info(client, id);
      if (a && a->unlocked & (v ? RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE : RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE)) { pending_edit(key, NULL); continue; }
      rc_api_award_achievement_request_t p = {me->username, me->token, id, (uint32_t)v, hash, ago > 0 ? (uint32_t)ago : 0};
      rc = rc_api_init_award_achievement_request(&req, &p);
    } else if (type == 'L') {
      rc_api_submit_lboard_entry_request_t p = {me->username, me->token, id, v, hash, ago > 0 ? (uint32_t)ago : 0};
      rc = rc_api_init_submit_lboard_entry_request(&req, &p);
    }
    if (rc != RC_OK) continue;
    say("ra: sending %s from an earlier session (%lld s ago)", key, ago);
    server_call(&req, replay_done, NULL, client);
    rc_api_destroy_request(&req);
  }
  free(d);
}

static void on_login(int result, const char *err, rc_client_t *c, void *ud) {
  (void)ud;
  memset(token, 0, sizeof token);  // rcheevos keeps its own copy
  if (result != RC_OK) {
    notify(es ? "RetroAchievements: sesión rechazada (%s). Vuelve a entrar en RA Hub" : "RetroAchievements: session refused (%s). Sign in again in RA Hub", err ? err : "?");
    return;
  }
  rc_client_begin_load_game(c, hash, on_load, NULL);
}

void ra_start(const void *m, const uint8_t *rom, size_t len, int hardcore, int32_t ps5_user, const char *user_dir) {
  int lang = -1, status = 0;
  char path[32];
  es = sceSystemServiceParamGetInt(1, &lang) == 0 && (lang == 3 || lang == 20);  // es-ES, es-419
  snprintf(dir, sizeof dir, "%s", user_dir);
  snprintf(pending_path, sizeof pending_path, "%s/ra_pending.txt", dir);
  size_t slen;
  snprintf(path, sizeof path, "/session?u=%08x", (uint32_t)ps5_user);  // this PS5 user's RetroAchievements account
  char *s = hub("GET", path, NULL, &status, &slen);
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
  session_start = now();
  client = rc_client_create(read_memory, server_call);
  rc_client_enable_logging(client, RC_CLIENT_LOG_LEVEL_WARN, on_log);
  rc_client_set_event_handler(client, on_event);
  // Hardcore unlocks only count once RetroAchievements approves GB5; until then its server answers "Unknown Emulator".
  rc_client_set_hardcore_enabled(client, hardcore);
  rc_client_begin_login_with_token(client, user, token, on_login, NULL);
}

void ra_lb_popups(int on) {
  pthread_mutex_lock(&hud_mx);
  lb_popups = on;
  hud_dirty = 1;  // trackers appear / go at once
  pthread_mutex_unlock(&hud_mx);
}

void ra_set_hardcore(int on) {
  // rcheevos raises RC_CLIENT_EVENT_RESET when it's turned on mid-game; main resets either way.
  if (client) rc_client_set_hardcore_enabled(client, on);
  notify(on ? (es ? "RetroAchievements: modo hardcore. El juego se reinició" : "RetroAchievements: hardcore mode. The game was reset")
            : (es ? "RetroAchievements: modo casual" : "RetroAchievements: casual mode"));
}

void ra_frame(void) {
  static int wait;  // frames to the next replay: once loaded, then every minute
  if (!client) return;
  rc_client_do_frame(client);
  if (loaded && --wait <= 0) {
    wait = 60 * 60;
    replay();
  }
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
