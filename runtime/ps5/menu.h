// The extra menu (L2 + R2): drawn at GBA resolution over the paused game, in the style of
// Pokémon's start menu, for every game (menu.c).
#pragma once
#include <stdint.h>

enum { SIZE_PIXEL, SIZE_FILL, SIZE_STRETCH, SIZE_COUNT };  // x6 1440x960 / fill height 1620x1080 / 16:9

struct settings {
  int size;   // SIZE_*
  int lcd;    // LCD grid between GBA pixels
  int color;  // GBA screen colours (duller, as the real LCD)
  int turbo;  // 1, 2 or 4 emulated frames per shown frame
  int swap_ab;  // 0: GBA A = Circle, B = Cross (Nintendo positions); 1: A = Cross, B = Circle
  int sun;      // solar sensor carts: sunlight 0..10 (touchpad or OPTIONS)
  int gyro;     // gyro carts: controller axis and sign, 0..5 = X+ X- Y+ Y- Z+ Z-
  int rumble;   // rumble carts: pass the motor to the DualSense
  int gyro_sens, tilt_sens;  // 1..10, 5 = mGBA's scale (x level / 5)
};

enum { MENU_NONE, MENU_CLOSED, MENU_RESET, MENU_QUIT, MENU_SAVE_STATE, MENU_LOAD_STATE, MENU_RECENTER, MENU_UPDATE_CHEATS };
// Cartridge extras, as gba_hw(): which sensor options OPTIONS shows.
enum { HW_CLOCK = 1, HW_LIGHT = 2, HW_GYRO = 4, HW_RUMBLE = 8, HW_TILT = 16 };

// slots_used: bit n set = state slot n (1..3) has a file.
void menu_open(int slots_used, unsigned hw);
// The state slot (1..3) for MENU_SAVE_STATE / MENU_LOAD_STATE.
int menu_slot(void);
// CHEATS shows "UPDATE FROM THE INTERNET" when the title knows its libretro cheat file.
void menu_cheats_updatable(int yes);
// A line at the foot of the page now showing (the cheat update's result); "" clears it.
void menu_status(const char *msg);
int menu_is_open(void);
// Raw DualSense buttons this frame (pad_sample.buttons). Changes *s; returns MENU_*.
int menu_input(uint32_t buttons, struct settings *s);
// Draws the menu over fb (240x160 BGR555, a copy of the paused frame).
void menu_draw(uint16_t *fb, const struct settings *s, int es);

// Rewind mode: over fb (already the selected moment's screen), the timeline strip and the time.
void rewind_draw(uint16_t *fb, int sel, int es, int swap_ab);

// Options files, "size=2\nlcd=0\n...": settings_defaults() then settings_read() each file in order
// (defaults.txt from gb5.py build, then settings.txt the menu writes); missing keys keep their value.
void settings_defaults(struct settings *s);
void settings_read(const char *path, struct settings *s);
void settings_save(const char *path, const struct settings *s);
