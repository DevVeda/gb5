// Veda Menu: L2 + R2 pauses the game and shows it, drawn at 240x160 over the whole screen in the style of
// Pokémon's OPTION screen (header box, tabs, rows on a grey band, the row under the cursor on white, labels in
// orange, the chosen value in red), then scaled with the picture so it looks native. L1/R1 change tabs.
// Same look for every game; the font is stb_easy_font's.
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

enum { PAD_UP = 0x10, PAD_RIGHT = 0x20, PAD_DOWN = 0x40, PAD_LEFT = 0x80, PAD_L1 = 0x400, PAD_R1 = 0x800, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000, PAD_OPTIONS = 0x8 };

static uint16_t rgb(unsigned r, unsigned g, unsigned b) { return (uint16_t)((b >> 3) << 10 | (g >> 3) << 5 | r >> 3); }
#define INK rgb(72, 72, 80)        // values, plain text
#define SHADOW rgb(208, 208, 200)  // text shadow, one pixel down-right
#define LABEL rgb(200, 120, 24)    // row names, title
#define LABEL_SH rgb(240, 200, 144)
#define HOT rgb(216, 40, 32)       // the chosen value, the tab showing
#define HOT_SH rgb(240, 168, 160)
#define EDGE rgb(88, 80, 112)      // outer border
#define RIM rgb(168, 160, 192)     // inner border, greyed-out text
#define PAPER rgb(248, 248, 248)   // box, the row under the cursor
#define BAND rgb(192, 188, 200)    // the other rows
#define BACKDROP rgb(136, 144, 232)

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

// Text with a shadow one pixel down-right; button glyphs drawn here, the rest through stb_easy_font.
static void text(uint16_t *fb, int x, int y, const char *s, uint16_t ink, uint16_t shadow) {
  char run[128];
  int n = 0;
  for (;; s++) {
    if (*s == 1 || *s == 2 || !*s) {
      run[n] = 0;
      if (n) {
        glyphs(fb, x + 1, y + 1, run, shadow);
        glyphs(fb, x, y, run, ink);
        x += stb_easy_font_width(run);
      }
      n = 0;
      if (!*s) return;
      icon(fb, x + 2, y, *s, shadow);
      icon(fb, x + 1, y - 1, *s, ink);
      x += 9;
    } else if (n < (int)sizeof run - 1) {
      run[n++] = *s;
    }
  }
}

static void label(uint16_t *fb, int x, int y, const char *s) { text(fb, x, y, s, INK, SHADOW); }

static void cursor(uint16_t *fb, int x, int y) {  // ▶, 4 wide, 7 tall
  for (int i = 0; i < 4; i++) rect(fb, x + i, y + i, 1, 7 - 2 * i, INK);
}

// ---------------- state ----------------
enum { TAB_GAME, TAB_MULTI, TAB_ACH, TAB_CHEATS, TAB_OPTIONS, TAB_COUNT };
// Row kinds. Options keep the order OPTIONS shows them in; the sensor ones only where the cartridge has them.
enum { R_RESUME, R_SAVE, R_LOAD, R_TURBO, R_RESET, R_QUIT,
       R_PLAYERS, R_MP_AUDIO, R_PLAYER, R_APPLY,
       R_ACH, R_UPDATE, R_CHEAT, R_GAME,
       R_SIZE, R_LCD, R_COLOR, R_SWAP, R_HARDCORE, R_LB_POPUPS, R_RUMBLE, R_SUN, R_GYRO, R_GYRO_SENS, R_TILT_SENS, R_CENTER };
#define ROWS 7  // rows the body box holds
#define MAX_ROWS 512

static int open_, tab, sel, picking, prev_buttons, held_frames, slot = 1, slots, ach_count = -1;
static int tab_sel[TAB_COUNT];  // where the cursor was on each tab: the menu reopens there
static int cheat_upd;  // CHEATS has its UPDATE row
static int players = 1, want = 1;  // linked now / on the PLAYERS row
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

int menu_slot(void) { return slot; }

void menu_open(int slots_used, unsigned hw) {
  slots = slots_used;
  cart_hw = hw;
  open_ = 1;
  picking = 0;
  sel = tab_sel[tab];
  if (tab == TAB_ACH) ach_count = ra_list_refresh();
  prev_buttons = ~0u;  // ignore the buttons that opened it until they're let go
  held_frames = 0;
  status[0] = 0;
  want = players;
}

int menu_is_open(void) { return open_; }

// One row of the body: its kind (and which cheat / achievement / player / game), and how it's drawn.
struct row {
  int id, arg;
  const char *name;
  const char *const *opt;  // values shown side by side, opt[val] in red ...
  int nopt, val;
  char value[40];          // ... or one value: in red between < > when `arrows`, else plain on the right
  int arrows, grey;
};

// The rows the tab now showing has, and in *head the line above them ("" for none).
static int rows_of(struct row *r, const struct settings *s, int es, const char **head) {
  static const char *const on_off[2][2] = {{"ON", "OFF"}, {"SI", "NO"}}, *const slots_txt[] = {"1", "2", "3"},
                           *const turbo_txt[] = {"X1", "X2", "X4"}, *const players_txt[] = {"1", "2", "3", "4"},
                           *const ab_txt[] = {BTN_CROSS, BTN_CIRCLE}, *const axes[] = {"X+", "X-", "Y+", "Y-", "Z+", "Z-"},
                           *const size_txt[2][3] = {{"INTEGER X6", "FILL HEIGHT", "16:9"}, {"ENTERO X6", "LLENAR ALTO", "16:9"}},
                           *const mpa_txt[2][3] = {{"TV + PADS", "TV ONLY", "PADS ONLY"}, {"TV + MANDOS", "SOLO TV", "SOLO MANDOS"}};
  static char head_buf[48];
  const char *const *yn = on_off[es];
  int n = 0;
#define ROW(...) (r[n++] = (struct row){__VA_ARGS__})
  *head = "";
  if (picking) {  // a new player's game
    snprintf(head_buf, sizeof head_buf, es ? "JUEGO DEL JUGADOR %d" : "PLAYER %d'S GAME", picking + 1);
    *head = head_buf;
    for (int i = 0; i < game_count && n < MAX_ROWS; i++) ROW(R_GAME, i, game_names[i]);
  } else if (tab == TAB_GAME) {
    ROW(R_RESUME, 0, es ? "CONTINUAR" : "RESUME");
    ROW(R_SAVE, 0, es ? "GUARDAR ESTADO" : "SAVE STATE", slots_txt, 3, slot - 1);
    ROW(R_LOAD, 0, es ? "CARGAR ESTADO" : "LOAD STATE", slots_txt, 3, slot - 1, .grey = !(slots & 1 << slot) || s->hardcore || players > 1);  // empty slot, hardcore, linked
    ROW(R_TURBO, 0, "TURBO", turbo_txt, 3, s->turbo == 4 ? 2 : s->turbo == 2 ? 1 : 0);
    ROW(R_RESET, 0, es ? "REINICIAR" : "RESET");
    ROW(R_QUIT, 0, es ? "SALIR" : "QUIT");
  } else if (tab == TAB_MULTI) {
    ROW(R_PLAYERS, 0, es ? "JUGADORES" : "PLAYERS", players_txt, 4, want - 1);
    ROW(R_MP_AUDIO, 0, es ? "SONIDO" : "SOUND", .arrows = 1);
    snprintf(r[n - 1].value, sizeof r[n - 1].value, "%s", mpa_txt[es][s->mp_audio]);
    for (int p = players; p < want && game_count > 1; p++) {  // each new player's game: X opens the list
      ROW(R_PLAYER, p, NULL);
      snprintf(r[n - 1].value, sizeof r[n - 1].value, "%.16s", game_names[game_sel[p]]);
    }
    if (want != players) ROW(R_APPLY, 0, es ? "APLICAR" : "APPLY");
  } else if (tab == TAB_ACH) {
    if (ach_count < 0) *head = es ? "RETROACHIEVEMENTS NO CONECTADO" : "RETROACHIEVEMENTS NOT CONNECTED";
    else if (!ach_count) *head = es ? "ESTE JUEGO NO TIENE LOGROS" : "THIS GAME HAS NO ACHIEVEMENTS";
    else {
      unsigned got, all, pg, pa;
      ra_list_totals(&got, &all, &pg, &pa);
      snprintf(head_buf, sizeof head_buf, es ? "LOGROS %u/%u   PUNTOS %u/%u" : "ACHIEVEMENTS %u/%u   POINTS %u/%u", got, all, pg, pa);
      *head = head_buf;
    }
    for (int i = 0; i < ach_count && n < MAX_ROWS; i++) {
      ROW(R_ACH, i, NULL, .grey = !ra_list_unlocked(i));
      snprintf(r[n - 1].value, sizeof r[n - 1].value, "%u", ra_list_points(i));
    }
  } else if (tab == TAB_CHEATS) {
    if (s->hardcore) *head = es ? "SIN CHEATS EN HARDCORE" : "NO CHEATS IN HARDCORE";
    else {
      if (!cheats_count()) *head = es ? "NO HAY CHEATS PARA ESTE JUEGO" : "NO CHEATS FOR THIS GAME";
      if (cheat_upd) ROW(R_UPDATE, 0, es ? "ACTUALIZAR DESDE INTERNET" : "UPDATE FROM THE INTERNET");
      for (int i = 0; i < cheats_count() && n < MAX_ROWS; i++) ROW(R_CHEAT, i, NULL);
    }
  } else {
    ROW(R_SIZE, 0, es ? "TAMANO" : "SIZE", .arrows = 1);
    snprintf(r[n - 1].value, sizeof r[n - 1].value, "%s", size_txt[es][s->size]);
    ROW(R_LCD, 0, es ? "FILTRO LCD" : "LCD GRID", yn, 2, !s->lcd);
    ROW(R_COLOR, 0, es ? "COLOR GBA" : "GBA COLORS", yn, 2, !s->color);
    ROW(R_SWAP, 0, es ? "BOTON A" : "A BUTTON", ab_txt, 2, !s->swap_ab);
    ROW(R_HARDCORE, 0, "HARDCORE", yn, 2, !s->hardcore);
    ROW(R_LB_POPUPS, 0, es ? "AVISOS CLASIF." : "LB ALERTS", yn, 2, !s->lb_popups);
    if (cart_hw & HW_RUMBLE) ROW(R_RUMBLE, 0, es ? "VIBRACION" : "RUMBLE", yn, 2, !s->rumble);
    if (cart_hw & HW_LIGHT) { ROW(R_SUN, 0, es ? "SOL" : "SUN", .arrows = 1); snprintf(r[n - 1].value, 8, "%d", s->sun); }
    if (cart_hw & HW_GYRO) {
      ROW(R_GYRO, 0, es ? "GIRO" : "GYRO", axes, 6, s->gyro, .arrows = 1);
      snprintf(r[n - 1].value, 8, "%s", axes[s->gyro]);
      ROW(R_GYRO_SENS, 0, es ? "SENS. GIRO" : "GYRO SENS.", .arrows = 1);
      snprintf(r[n - 1].value, 8, "%d", s->gyro_sens);
    }
    if (cart_hw & HW_TILT) {
      ROW(R_TILT_SENS, 0, es ? "SENS. INCLINACION" : "TILT SENS.", .arrows = 1);
      snprintf(r[n - 1].value, 8, "%d", s->tilt_sens);
      ROW(R_CENTER, 0, es ? "CENTRAR INCLINACION" : "LEVEL TILT HERE");
    }
  }
#undef ROW
  return n;
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int menu_input_(uint32_t b, struct settings *s);

int menu_input(uint32_t b, struct settings *s) {
  int act = menu_input_(b, s);
  if (!picking) tab_sel[tab] = sel;
  return act;
}

static int menu_input_(uint32_t b, struct settings *s) {
  if (prev_buttons == ~0u) {  // wait for every button to be released after opening
    if (b & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_CROSS | PAD_CIRCLE | PAD_OPTIONS | PAD_L1 | PAD_R1 | 0x300)) return MENU_NONE;
    prev_buttons = 0;
  }
  uint32_t press = b & ~prev_buttons;
  // held up/down repeats after ~1/3 s, every 6 frames
  held_frames = b & (PAD_UP | PAD_DOWN) ? held_frames + 1 : 0;
  if (held_frames > 20 && held_frames % 6 == 0) press |= b & (PAD_UP | PAD_DOWN);
  prev_buttons = b;

  if (picking) {
    if (press & PAD_CIRCLE) { sel = 2 + picking - players; picking = 0; return MENU_NONE; }  // back on that player's row
  } else {
    if (press & (PAD_CIRCLE | PAD_OPTIONS)) { open_ = 0; return MENU_CLOSED; }
    if (press & (PAD_L1 | PAD_R1)) {
      tab = (tab + (press & PAD_L1 ? TAB_COUNT - 1 : 1)) % TAB_COUNT;
      sel = tab_sel[tab];
      status[0] = 0;
      if (tab == TAB_ACH) ach_count = ra_list_refresh();
    }
  }
  static struct row rows[MAX_ROWS];
  const char *head;
  int count = rows_of(rows, s, 0, &head);
  if (!count) return MENU_NONE;
  if (sel >= count) sel = count - 1;
  if (press & PAD_UP) sel = (sel + count - 1) % count;
  if (press & PAD_DOWN) sel = (sel + 1) % count;
  int dir = press & PAD_LEFT ? -1 : press & PAD_RIGHT ? 1 : 0;
  int cross = !!(press & PAD_CROSS);
  struct row *r = &rows[sel];
  // ✕ on a row with a choice moves it on, as right does
  int step = dir ? dir : cross && (r->opt || r->arrows) && r->id != R_SAVE && r->id != R_LOAD ? 1 : 0;

  switch (r->id) {
    case R_RESUME: if (cross) { open_ = 0; return MENU_CLOSED; } break;
    case R_SAVE: case R_LOAD:
      if (dir) slot = (slot - 1 + dir + 3) % 3 + 1;
      if (cross && r->id == R_SAVE) { open_ = 0; return MENU_SAVE_STATE; }
      if (cross && !r->grey) { open_ = 0; return MENU_LOAD_STATE; }  // not empty, not hardcore, not linked
      break;
    case R_TURBO: if (step) s->turbo = (int[]){1, 2, 4}[(r->val + step + 3) % 3]; break;
    case R_RESET: if (cross) { open_ = 0; return MENU_RESET; } break;
    case R_QUIT: if (cross) { open_ = 0; return MENU_QUIT; } break;
    case R_PLAYERS: if (step) want = clamp(want + step, 1, 4); break;
    case R_MP_AUDIO: if (step) s->mp_audio = (s->mp_audio + step + MP_AUDIO_COUNT) % MP_AUDIO_COUNT; break;
    case R_PLAYER: if (cross) { picking = r->arg; sel = game_sel[picking]; status[0] = 0; } break;
    case R_APPLY: if (cross) return MENU_PLAYERS; break;  // stays open: main may put a status line
    case R_GAME: if (cross) { game_sel[picking] = r->arg; sel = 2 + picking - players; picking = 0; } break;
    case R_UPDATE: if (cross) return MENU_UPDATE_CHEATS; break;
    case R_CHEAT: if (cross) cheats_toggle(r->arg); break;
    case R_SIZE: if (step) s->size = (s->size + step + SIZE_COUNT) % SIZE_COUNT; break;
    case R_LCD: if (step) s->lcd = !s->lcd; break;
    case R_COLOR: if (step) s->color = !s->color; break;
    case R_SWAP: if (step) s->swap_ab = !s->swap_ab; break;
    case R_LB_POPUPS: if (step) s->lb_popups = !s->lb_popups; break;
    case R_RUMBLE: if (step) s->rumble = !s->rumble; break;
    case R_SUN: if (step) s->sun = clamp(s->sun + step, 0, 10); break;
    case R_GYRO: if (step) s->gyro = (s->gyro + step + 6) % 6; break;
    case R_GYRO_SENS: if (step) s->gyro_sens = clamp(s->gyro_sens + step, 1, 10); break;
    case R_TILT_SENS: if (step) s->tilt_sens = clamp(s->tilt_sens + step, 1, 10); break;
    case R_CENTER: if (cross) return MENU_RECENTER; break;
    case R_HARDCORE:  // on: main resets the game (RetroAchievements' rule), so back to it
      if (!step) break;
      s->hardcore = !s->hardcore;
      if (s->hardcore) open_ = 0;
      return MENU_HARDCORE;
  }
  return MENU_NONE;
}

// Fits s in w pixels, cutting at the end.
static void fit(char *s, int w) {
  for (int n = (int)strlen(s); n > 0 && text_width(s) > w; n--) s[n - 1] = 0;
}

void menu_draw(uint16_t *fb, const struct settings *s, int es) {
  static const char *const tabs[2][TAB_COUNT] = {{"GAME", "MULTI", "CHEEVOS", "CHEATS", "OPTIONS"},
                                                 {"PARTIDA", "MULTI", "LOGROS", "CHEATS", "OPCIONES"}};
  static struct row rows[MAX_ROWS];
  char line[64], txt[200];
  const char *head;
  int n = rows_of(rows, s, es, &head);

  // header: the title, RetroAchievements' mode, the tabs between L1 and R1
  rect(fb, 0, 0, 240, 160, BACKDROP);
  box(fb, 4, 2, 232, 32);
  text(fb, 12, 8, "VEDA MENU", LABEL, LABEL_SH);
  const char *mode = s->hardcore ? "HARDCORE" : "CASUAL";
  text(fb, 228 - text_width(mode), 8, mode, s->hardcore ? HOT : INK, s->hardcore ? HOT_SH : SHADOW);
  label(fb, 120 - text_width("L1 / R1") / 2, 8, "L1 / R1");
  int tw = 0;
  for (int i = 0; i < TAB_COUNT; i++) tw += text_width(tabs[es][i]);
  int gap = (216 - tw) / (TAB_COUNT - 1), tx = 12;  // the tabs spread over x 12..228
  for (int i = 0; i < TAB_COUNT; i++) {
    int w = text_width(tabs[es][i]), on = i == tab;
    text(fb, tx, 21, tabs[es][i], on ? HOT : i == TAB_CHEATS && s->hardcore ? RIM : INK, on ? HOT_SH : SHADOW);
    if (on) rect(fb, tx, 29, w, 1, HOT);
    tx += w + gap;
  }

  // body: the head line, then the rows on the grey band, the one under the cursor on white
  box(fb, 4, 36, 232, 122);
  rect(fb, 8, 40, 224, 114, BAND);
  int lead = head[0] ? 1 : 0, desc = tab == TAB_ACH && !picking && sel < n ? 2 : 0;  // achievements: 2 rows for the description
  int vis = ROWS - lead - desc - (status[0] != 0), top = sel >= vis ? sel - vis + 1 : 0;
  if (lead) label(fb, 14, 44, head);
  for (int k = 0; k < vis && top + k < n; k++) {
    struct row *r = &rows[top + k];
    int ry = 40 + (k + lead) * 15, ty = ry + 4;
    if (top + k == sel) { rect(fb, 8, ry, 224, 15, PAPER); cursor(fb, 11, ty); }
    const char *name = r->name;
    if (r->id == R_PLAYER) snprintf(line, sizeof line, es ? "JUGADOR %d" : "PLAYER %d", r->arg + 1), name = line;
    if (r->id == R_CHEAT) snprintf(line, sizeof line, "[%c] %.40s", cheats_enabled(r->arg) ? 'X' : ' ', cheats_name(r->arg)), name = line;
    if (r->id == R_ACH) text_ascii(txt, sizeof txt, ra_list_title(r->arg)), snprintf(line, sizeof line, "%s", txt), name = line;
    if (r->id == R_GAME) snprintf(line, sizeof line, "%.40s", r->name), name = line;
    if (name == line) {
      for (char *c = line; *c; c++) if ((unsigned char)*c < 32 || (unsigned char)*c > 126) *c = '?';  // font is ASCII
      fit(line, r->value[0] ? 150 : 200);
    }
    if (r->grey) glyphs(fb, 18, ty, name, RIM);  // greyed: no shadow
    else text(fb, 18, ty, name, LABEL, LABEL_SH);
    if (r->arrows) {  // < value >, centred over the values' column
      snprintf(txt, sizeof txt, "<  %s  >", r->value);
      text(fb, 172 - text_width(txt) / 2, ty, txt, HOT, HOT_SH);
    } else if (r->opt) {  // every value side by side, the chosen one in red
      int cw = 96 / r->nopt;
      for (int o = 0; o < r->nopt; o++) {
        int hot = o == r->val && !r->grey;
        text(fb, 124 + o * cw + (cw - text_width(r->opt[o])) / 2, ty, r->opt[o], hot ? HOT : INK, hot ? HOT_SH : SHADOW);
      }
    } else if (r->value[0]) {
      label(fb, 218 - text_width(r->value), ty, r->value);
    }
  }
  if (top > 0) label(fb, 224, 40 + lead * 15 + 4, "^");
  if (top + vis < n) label(fb, 224, 40 + (lead + vis - 1) * 15 + 4, "v");
  if (desc) {  // the achievement's description, two lines at most, plus measured progress
    int by = 40 + (ROWS - desc - (status[0] != 0)) * 15 + 2;
    rect(fb, 8, by - 2, 224, 1, RIM);
    text_ascii(txt, sizeof txt, ra_list_desc(sel));
    const char *prog = ra_list_progress(sel);
    int avail = 200 - (prog[0] ? text_width(prog) + 8 : 0);  // keep the progress clear of the text
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
      label(fb, 14, by + 4 + l * 12, p);
      p[cut] = save;
      p += cut;
      while (*p == ' ') p++;
    }
    if (prog[0]) label(fb, 218 - text_width(prog), by + 4, prog);
  }
  if (status[0]) glyphs(fb, 14, 40 + (ROWS - 1) * 15 + 4, status, EDGE);  // why a player couldn't join, the cheat update's result
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
  label(fb, bx + (bw - text_width(line)) / 2, by + 38, line);
}

// ---------------- settings.txt ----------------
void settings_defaults(struct settings *s) {
  *s = (struct settings){SIZE_STRETCH, 0, 0, 1, 1, 5, 4, 1, 5, 5, 0, 0, MP_AUDIO_TV_PADS};  // 16:9, no grid, own colours, x1, A = Cross, half sun, gyro Z+, rumble on, sens 5, casual, no leaderboard alerts, TV + pads
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
    if (!strcmp(k, "mpaudio") && v >= 0 && v < MP_AUDIO_COUNT) s->mp_audio = v;
  }
  if (f) fclose(f);
}

void settings_save(const char *path, const struct settings *s) {
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "size=%d\nlcd=%d\ncolor=%d\nturbo=%d\nswapab=%d\nsun=%d\ngyro=%d\nrumble=%d\ngyrosens=%d\ntiltsens=%d\nhardcore=%d\nlbpopups=%d\nmpaudio=%d\n", s->size,
          s->lcd, s->color, s->turbo, s->swap_ab, s->sun, s->gyro, s->rumble, s->gyro_sens, s->tilt_sens, s->hardcore, s->lb_popups, s->mp_audio);
  fclose(f);
  chmod(path, 0666);
}
