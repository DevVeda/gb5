// RetroAchievements through RA Hub (ra.c).
#pragma once
#include <stddef.h>
#include <stdint.h>

// Sign in with RA Hub's session for this PS5 user and load the game by the ROM's MD5. Async: notifies when
// connected. Does nothing (but log) when RA Hub isn't running.
// hardcore: start in RetroAchievements' hardcore mode (the caller blocks state loading, rewind, cheats).
// user_dir: the PS5 user's folder in the title, for the offline queue and cache.
void ra_start(const void *machine, const uint8_t *rom, size_t len, int hardcore, int32_t ps5_user, const char *user_dir);
// OPTIONS > LEADERBOARD ALERTS: show leaderboard started / failed / submitted / rank notifications.
void ra_lb_popups(int on);
// The player switched mode in OPTIONS. Turning it on: the caller resets the game (then ra_reset).
void ra_set_hardcore(int on);
// Once per emulated frame.
void ra_frame(void);
// The in-game indicators (challenges, leaderboard trackers, measured progress) to draw bottom right:
// CARD_W x *h pixels like card_frame's, or NULL when there are none. Main thread.
const uint32_t *ra_hud(int *h);
// The game was reset (new machine at the same ROM): restart the achievement session on it.
void ra_reset(const void *machine);
// The menu's ACHIEVEMENTS page: refresh when it opens. -1 = not connected / no game, else the count.
int ra_list_refresh(void);
const char *ra_list_title(int i);
const char *ra_list_desc(int i);
const char *ra_list_progress(int i);  // "3/10" for measured ones, else ""
unsigned ra_list_points(int i);
int ra_list_unlocked(int i);
// Totals for the header: unlocked / all achievements, points earned / all points.
void ra_list_totals(unsigned *got, unsigned *all, unsigned *pts_got, unsigned *pts_all);
// Achievement progress, saved with a save state so unlock conditions don't desync when it's loaded.
size_t ra_state_size(void);
int ra_state_save(uint8_t *buf, size_t n);         // 0 = ok
void ra_state_load(const uint8_t *buf, size_t n);  // n = 0: the state had none (progress resets)
