// Rewind (rewind.c): a save state every second for the last two minutes, each with a thumbnail of
// the screen. L3 pauses on the timeline; the right stick scrubs it; A goes back there, B cancels.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define RW_SECONDS 120

// Every emulated frame: keeps one snapshot per second of play.
void rewind_record(void *machine, const uint16_t *fb);
// Snapshot the present moment now (entering rewind mode, so "now" is on the timeline).
void rewind_mark(void *machine, const uint16_t *fb);
int rewind_count(void);                  // snapshots kept (0 = nothing to rewind to yet)
const uint16_t *rewind_thumb(int back);  // 240x160 BGR555 screen of snapshot `back` (0 = newest)
// Restore snapshot `back` into the machine and forget the newer ones. 0 = done.
int rewind_restore(void *machine, int back);
