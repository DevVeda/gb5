// The extra menu: L2 + R2 pauses the game and shows it, drawn at 240x160 like a GBA menu
// (white box, dark rounded border, ▶ cursor, grey text with a light shadow), then scaled with
// the picture so it looks native. Same look for every game; the font is stb_easy_font's.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu.h"
#include "cheats.h"
#include "card.h"
#include "ra.h"
#include "rewind.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "stb_easy_font.h"

enum { PAD_UP = 0x10, PAD_RIGHT = 0x20, PAD_DOWN = 0x40, PAD_LEFT = 0x80, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000, PAD_OPTIONS = 0x8 };

static uint16_t rgb(unsigned r, unsigned g, unsigned b) { return (uint16_t)((b >> 3) << 10 | (g >> 3) << 5 | r >> 3); }
#define INK rgb(72, 72, 80)       // text
#define SHADOW rgb(208, 208, 200) // text shadow, one pixel down-right
#define EDGE rgb(64, 80, 104)     // outer border
#define RIM rgb(168, 184, 200)    // inner border
#define PAPER rgb(248, 248, 248)  // box

static void rect(uint16_t *fb, int x0, int y0, int w, int h, uint16_t c) {
  for (int y = y0 < 0 ? 0 : y0; y < y0 + h && y < 160; y++)
    for (int x = x0 < 0 ? 0 : x0; x < x0 + w && x < 240; x++) fb[y * 240 + x] = c;
}

// The box: 1-pixel dark edge, 1-pixel light rim, white inside, corners rounded off.
static void box(uint16_t *fb, int x, int y, int w, int h) {
  rect(fb, x + 2, y, w - 4, h, EDGE);
  rect(fb, x, y + 2, w, h - 4, EDGE);
  rect(fb, x + 1, y + 1, w - 2, h - 2, EDGE);
  rect(fb, x + 2, y + 1, w - 4, h - 2, RIM);
  rect(fb, x + 1, y + 2, w - 2, h - 4, RIM);
  rect(fb, x + 3, y + 2, w - 6, h - 4, PAPER);
  rect(fb, x + 2, y + 3, w - 4, h - 6, PAPER);
}

static void glyphs(uint16_t *fb, int x, int y, const char *s, uint16_t c) {
  static char vb[16 * 1024];
  int quads = stb_easy_font_print(0, 0, (char *)s, NULL, vb, sizeof vb);
  for (int q = 0; q < quads; q++) {
    float *v = (float *)(vb + q * 64);  // 4 vertices of 16 bytes: x, y, z, rgba
    rect(fb, x + (int)v[0], y + (int)v[1], (int)(v[8] - v[0]), (int)(v[9] - v[1]), c);
  }
}

static void label(uint16_t *fb, int x, int y, const char *s) {
  glyphs(fb, x + 1, y + 1, s, SHADOW);
  glyphs(fb, x, y, s, INK);
}

// PlayStation button glyphs, 7x7, written in menu text as BTN_CROSS / BTN_CIRCLE.
#define BTN_CROSS "\x01"
#define BTN_CIRCLE "\x02"
static const char *const glyph_px[3] = {
    NULL,
    "X.....X" ".X...X." "..X.X.." "...X..." "..X.X.." ".X...X." "X.....X",  // cross
    "..XXX.." ".X...X." "X.....X" "X.....X" "X.....X" ".X...X." "..XXX..",  // circle
};

static void icon(uint16_t *fb, int x, int y, int which, uint16_t c) {
  for (int i = 0; i < 49; i++)
    if (glyph_px[which][i] == 'X') rect(fb, x + i % 7, y + i / 7, 1, 1, c);
}

// Width of a menu string, button glyphs included.
static int text_width(const char *s) {
  char run[128];
  int w = 0, n = 0;
  for (;; s++) {
    if (*s == 1 || *s == 2 || !*s) {
      run[n] = 0;
      w += n ? stb_easy_font_width(run) : 0;
      n = 0;
      if (!*s) return w;
      w += 9;
    } else if (n < (int)sizeof run - 1) {
      run[n++] = *s;
    }
  }
}

// label() with button glyphs: plain runs through stb_easy_font, glyphs drawn here (same shadow).
static void label_glyphs(uint16_t *fb, int x, int y, const char *s) {
  char run[128];
  int n = 0;
  for (;; s++) {
    if (*s == 1 || *s == 2 || !*s) {
      run[n] = 0;
      if (n) { label(fb, x, y, run); x += stb_easy_font_width(run); }
      n = 0;
      if (!*s) return;
      icon(fb, x + 2, y, *s, SHADOW);
      icon(fb, x + 1, y - 1, *s, INK);
      x += 9;
    } else if (n < (int)sizeof run - 1) {
      run[n++] = *s;
    }
  }
}

static void cursor(uint16_t *fb, int x, int y) {  // ▶, 4 wide, 7 tall
  for (int i = 0; i < 4; i++) rect(fb, x + i, y + i, 1, 7 - 2 * i, INK);
}

// ---------------- state ----------------
enum { PAGE_MAIN, PAGE_DISPLAY, PAGE_CHEATS, PAGE_ACH, PAGE_GAMES };
#define ACH_ROWS 6  // achievements visible at once
#define CHEAT_ROWS 9  // visible at once; the list scrolls
enum { M_RESUME, M_SAVE, M_LOAD, M_ACH, M_CHEATS, M_DISPLAY, M_TURBO, M_PLAYERS, M_RESET, M_QUIT, M_COUNT };
enum { D_SIZE, D_LCD, D_COLOR, D_SWAP, D_HARDCORE, D_LB_POPUPS, D_RUMBLE, D_SUN, D_GYRO, D_GYRO_SENS, D_TILT_SENS, D_CENTER, D_BACK, D_COUNT };

static int open_, page, sel, prev_buttons, held_frames, slot = 1, slots, ach_count = -1;
static int main_sel;  // where the cursor was on the main page: the menu reopens there
static int cheat_upd;  // CHEATS has its UPDATE row
static int players = 1, want = 1, picking;  // linked now / on the PLAYERS row / whose game PAGE_GAMES picks (1..3)
static char status[48];

void menu_cheats_updatable(int yes) { cheat_upd = !!yes; }
void menu_players(int n) { players = want = n; }
int menu_players_wanted(void) { return want; }
void menu_close(void) { open_ = 0; }
static const char *const *game_names;
static int game_count, game_sel[4];
void menu_games(const char *const *names, int n) { game_names = names, game_count = n; }
int menu_game(int p) { return p > 0 && p < 4 ? game_sel[p] : 0; }
void menu_status(const char *msg) { snprintf(status, sizeof status, "%s", msg); }
static unsigned cart_hw;

// OPTIONS rows this cartridge has: the sensor ones only where the hardware is.
static int options(int *rows) {
  int n = 0;
  for (int d = 0; d < D_COUNT; d++)
    if ((d != D_SUN || cart_hw & HW_LIGHT) && ((d != D_GYRO && d != D_GYRO_SENS) || cart_hw & HW_GYRO) &&
        ((d != D_CENTER && d != D_TILT_SENS) || cart_hw & HW_TILT) &&
        (d != D_RUMBLE || cart_hw & HW_RUMBLE))
      rows[n++] = d;
  return n;
}

int menu_slot(void) { return slot; }

void menu_open(int slots_used, unsigned hw) {
  slots = slots_used;
  cart_hw = hw;
  open_ = 1;
  page = PAGE_MAIN;
  sel = main_sel;
  prev_buttons = ~0u;  // ignore the buttons that opened it until they're let go
  held_frames = 0;
  status[0] = 0;
  want = players;
}

int menu_is_open(void) { return open_; }

static int next_turbo(int t, int dir) {
  static const int steps[] = {1, 2, 4};
  int i = t == 4 ? 2 : t == 2 ? 1 : 0;
  i = (i + dir + 3) % 3;
  return steps[i];
}

static int menu_input_(uint32_t b, struct settings *s);

int menu_input(uint32_t b, struct settings *s) {
  int act = menu_input_(b, s);
  if (page == PAGE_MAIN) main_sel = sel;
  return act;
}

static int menu_input_(uint32_t b, struct settings *s) {
  if (prev_buttons == ~0u) {  // wait for every button to be released after opening
    if (b & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_CROSS | PAD_CIRCLE | PAD_OPTIONS | 0x300)) return MENU_NONE;
    prev_buttons = 0;
  }
  uint32_t press = b & ~prev_buttons;
  // held up/down repeats after ~1/3 s, every 6 frames
  held_frames = b & (PAD_UP | PAD_DOWN) ? held_frames + 1 : 0;
  if (held_frames > 20 && held_frames % 6 == 0) press |= b & (PAD_UP | PAD_DOWN);
  prev_buttons = b;

  int rows[D_COUNT], nrows = options(rows);
  int count = page == PAGE_MAIN ? M_COUNT : page == PAGE_DISPLAY ? nrows : page == PAGE_ACH ? (ach_count > 0 ? ach_count : 0) + 1
              : page == PAGE_GAMES ? game_count + 1 : cheat_upd + cheats_count() + 1;  // [UPDATE] list BACK
  if (press & PAD_UP) sel = (sel + count - 1) % count;
  if (press & PAD_DOWN) sel = (sel + 1) % count;
  int dir = press & PAD_LEFT ? -1 : press & (PAD_RIGHT | PAD_CROSS) ? 1 : 0;

  if (page == PAGE_MAIN) {
    if (press & (PAD_CIRCLE | PAD_OPTIONS)) { open_ = 0; return MENU_CLOSED; }
    if (sel == M_TURBO && dir) s->turbo = next_turbo(s->turbo, dir);
    if (sel == M_PLAYERS && press & (PAD_LEFT | PAD_RIGHT)) want = want + dir < 1 ? 1 : want + dir > 4 ? 4 : want + dir;
    if ((sel == M_SAVE || sel == M_LOAD) && press & (PAD_LEFT | PAD_RIGHT)) slot = (slot - 1 + (press & PAD_LEFT ? 2 : 1)) % 3 + 1;
    if (press & PAD_CROSS) {
      if (sel == M_RESUME) { open_ = 0; return MENU_CLOSED; }
      if (sel == M_DISPLAY) { page = PAGE_DISPLAY; sel = 0; }
      else if (sel == M_CHEATS && !s->hardcore) { page = PAGE_CHEATS; sel = 0; status[0] = 0; }
      else if (sel == M_ACH) { page = PAGE_ACH; sel = 0; ach_count = ra_list_refresh(); }
      if (sel == M_SAVE) { open_ = 0; return MENU_SAVE_STATE; }
      if (sel == M_PLAYERS && want > players && game_count > 1) { page = PAGE_GAMES; picking = players; sel = 0; status[0] = 0; return MENU_NONE; }
      if (sel == M_PLAYERS && want != players) { memset(game_sel, 0, sizeof game_sel); return MENU_PLAYERS; }  // stays open: main may put a status line
      if (sel == M_LOAD && slots & 1 << slot && !s->hardcore && players == 1) { open_ = 0; return MENU_LOAD_STATE; }  // empty slot: nothing to load
      if (sel == M_RESET) { open_ = 0; return MENU_RESET; }
      if (sel == M_QUIT) { open_ = 0; return MENU_QUIT; }
    }
  } else if (page == PAGE_GAMES) {
    if (press & PAD_CIRCLE || (press & PAD_CROSS && sel == count - 1)) { page = PAGE_MAIN; sel = M_PLAYERS; want = players; return MENU_NONE; }
    if (press & PAD_CROSS) {  // one list per new player, then they join
      game_sel[picking++] = sel;
      sel = 0;
      if (picking < want) return MENU_NONE;
      page = PAGE_MAIN;
      sel = M_PLAYERS;
      return MENU_PLAYERS;
    }
  } else if (page == PAGE_ACH) {
    if (press & PAD_CIRCLE || (press & PAD_CROSS && sel == count - 1)) { page = PAGE_MAIN; sel = M_ACH; return MENU_NONE; }
  } else if (page == PAGE_CHEATS) {
    if (press & PAD_CIRCLE || (press & PAD_CROSS && sel == count - 1)) { page = PAGE_MAIN; sel = M_CHEATS; return MENU_NONE; }
    if (press & PAD_CROSS && cheat_upd && sel == 0) return MENU_UPDATE_CHEATS;
    if (press & PAD_CROSS) cheats_toggle(sel - cheat_upd);
  } else {
    int row = rows[sel];
    if (press & PAD_CIRCLE || (press & PAD_CROSS && row == D_BACK)) { page = PAGE_MAIN; sel = M_DISPLAY; return MENU_NONE; }
    if (dir && row == D_SIZE) s->size = (s->size + dir + SIZE_COUNT) % SIZE_COUNT;
    if (dir && row == D_LCD) s->lcd = !s->lcd;
    if (dir && row == D_COLOR) s->color = !s->color;
    if (dir && row == D_SWAP) s->swap_ab = !s->swap_ab;
    if (dir && row == D_RUMBLE) s->rumble = !s->rumble;
    if (dir && row == D_LB_POPUPS) s->lb_popups = !s->lb_popups;
    if (dir && row == D_SUN) s->sun = s->sun + dir < 0 ? 0 : s->sun + dir > 10 ? 10 : s->sun + dir;
    if (dir && row == D_GYRO) s->gyro = (s->gyro + dir + 6) % 6;
    if (dir && row == D_GYRO_SENS) s->gyro_sens = s->gyro_sens + dir < 1 ? 1 : s->gyro_sens + dir > 10 ? 10 : s->gyro_sens + dir;
    if (dir && row == D_TILT_SENS) s->tilt_sens = s->tilt_sens + dir < 1 ? 1 : s->tilt_sens + dir > 10 ? 10 : s->tilt_sens + dir;
    if (press & PAD_CROSS && row == D_CENTER) return MENU_RECENTER;
    if (dir && row == D_HARDCORE) {  // on: main resets the game (RetroAchievements' rule), so back to it
      s->hardcore = !s->hardcore;
      if (s->hardcore) { open_ = 0; page = PAGE_MAIN; }
      return MENU_HARDCORE;
    }
  }
  return MENU_NONE;
}

void menu_draw(uint16_t *fb, const struct settings *s, int es) {
  char line[48];
  if (page == PAGE_MAIN) {
    // the start menu's place: a column on the right
    static const char *en[] = {"RESUME", "SAVE STATE", "LOAD STATE", "ACHIEVEMENTS", "CHEATS", "OPTIONS", "TURBO", "PLAYERS", "RESET", "QUIT"},
                      *sp[] = {"CONTINUAR", "GUARDAR ESTADO", "CARGAR ESTADO", "LOGROS", "CHEATS", "OPCIONES", "TURBO", "JUGADORES", "REINICIAR", "SALIR"};
    const char **t = es ? sp : en;
    int x = 114, y = 4, w = 122, h = 10 + M_COUNT * 14;
    box(fb, x, y, w, h);
    if (status[0]) {  // why a player couldn't join: a box on the left
      box(fb, 4, 112, 106, 44);
      glyphs(fb, 12, 120, status, EDGE);
    }
    for (int i = 0; i < M_COUNT; i++) {
      if (i == M_TURBO) snprintf(line, sizeof line, "%s x%d", t[i], s->turbo);
      else if (i == M_PLAYERS) snprintf(line, sizeof line, "%s < %d >", t[i], want);
      else if (i == M_SAVE || i == M_LOAD) snprintf(line, sizeof line, "%s %d", t[i], slot);
      else snprintf(line, sizeof line, "%s", t[i]);
      if ((i == M_LOAD && (!(slots & 1 << slot) || s->hardcore || players > 1)) || (i == M_CHEATS && s->hardcore))
        glyphs(fb, x + 16, y + 8 + i * 14, line, SHADOW);  // empty slot, or not in hardcore / linked: greyed out
      else label(fb, x + 16, y + 8 + i * 14, line);
      if (i == sel) cursor(fb, x + 7, y + 8 + i * 14);
    }
  } else if (page == PAGE_ACH) {
    // header + list (top box), the selected one's description (bottom box)
    int n = ach_count > 0 ? ach_count : 0, top = sel >= ACH_ROWS ? sel - ACH_ROWS + 1 : 0;
    int x = 4, y = 4, w = 232, h = 26 + ACH_ROWS * 15;
    char txt[200];
    box(fb, x, y, w, h);
    if (ach_count < 0) label(fb, x + 16, y + 8, es ? "RETROACHIEVEMENTS NO CONECTADO" : "RETROACHIEVEMENTS NOT CONNECTED");
    else if (!n) label(fb, x + 16, y + 8, es ? "ESTE JUEGO NO TIENE LOGROS" : "THIS GAME HAS NO ACHIEVEMENTS");
    else {
      unsigned got, all, pg, pa;
      ra_list_totals(&got, &all, &pg, &pa);
      snprintf(line, sizeof line, es ? "LOGROS %u/%u   PUNTOS %u/%u" : "ACHIEVEMENTS %u/%u   POINTS %u/%u", got, all, pg, pa);
      label(fb, x + 8, y + 6, line);
    }
    for (int r = 0; r < ACH_ROWS && top + r <= n; r++) {
      int i = top + r, ry = y + 22 + r * 15;
      if (i == n) {
        label(fb, x + 16, ry, es ? "VOLVER" : "BACK");
      } else {
        text_ascii(txt, sizeof txt, ra_list_title(i));
        snprintf(line, sizeof line, "%c %.27s", ra_list_unlocked(i) ? '+' : ' ', txt);
        if (ra_list_unlocked(i)) label(fb, x + 16, ry, line);
        else glyphs(fb, x + 16, ry, line, RIM);  // locked: greyed
        snprintf(line, sizeof line, "%u", ra_list_points(i));
        label(fb, x + w - 10 - stb_easy_font_width(line), ry, line);
      }
      if (i == sel) cursor(fb, x + 7, ry);
    }
    if (top > 0) label(fb, x + w - 10, y + 20, "^");
    if (top + ACH_ROWS <= n) label(fb, x + w - 10, y + h - 10, "v");
    if (sel < n) {  // description, two lines at most, plus measured progress
      int by = y + h + 2;
      box(fb, x, by, w, 160 - by - 2);
      text_ascii(txt, sizeof txt, ra_list_desc(sel));
      char *prog = (char *)ra_list_progress(sel);
      int avail = w - 20 - (prog[0] ? stb_easy_font_width(prog) + 8 : 0);  // keep the progress clear of the text
      char *p = txt;
      for (int l = 0; l < 2 && *p; l++) {
        int cut = (int)strlen(p);
        char save;
        while (cut > 0) {  // longest prefix (at a space) that fits
          save = p[cut]; p[cut] = 0;
          int fits = stb_easy_font_width(p) <= avail;
          p[cut] = save;
          if (fits) break;
          while (--cut > 0 && p[cut] != ' ') {}
        }
        if (cut <= 0) cut = 36 < (int)strlen(p) ? 36 : (int)strlen(p);
        save = p[cut]; p[cut] = 0;
        label(fb, x + 10, by + 6 + l * 12, p);
        p[cut] = save;
        p += cut;
        while (*p == ' ') p++;
      }
      if (prog[0]) label(fb, x + w - 10 - stb_easy_font_width(prog), by + 6, prog);
    }
  } else if (page == PAGE_GAMES) {  // a new player's game: a scrolling list, BACK last
    int total = game_count + 1, rows = CHEAT_ROWS - 1, top = sel >= rows ? sel - rows + 1 : 0;
    int x = 4, y = 4, w = 232, h = 10 + CHEAT_ROWS * 15;
    box(fb, x, y, w, h);
    snprintf(line, sizeof line, es ? "JUEGO DEL JUGADOR %d" : "PLAYER %d'S GAME", picking + 1);
    label(fb, x + 8, y + 6, line);
    for (int r = 0; r < rows && top + r < total; r++) {
      int i = top + r, ry = y + 23 + r * 15;
      snprintf(line, sizeof line, "%.34s", i == total - 1 ? (es ? "VOLVER" : "BACK") : game_names[i]);
      label(fb, x + 16, ry, line);
      if (i == sel) cursor(fb, x + 7, ry);
    }
    if (top > 0) label(fb, x + w - 14, y + 18, "^");
    if (top + rows < total) label(fb, x + w - 14, y + h - 12, "v");
  } else if (page == PAGE_CHEATS) {
    // items: [UPDATE FROM THE INTERNET] cheats... BACK; the status line takes the last row when set
    int n = cheats_count(), total = cheat_upd + n + 1, lead = n ? 0 : 1, rows = CHEAT_ROWS - lead - (status[0] != 0);
    int top = sel >= rows ? sel - rows + 1 : 0;
    int x = 4, y = 4, w = 232, h = 10 + CHEAT_ROWS * 15;
    box(fb, x, y, w, h);
    if (!n) label(fb, x + 16, y + 8, es ? "NO HAY CHEATS PARA ESTE JUEGO" : "NO CHEATS FOR THIS GAME");
    for (int r = 0; r < rows && top + r < total; r++) {
      int i = top + r, ry = y + 8 + (r + lead) * 15;
      if (cheat_upd && i == 0) snprintf(line, sizeof line, "%s", es ? "ACTUALIZAR DESDE INTERNET" : "UPDATE FROM THE INTERNET");
      else if (i == total - 1) snprintf(line, sizeof line, "%s", es ? "VOLVER" : "BACK");
      else snprintf(line, sizeof line, "[%c] %.32s", cheats_enabled(i - cheat_upd) ? 'X' : ' ', cheats_name(i - cheat_upd));
      for (char *c = line; *c; c++) if ((unsigned char)*c < 32 || (unsigned char)*c > 126) *c = '?';  // font is ASCII
      label(fb, x + 16, ry, line);
      if (i == sel) cursor(fb, x + 7, ry);
    }
    if (top > 0) label(fb, x + w - 14, y + 4, "^");                // more above
    if (top + rows < total) label(fb, x + w - 14, y + h - 12, "v");  // more below
    if (status[0]) glyphs(fb, x + 16, y + h - 13, status, EDGE);
  } else {
    static const char *size_en[] = {"INTEGER X6", "FILL HEIGHT", "16:9"}, *size_es[] = {"ENTERO X6", "LLENAR ALTO", "16:9"};
    const char *on = es ? "SI" : "ON", *off = es ? "NO" : "OFF";
    static const char *axes[] = {"X+", "X-", "Y+", "Y-", "Z+", "Z-"};
    int rows[D_COUNT], nrows = options(rows);
    int x = 24, w = 192, h = 10 + nrows * 15, y = h > 144 ? (160 - h) / 2 : 8;  // 10 rows (gyro + rumble carts) fill the screen
    box(fb, x, y, w, h);
    for (int r = 0; r < nrows; r++) {
      int i = rows[r];
      if (i == D_SIZE) snprintf(line, sizeof line, "%s: < %s >", es ? "TAMANO" : "SIZE", (es ? size_es : size_en)[s->size]);
      if (i == D_LCD) snprintf(line, sizeof line, "%s: < %s >", es ? "FILTRO LCD" : "LCD GRID", s->lcd ? on : off);
      if (i == D_COLOR) snprintf(line, sizeof line, "%s: < %s >", es ? "COLOR GBA" : "GBA COLORS", s->color ? on : off);
      if (i == D_SWAP) snprintf(line, sizeof line, "%s: < %s >", es ? "BOTON A" : "A BUTTON", s->swap_ab ? BTN_CROSS : BTN_CIRCLE);
      if (i == D_HARDCORE) snprintf(line, sizeof line, "HARDCORE: < %s >", s->hardcore ? on : off);
      if (i == D_LB_POPUPS) snprintf(line, sizeof line, "%s: < %s >", es ? "AVISOS CLASIFICACION" : "LEADERBOARD ALERTS", s->lb_popups ? on : off);
      if (i == D_RUMBLE) snprintf(line, sizeof line, "%s: < %s >", es ? "VIBRACION" : "RUMBLE", s->rumble ? on : off);
      if (i == D_SUN) snprintf(line, sizeof line, "%s: < %d >", es ? "SOL" : "SUN", s->sun);
      if (i == D_GYRO) snprintf(line, sizeof line, "%s: < %s >", es ? "GIRO" : "GYRO", axes[s->gyro]);
      if (i == D_GYRO_SENS) snprintf(line, sizeof line, "%s: < %d >", es ? "SENS. GIRO" : "GYRO SENS.", s->gyro_sens);
      if (i == D_TILT_SENS) snprintf(line, sizeof line, "%s: < %d >", es ? "SENS. INCLINACION" : "TILT SENS.", s->tilt_sens);
      if (i == D_CENTER) snprintf(line, sizeof line, "%s", es ? "CENTRAR INCLINACION" : "LEVEL TILT HERE");
      if (i == D_BACK) snprintf(line, sizeof line, "%s", es ? "VOLVER" : "BACK");
      label_glyphs(fb, x + 16, y + 8 + r * 15, line);
      if (r == sel) cursor(fb, x + 7, y + 8 + r * 15);
    }
  }
}

// ---------------- rewind mode ----------------
// The selected moment fills the screen (the caller copied it into fb); on top, how far back it is,
// and along the bottom five thumbnails around it, older to the left, like a video scrub bar.
void rewind_draw(uint16_t *fb, int sel, int es, int swap_ab) {
  static uint16_t thumbs[5][40 * 26];
  char line[48];
  for (int k = 0; k < 5; k++) {  // copy first: the strip is drawn over fb, which is one of them
    const uint16_t *src = rewind_thumb(sel + 2 - k);
    for (int y = 0; y < 26; y++)
      for (int x = 0; x < 40; x++) thumbs[k][y * 40 + x] = src ? src[(y * 160 / 26) * 240 + x * 6] : 0;
  }
  box(fb, 4, 4, 70, 18);
  snprintf(line, sizeof line, "<< -%d S", sel);
  label(fb, 12, 9, line);
  int bx = 4, by = 108, bw = 232, bh = 50;
  box(fb, bx, by, bw, bh);
  for (int k = 0; k < 5; k++) {
    int tx = bx + 6 + k * 45, ty = by + 6;
    if (!rewind_thumb(sel + 2 - k)) continue;  // before the oldest / after now
    if (k == 2) rect(fb, tx - 2, ty - 2, 44, 30, EDGE);  // the selected one, framed
    for (int y = 0; y < 26; y++) memcpy(fb + (ty + y) * 240 + tx, thumbs[k] + y * 40, 40 * 2);
  }
  const char *a = swap_ab ? BTN_CROSS : BTN_CIRCLE, *b = swap_ab ? BTN_CIRCLE : BTN_CROSS;
  snprintf(line, sizeof line, es ? "%s VOLVER AQUI     %s CANCELAR" : "%s GO BACK HERE     %s CANCEL", a, b);
  label_glyphs(fb, bx + (bw - text_width(line)) / 2, by + 38, line);
}

// ---------------- settings.txt ----------------
void settings_defaults(struct settings *s) {
  *s = (struct settings){SIZE_STRETCH, 0, 0, 1, 1, 5, 4, 1, 5, 5, 0, 0};  // 16:9, no grid, own colours, x1, A = Cross, half sun, gyro Z+, rumble on, sens 5, casual, no leaderboard alerts
}

void settings_read(const char *path, struct settings *s) {
  FILE *f = fopen(path, "r");
  char k[16];
  int v;
  while (f && fscanf(f, " %15[a-z]=%d", k, &v) == 2) {
    if (!strcmp(k, "size") && v >= 0 && v < SIZE_COUNT) s->size = v;
    if (!strcmp(k, "lcd")) s->lcd = !!v;
    if (!strcmp(k, "color")) s->color = !!v;
    if (!strcmp(k, "turbo") && (v == 1 || v == 2 || v == 4)) s->turbo = v;
    if (!strcmp(k, "swapab")) s->swap_ab = !!v;
    if (!strcmp(k, "sun") && v >= 0 && v <= 10) s->sun = v;
    if (!strcmp(k, "gyro") && v >= 0 && v < 6) s->gyro = v;
    if (!strcmp(k, "rumble")) s->rumble = !!v;
    if (!strcmp(k, "gyrosens") && v >= 1 && v <= 10) s->gyro_sens = v;
    if (!strcmp(k, "tiltsens") && v >= 1 && v <= 10) s->tilt_sens = v;
    if (!strcmp(k, "hardcore")) s->hardcore = !!v;
    if (!strcmp(k, "lbpopups")) s->lb_popups = !!v;
  }
  if (f) fclose(f);
}

void settings_save(const char *path, const struct settings *s) {
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "size=%d\nlcd=%d\ncolor=%d\nturbo=%d\nswapab=%d\nsun=%d\ngyro=%d\nrumble=%d\ngyrosens=%d\ntiltsens=%d\nhardcore=%d\nlbpopups=%d\n", s->size,
          s->lcd, s->color, s->turbo, s->swap_ab, s->sun, s->gyro, s->rumble, s->gyro_sens, s->tilt_sens, s->hardcore, s->lb_popups);
  fclose(f);
  chmod(path, 0666);
}
