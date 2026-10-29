// The in-game achievement card: badge, "ACHIEVEMENT UNLOCKED", title and points, drawn in the black
// border right of the GBA picture for a few seconds. Badges come through RA Hub (GET /badge?u=<name>).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "card.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_easy_font.h"

void say(const char *fmt, ...);
char *ra_hub_get(const char *path, size_t *len);  // ra.c: RA Hub GET, malloc'd body or NULL

#define BG 0xff241c16u     // bytes R,G,B,A: a dark blue-grey
#define EDGE 0xff38c8f0u   // gold
#define WHITE 0xffffffffu
#define DIM 0xffb4aaa0u
#define PILL 0xe8222222u  // the toast: dark grey, ~90% opaque

struct canvas {
  uint32_t *px;
  int w, h;
};

static void fill(const struct canvas *cv, int x0, int y0, int w, int h, uint32_t c) {
  for (int y = y0 < 0 ? 0 : y0; y < y0 + h && y < cv->h; y++)
    for (int x = x0 < 0 ? 0 : x0; x < x0 + w && x < cv->w; x++) cv->px[y * cv->w + x] = c;
}

// stb_easy_font draws only ASCII 32..126: fold Latin-1 letters in UTF-8 to their base letter.
void text_ascii(char *out, size_t n, const char *in) {
  static const char *from[] = {"á", "à", "â", "ä", "ã", "é", "è", "ê", "ë", "í", "ì", "î", "ï", "ó", "ò", "ô", "ö", "õ", "ú", "ù", "û", "ü", "ñ", "ç",
                               "Á", "É", "Í", "Ó", "Ú", "Ñ", "Ü", "¡", "¿", "’", "“", "”", "…"};
  static const char to[] = "aaaaaeeeeiiiiooooouuuuncAEIOUNU!?'\"\".";
  size_t o = 0;
  while (*in && o + 1 < n) {
    size_t k = 0;
    for (; k < sizeof from / sizeof *from; k++)
      if (!strncmp(in, from[k], strlen(from[k]))) break;
    if (k < sizeof from / sizeof *from) {
      out[o++] = to[k];
      in += strlen(from[k]);
    } else {
      out[o++] = (unsigned char)*in >= 32 && (unsigned char)*in < 127 ? *in : '?';
      if ((unsigned char)*in >= 0xC0) while (((unsigned char)in[1] & 0xC0) == 0x80) in++;  // skip the rest of a UTF-8 char
      in++;
    }
  }
  out[o] = 0;
}

// One line of text, `s` pixels per font unit, top-left at (x, y).
static void text(const struct canvas *px, int x, int y, int s, char *str, uint32_t c) {
  static char vb[64 * 1024];  // shared by the card threads and the main thread's indicators: one lock
  static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&mx);
  int quads = stb_easy_font_print(0, 0, str, NULL, vb, sizeof vb);
  for (int q = 0; q < quads; q++) {
    float *v = (float *)(vb + q * 64);  // 4 vertices of 16 bytes: x, y, z, rgba
    float x0 = v[0], y0 = v[1], x1 = v[8], y1 = v[9];
    fill(px, x + (int)(x0 * s), y + (int)(y0 * s), (int)((x1 - x0) * s), (int)((y1 - y0) * s), c);
  }
  pthread_mutex_unlock(&mx);
}

// Greedy word wrap into at most `max_lines` lines of `width` pixels; returns the y after the text.
static int wrapped(const struct canvas *px, int x, int y, int s, int width, int max_lines, const char *str, uint32_t c) {
  char buf[512], line[512];
  text_ascii(buf, sizeof buf, str);
  char *p = buf;
  for (int l = 0; l < max_lines && *p; l++) {
    size_t best = 0, n = 0;
    while (p[n]) {  // longest prefix ending at a word boundary that fits
      size_t next = n;
      while (p[next] == ' ') next++;
      while (p[next] && p[next] != ' ') next++;
      memcpy(line, p, next);
      line[next] = 0;
      if (stb_easy_font_width(line) * s > width) break;
      best = n = next;
    }
    if (!best) best = strlen(p) < 12 ? strlen(p) : 12;  // one word wider than the card: cut it
    memcpy(line, p, best);
    line[best] = 0;
    if (l == max_lines - 1 && p[best]) strcpy(line + (best > 3 ? best - 3 : 0), "...");
    text(px, x, y, s, line, c);
    y += 12 * s;
    p += best;
    while (*p == ' ') p++;
  }
  return y;
}

// Rounded-corner mask: 1 inside the CARD_W x CARD_H pill, with a CORNER-pixel radius.
#define CORNER 14
static int inside(int x, int y) {
  int dx = x < CORNER ? CORNER - x : x >= CARD_W - CORNER ? x - (CARD_W - CORNER - 1) : 0;
  int dy = y < CORNER ? CORNER - y : y >= CARD_H - CORNER ? y - (CARD_H - CORNER - 1) : 0;
  return dx * dx + dy * dy <= CORNER * CORNER;
}

// Like the PS5's trophy toast: a dark translucent pill, the badge on the left, the achievement's name
// on the right. Alpha in the top byte: present() blends it over the game.
uint32_t *card_compose(const char *title, unsigned points, const uint8_t *png, size_t png_len, int es) {
  (void)points;
  (void)es;
  uint32_t *px = malloc(sizeof(uint32_t) * CARD_W * CARD_H);
  if (!px) return NULL;
  struct canvas cv = {px, CARD_W, CARD_H};
  for (int y = 0; y < CARD_H; y++)
    for (int x = 0; x < CARD_W; x++) px[y * CARD_W + x] = inside(x, y) ? PILL : 0;  // 0: fully transparent
  int w = 0, h = 0, n;
  uint8_t *badge = png ? stbi_load_from_memory(png, (int)png_len, &w, &h, &n, 4) : NULL;
  const int B = 64, bx = 16, by = (CARD_H - B) / 2;  // RA badges are 64x64: drawn 1:1
  if (badge && w > 0 && h > 0) {
    for (int y = 0; y < B; y++)
      for (int x = 0; x < B; x++) {
        const uint8_t *s = badge + ((y * h / B) * w + x * w / B) * 4;
        uint32_t a = s[3];  // over the pill's own colour
        uint32_t r = (s[0] * a + 0x22 * (255 - a)) / 255, g = (s[1] * a + 0x22 * (255 - a)) / 255, b = (s[2] * a + 0x22 * (255 - a)) / 255;
        px[(by + y) * CARD_W + bx + x] = 0xff000000u | b << 16 | g << 8 | r;
      }
  } else {
    fill(&cv, bx, by, B, B, EDGE);  // no badge (offline / RA Hub without /badge): a gold square
  }
  stbi_image_free(badge);
  int tx = bx + B + 16, tw = CARD_W - tx - 16;
  // one line if it fits, else two (vertically centred either way)
  char buf[512];
  text_ascii(buf, sizeof buf, title);
  int lines = stb_easy_font_width(buf) * 2 <= tw ? 1 : 2;
  wrapped(&cv, tx, (CARD_H - lines * 24) / 2 + 4, 2, tw, lines, title, WHITE);
  return px;
}

const uint32_t *card_sun(int level, int es) {
  static uint32_t px[SUN_W * SUN_H];
  struct canvas cv = {px, SUN_W, SUN_H};
  char line[32];
  fill(&cv, 0, 0, SUN_W, SUN_H, BG);
  fill(&cv, 0, 0, SUN_W, 4, EDGE);
  snprintf(line, sizeof line, "%s %d/10", es ? "SOL" : "SUN", level);
  text(&cv, (SUN_W - 2 * stb_easy_font_width(line)) / 2, 12, 2, line, WHITE);
  for (int i = 0; i < 10; i++) fill(&cv, 10 + i * 20, 40, 18, 14, i < level ? EDGE : 0xff3a3028u);
  return px;
}

#define HUD_ROW 40
#define HUD_GAP 6
const uint32_t *card_hud(const char *const *lines, int n, int *h) {
  static uint32_t px[CARD_W * 13 * (HUD_ROW + HUD_GAP)];
  struct canvas cv = {px, CARD_W, 0};
  if (n > 13) n = 13;
  cv.h = *h = n * (HUD_ROW + HUD_GAP) - HUD_GAP;
  memset(px, 0, sizeof(uint32_t) * CARD_W * (size_t)cv.h);  // 0: fully transparent between rows
  for (int i = 0; i < n; i++) {
    int y = i * (HUD_ROW + HUD_GAP);
    fill(&cv, 0, y, CARD_W, HUD_ROW, PILL);
    fill(&cv, 0, y, 4, HUD_ROW, EDGE);
    wrapped(&cv, 16, y + 10, 2, CARD_W - 32, 1, lines[i], WHITE);
  }
  return px;
}

// ---------------- showing them ----------------
// Unlock -> worker thread (badge download + compose) -> ring -> main thread shows one at a time.
#define QUEUE 8
static uint32_t *_Atomic ready[QUEUE];
static atomic_uint ready_w, ready_r;
static uint32_t *shown;
static int shown_frames;
static int lang_es;

struct job {
  char title[256], badge[32];
  unsigned points;
};

static void *job_main(void *arg) {
  struct job *j = arg;
  char path[64];
  size_t len = 0;
  snprintf(path, sizeof path, "/badge?u=%s", j->badge);
  char *png = j->badge[0] ? ra_hub_get(path, &len) : NULL;
  uint32_t *px = card_compose(j->title, j->points, (const uint8_t *)png, len, lang_es);
  free(png);
  unsigned w = atomic_fetch_add(&ready_w, 1);
  if (px && w - atomic_load(&ready_r) < QUEUE) atomic_store(&ready[w % QUEUE], px);
  else free(px);  // ponytail: more than 8 unlocks in flight drop the extras' cards
  free(j);
  return NULL;
}

void card_unlock(const char *title, unsigned points, const char *badge, int es) {
  struct job *j = calloc(1, sizeof *j);
  pthread_t t;
  if (!j) return;
  lang_es = es;
  snprintf(j->title, sizeof j->title, "%s", title ? title : "");
  for (size_t i = 0; badge && badge[i] && i + 1 < sizeof j->badge; i++)  // badge names are digits (or _lock)
    if ((badge[i] >= '0' && badge[i] <= '9') || (badge[i] >= 'a' && badge[i] <= 'z') || badge[i] == '_') j->badge[i] = badge[i];
    else { j->badge[0] = 0; break; }
  j->points = points;
  if (pthread_create(&t, NULL, job_main, j)) { free(j); return; }
  pthread_detach(t);
}

const uint32_t *card_frame(void) {
  if (shown && --shown_frames <= 0) {
    free(shown);
    shown = NULL;
  }
  unsigned r = atomic_load(&ready_r);
  uint32_t *next;
  if (!shown && r != atomic_load(&ready_w) && (next = atomic_exchange(&ready[r % QUEUE], NULL))) {
    shown = next;
    shown_frames = 5 * 60;  // 5 s
    atomic_store(&ready_r, r + 1);
  }
  return shown;
}
