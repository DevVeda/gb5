// Rewind history: a ring of save states (gba_rt's, ~600 KB each) plus the screen at that moment and
// RetroAchievements progress, one per second of play. RW_SECONDS of them stay; the oldest goes first.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "rewind.h"
#include "ra.h"
#include <stdlib.h>
#include <string.h>

uint8_t *gba_state_save(const void *m, size_t *len);
void gba_state_free(uint8_t *p, size_t len);
int gba_state_load(void *m, const uint8_t *data, size_t len);
void say(const char *fmt, ...);

#define EVERY 60  // emulated frames between snapshots: about a second

struct snap {
  uint8_t *state, *ra;
  size_t len, ra_len;
  uint16_t screen[240 * 160];
};

static struct snap ring[RW_SECONDS];
static int newest = -1, count, frames;

static void drop(struct snap *s) {
  gba_state_free(s->state, s->len);
  free(s->ra);
  s->state = s->ra = NULL;
}

void rewind_mark(void *m, const uint16_t *fb) {
  int i = (newest + 1) % RW_SECONDS;
  struct snap *s = &ring[i];
  drop(s);  // the oldest, once the ring is full
  if (!(s->state = gba_state_save(m, &s->len))) return;
  if ((s->ra_len = ra_state_size()) && (s->ra = malloc(s->ra_len)) && ra_state_save(s->ra, s->ra_len)) {
    free(s->ra);
    s->ra = NULL;
  }
  if (!s->ra) s->ra_len = 0;
  memcpy(s->screen, fb, sizeof s->screen);
  newest = i;
  if (count < RW_SECONDS) count++;
  frames = 0;
}

void rewind_record(void *m, const uint16_t *fb) {
  if (++frames >= EVERY) rewind_mark(m, fb);
}

int rewind_count(void) { return count; }

const uint16_t *rewind_thumb(int back) {
  return back >= 0 && back < count ? ring[(newest - back + RW_SECONDS) % RW_SECONDS].screen : NULL;
}

int rewind_restore(void *m, int back) {
  if (back < 0 || back >= count) return -1;
  struct snap *s = &ring[(newest - back + RW_SECONDS) % RW_SECONDS];
  if (gba_state_load(m, s->state, s->len)) { say("rewind: state refused"); return -1; }
  ra_state_load(s->ra, s->ra_len);
  for (int k = 0; k < back; k++) drop(&ring[(newest - k + RW_SECONDS) % RW_SECONDS]);  // that future is gone
  newest = (newest - back + RW_SECONDS) % RW_SECONDS;
  count -= back;
  frames = 0;
  say("rewind: back %d s", back);
  return 0;
}
