// Cheats (cheats.c): CodeBreaker codes from cheats.txt (GB5 picks them from libretro-database),
// all off until turned on in Veda Menu; which are on is kept in cheats_on.txt.
#pragma once
#include <stddef.h>
#include <stdint.h>

void cheats_load(const char *cheats_path, const char *on_path);
// A fresh libretro .cht (as RA Hub's /cheats returns it): keeps its CodeBreaker cheats in cheats.txt,
// the ones that were on by name stay on, and reloads. Returns how many, or -1 if the text had none.
int cheats_update(const char *cht, size_t len);
int cheats_count(void);
const char *cheats_name(int i);
int cheats_enabled(int i);
void cheats_toggle(int i);  // and saves cheats_on.txt
// Once per emulated frame, before it runs. keys = GBA keys held (1 = pressed), for "if button" codes.
void cheats_apply(void *machine, uint16_t keys);
