// In-game achievement card (card.c).
#pragma once
#include <stddef.h>
#include <stdint.h>

// The achievement toast, PS5-sized at 1080p: badge + name, top right over the picture.
#define CARD_W 440
#define CARD_H 96

#define SUN_W 220  // the sunlight meter, shown while the touchpad sets it
#define SUN_H 64
// The meter for sunlight level 0..10 (VideoOut bytes R,G,B,A, SUN_W x SUN_H). Main thread.
const uint32_t *card_sun(int level, int es);

// Queue a card for an unlock; fetches the badge through RA Hub on a thread. Any thread.
void card_unlock(const char *title, unsigned points, const char *badge, int es);
// Once per presented frame, main thread: the card to draw now (CARD_W x CARD_H, VideoOut bytes R,G,B,A,
// alpha in the top byte: blend it), or NULL.
const uint32_t *card_frame(void);
// UTF-8 -> what stb_easy_font can draw (ASCII 32..126): Latin-1 letters lose their accent, others become '?'.
void text_ascii(char *out, size_t n, const char *in);
// The pixels of one card; png may be NULL. Caller frees. (Separate so it can be checked off the console.)
uint32_t *card_compose(const char *title, unsigned points, const uint8_t *png, size_t png_len, int es);
