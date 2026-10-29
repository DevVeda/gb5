// GB5 title frontend: gba_rt (the GBA) -> VideoOut, AudioOut, DualSense.
// The title folder (/app0 while it runs) holds game.gba; each PS5 user's battery save, states and options
// go to /app0/user/<user id>/.
// VideoOut copy after an earlier port of ours (itself after PSXS5's ps5_video.c, GPL-3.0-or-later);
// pad and audio declarations as PS5_VulkanTemplate's ps5/src/platform.c (MIT) drives them.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "card.h"
#include "cheats.h"
#include "menu.h"
#include "ra.h"
#include "rewind.h"

// gba_rt (runtime/src/lib.rs)
void *gba_new(const uint8_t *rom, size_t len);
void *gba_new_bios(const uint8_t *rom, size_t len, const uint8_t *bios, size_t bios_len);
const uint16_t *gba_frame(void *m, uint16_t keys);
size_t gba_audio(void *m, int16_t *out, size_t max);
const uint8_t *gba_save(const void *m, size_t *len);
void gba_load_save(void *m, const uint8_t *data, size_t len);
void gba_free(void *m);
const char *gba_save_kind(const void *m);
uint32_t gba_hw(const void *m);
void gba_hw_input(void *m, int32_t gyro, int32_t tilt_x, int32_t tilt_y, uint8_t light);
uint8_t gba_hw_rumble(void *m);
uint8_t *gba_state_save(const void *m, size_t *len);
void gba_state_free(uint8_t *p, size_t len);
int gba_state_load(void *m, const uint8_t *data, size_t len);
void *gba_link_new(void *const *ms, size_t n);  // link cable: the machines run in lockstep, ms[0] is the master
void gba_link_frame(void *l, const uint16_t *keys);
const uint16_t *gba_link_fb(const void *l, size_t i);
void gba_link_free(void *l);  // unlinks; the machines stay

typedef struct { void *data, *metadata, *reserved0, *reserved1; } VideoBuffer;
typedef struct { uint8_t reserved[80]; } VideoAttribute;
int sceVideoOutOpen(int user_id, int bus_type, int index, const void *param);
int sceVideoOutSetFlipRate(int handle, int rate);
void sceVideoOutSetBufferAttribute2(VideoAttribute *attr, uint64_t pixel_format, uint32_t tiling_mode, uint32_t width,
                                    uint32_t height, uint64_t option, uint32_t dcc_control, uint64_t dcc_clear_color);
int sceVideoOutRegisterBuffers2(int handle, int set_index, int buffer_index_start, VideoBuffer *buffers, int buffer_count,
                                VideoAttribute *attr, int category, void *option);
int sceVideoOutSubmitFlip(int handle, int buffer_index, uint32_t flip_mode, int64_t flip_arg);
int sceVideoOutIsFlipPending(int handle);
int sceVideoOutWaitVblank(int handle);
int sceKernelAllocateMainDirectMemory(size_t len, size_t align, int type, int64_t *phys);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t phys, size_t align);
int sceKernelUsleep(uint32_t microseconds);
int sceSystemServiceHideSplashScreen(void);
int sceSystemServiceLoadExec(const char *path, const char *const *argv);
int sceSystemServiceParamGetInt(int param_id, int *value);
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *user_id);
int sceUserServiceGetLoginUserIdList(int32_t ids[4]);  // -1 = empty
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t port_type, int32_t index, const void *params);
int scePadClose(int32_t handle);
int scePadRead(int32_t handle, void *samples, int32_t capacity);
int scePadSetMotionSensorState(int32_t handle, int enable);
int scePadSetVibrationMode(int32_t handle, int32_t mode);
int scePadSetVibration(int32_t handle, const uint8_t motors[2]);
int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t grain, uint32_t rate, uint32_t format);
int sceAudioOutOutput(int32_t port, const void *samples);

#define ROM_PATH "/app0/game.gba"
#define BIOS_PATH "/app0/bios.bin"  // optional: the player's own GBA BIOS (16 KB), for the boot intro
#define LOG_PATH "/app0/log.txt"
#define DEFAULTS_PATH "/app0/defaults.txt"  // the options gb5.py build chose; settings.txt wins
#define CHEATS_PATH "/app0/cheats.txt"   // gb5.py writes it
#define CHEATS_SRC_PATH "/app0/cheats_src.txt" // the libretro .cht name gb5.py matched: the menu can refresh it
// Each PS5 user has their own folder: game.sav, state1-3.bin (slots), settings.txt (the extra menu's
// choices, menu.c), cheats_on.txt and the RetroAchievements queue. The rest of the title is shared.
#define USER_DIR "/app0/user"
static char udir[48], sav_path[80], set_path[80], cheats_on_path[80];

static FILE *logfile;
void say(const char *fmt, ...) {  // also used by ra.c
  if (!logfile) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(logfile, fmt, ap);
  va_end(ap);
  fputc('\n', logfile);
  fflush(logfile);
}

// The CRT calls this after main returns: exit() would show as a crash, so ask the shell to close us.
void catchReturnFromMain(int status) {
  say("exit %d", status);
  sceSystemServiceLoadExec("exit", NULL);
  for (;;) sceKernelUsleep(100000);
}

// ---------------- video: the 240x160 picture scaled into 1920x1080 as the menu's DISPLAY says ----------------
#define W 1920
#define H 1080
#define VBUFS 3
#define VFRAME 0x1000000u
#define VBLOCKS_X (W / 128)
#define VBLOCKS_Y ((H + 127) / 128)

static int vout = -1, vlast;
static int64_t vflips = 1;
static uint32_t *vframes[VBUFS];
static uint16_t untile[128 * 128];  // memory offset in a 64 KiB tile -> (y << 7 | x)
static int16_t srcx[W], srcy[VBLOCKS_Y * 128];  // screen -> GBA pixel, -1 = border
static uint8_t edgex[W], edgey[VBLOCKS_Y * 128];  // last screen column / row of a GBA pixel: the LCD grid
static uint32_t color[32768], color_dim[32768];   // BGR555 -> VideoOut bytes R,G,B,A (dim: on the grid)
static struct settings st;                       // the extra menu's choices
static int lang_es;                               // console in Spanish
static int card_x, sun_x;                         // achievement card's / sun meter's left edge
static int sun_frames;                            // the sun meter shows while > 0
static int fbw = 240, fbh = 160;                  // picture size: 480x160 = 2 players side by side, 480x320 = 3-4 in a 2x2 grid

static uint32_t rgba(double r, double g, double b) {
  uint32_t R = r > 255 ? 255 : (uint32_t)r, G = g > 255 ? 255 : (uint32_t)g, B = b > 255 ? 255 : (uint32_t)b;
  return 0xff000000u | B << 16 | G << 8 | R;
}

// Picture size (x6 1440x960 / fill height 1620x1080 / all of 16:9) and colours, from st.
static void layout(void) {
  int pw = st.size == SIZE_PIXEL ? 1440 : st.size == SIZE_FILL ? 1620 : W, ph = st.size == SIZE_PIXEL ? 960 : H;
  if (fbh == 320) pw = 1440, ph = 960;  // 3-4 PLAYERS: four 720x480 pictures (x3)
  else if (fbw == 480) pw = W, ph = 640;  // 2 PLAYERS: two 960x640 pictures (x4)
  int ox = (W - pw) / 2, oy = (H - ph) / 2, rows = VBLOCKS_Y * 128;
  for (int x = 0; x < W; x++) srcx[x] = x >= ox && x < ox + pw ? (x - ox) * fbw / pw : -1;
  for (int y = 0; y < rows; y++) srcy[y] = y >= oy && y < oy + ph ? (y - oy) * fbh / ph : -1;
  for (int x = 0; x < W; x++) edgex[x] = srcx[x] >= 0 && (x + 1 == W || srcx[x + 1] != srcx[x]);
  for (int y = 0; y < rows; y++) edgey[y] = srcy[y] >= 0 && (y + 1 == rows || srcy[y + 1] != srcy[y]);
  card_x = W - CARD_W - 48;  // the toast: top right over the picture, where the PS5 puts its own
  sun_x = ox >= SUN_W + 20 ? (ox - SUN_W) / 2 : 24;  // the meter: the left border, else over the picture
  for (int c = 0; c < 32768; c++) {
    int r = c & 31, g = c >> 5 & 31, b = c >> 10 & 31;
    double R, G, B;
    if (st.color) {  // the GBA LCD: dark gamma and bleeding primaries (higan's model)
      double lr = pow(r / 31.0, 4.0), lg = pow(g / 31.0, 4.0), lb = pow(b / 31.0, 4.0), k = 255.0 * 255.0 / 280.0;
      R = pow((50 * lg + 255 * lr) / 255, 1 / 2.2) * k;
      G = pow((30 * lb + 230 * lg + 10 * lr) / 255, 1 / 2.2) * k;
      B = pow((220 * lb + 10 * lg + 50 * lr) / 255, 1 / 2.2) * k;
    } else {
      R = r << 3 | r >> 2, G = g << 3 | g >> 2, B = b << 3 | b >> 2;
    }
    color[c] = rgba(R, G, B);
    color_dim[c] = rgba(R * 0.7, G * 0.7, B * 0.7);
  }
}

static int video_init(void) {
  int64_t phys = 0;
  void *mem = NULL;
  VideoBuffer bufs[VBUFS];
  VideoAttribute attr;
  int rc;
  for (uint32_t y = 0; y < 128; y++) {
    uint32_t row = ((y << 4) & 0x70u) ^ ((y << 5) & 0xf00u) ^ ((y << 9) & 0x1000u) ^ ((y << 8) & 0x4000u);
    for (uint32_t x = 0; x < 128; x++) {
      uint32_t col = ((x << 2) & 0xcu) ^ ((x << 5) & 0x380u) ^ ((x << 4) & 0x400u) ^ ((x << 6) & 0x800u) ^ ((x << 9) & 0xa000u);
      untile[(row ^ col) >> 2] = (uint16_t)(y << 7 | x);
    }
  }
  layout();
  for (int i = 0; (vout = sceVideoOutOpen(0xff, 0, 0, NULL)) < 0 && i < 80; i++) sceKernelUsleep(100000);
  if (vout < 0) return say("sceVideoOutOpen 0x%x", vout), -1;
  if ((rc = sceKernelAllocateMainDirectMemory(VFRAME * VBUFS, 0x200000, 3, &phys))) return say("AllocateMainDirectMemory 0x%x", rc), -1;
  if ((rc = sceKernelMapDirectMemory(&mem, VFRAME * VBUFS, 0x33, 0, phys, 0x200000))) return say("MapDirectMemory 0x%x", rc), -1;
  memset(mem, 0, VFRAME * VBUFS);
  for (int i = 0; i < VBUFS; i++) {
    vframes[i] = (uint32_t *)((uint8_t *)mem + (size_t)i * VFRAME);
    bufs[i] = (VideoBuffer){vframes[i], NULL, NULL, NULL};
  }
  memset(&attr, 0, sizeof attr);
  sceVideoOutSetFlipRate(vout, 0);
  sceVideoOutSetBufferAttribute2(&attr, 0x8000000022000000ull, 0, W, H, 0, 0, 0);  // RGBA8, tiled
  if ((rc = sceVideoOutRegisterBuffers2(vout, 0, 0, bufs, VBUFS, &attr, 0, NULL))) return say("RegisterBuffers2 0x%x", rc), -1;
  sceVideoOutSubmitFlip(vout, 0, 1, vflips++);
  return 0;
}

// ponytail: one thread writes 1080p in ~2 ms; split it across threads if a profile says so
#define CARD_Y 48  // the achievement toast's top; card_x comes from layout()

// c over px, alpha in c's top byte (the toast and the indicators are translucent)
static inline uint32_t blend(uint32_t px, uint32_t c) {
  uint32_t a = c >> 24;
  if (a == 255) return c;
  if (!a) return px;
  return 0xff000000u | ((((c >> 16 & 255) * a + (px >> 16 & 255) * (255 - a)) / 255) << 16) |
         ((((c >> 8 & 255) * a + (px >> 8 & 255) * (255 - a)) / 255) << 8) | (((c & 255) * a + (px & 255) * (255 - a)) / 255);
}

static void present(const uint16_t *fb) {
  const int target = (vlast + 1) % VBUFS;
  const uint32_t *card = card_frame(), *sun = sun_frames > 0 ? card_sun(st.sun, lang_es) : NULL;
  int hud_h = 0;
  const uint32_t *hud = ra_hud(&hud_h);  // challenges, leaderboard trackers, progress: bottom right
  const int hud_y = H - CARD_Y - hud_h;
  if (sun_frames > 0) sun_frames--;
  for (int b = 0; b < VBLOCKS_X * VBLOCKS_Y; b++) {
    int by = b / VBLOCKS_X * 128, bx = b % VBLOCKS_X * 128;
    uint32_t *out = vframes[target] + (size_t)b * 128 * 128;
    for (int i = 0; i < 128 * 128; i++) {  // write-combined memory: in order, never scattered
      int v = untile[i], y = by + (v >> 7), x = bx + (v & 127), sy = srcy[y], sx = srcx[x];
      uint32_t px = sy >= 0 && sx >= 0 ? (st.lcd && (edgex[x] | edgey[y]) ? color_dim : color)[fb[sy * fbw + sx] & 0x7fff] : 0xff000000u;
      if (sun && x >= sun_x && x < sun_x + SUN_W && y >= CARD_Y && y < CARD_Y + SUN_H) px = sun[(y - CARD_Y) * SUN_W + x - sun_x];
      if (card && x >= card_x && x < card_x + CARD_W && y >= CARD_Y && y < CARD_Y + CARD_H) px = blend(px, card[(y - CARD_Y) * CARD_W + x - card_x]);
      if (hud && x >= card_x && x < card_x + CARD_W && y >= hud_y && y < hud_y + hud_h) px = blend(px, hud[(y - hud_y) * CARD_W + x - card_x]);
      out[i] = px;
    }
  }
  __asm__ volatile("sfence" ::: "memory");
  for (int spins = 0; sceVideoOutIsFlipPending(vout) > 0 && spins < 4; spins++) sceVideoOutWaitVblank(vout);
  sceVideoOutSubmitFlip(vout, target, 1, vflips++);
  vlast = target;
}

// ---------------- audio: 65536 Hz from the GBA -> 48 kHz grains; the ring paces the emulation ----------------
#define GRAIN 256
#define RING 8192  // stereo frames, power of two
#define AHEAD 2400 // ~50 ms queued before the game waits: audio is the clock, so it never drifts
static int16_t ring[RING * 2];
static atomic_uint ring_r, ring_w;
static int32_t aport = -1;

static void *audio_main(void *arg) {
  (void)arg;
  static int16_t grain[GRAIN * 2] __attribute__((aligned(64)));
  for (;;) {
    unsigned r = atomic_load(&ring_r), avail = atomic_load(&ring_w) - r;
    for (unsigned i = 0; i < GRAIN; i++) {
      int have = i < avail;  // underrun: silence
      grain[i * 2] = have ? ring[(r + i) % RING * 2] : 0;
      grain[i * 2 + 1] = have ? ring[(r + i) % RING * 2 + 1] : 0;
    }
    atomic_store(&ring_r, r + (avail < GRAIN ? avail : GRAIN));
    if (sceAudioOutOutput(aport, grain) < 0) sceKernelUsleep(5000);  // blocks one grain
  }
  return NULL;
}

static void audio_init(void) {
  pthread_t t;
  int rc = sceAudioOutInit();
  if (rc < 0 && rc != (int)0x8026000e) { say("sceAudioOutInit 0x%x", rc); return; }
  if ((aport = sceAudioOutOpen(0xff, 0, 0, GRAIN, 48000, 1)) < 0) { say("sceAudioOutOpen 0x%x", aport); aport = -1; return; }
  pthread_create(&t, NULL, audio_main, NULL);
}

// Linear resample of what the frame produced; the fractional position carries over.
static void audio_push(void *m) {
  static int16_t in[4096 * 2];
  static int16_t last[2];
  static double pos;  // in input frames, relative to `last` at index -1
  size_t n = gba_audio(m, in, sizeof in / sizeof *in) / 2;
  if (aport < 0 || !n) return;
  const double step = 65536.0 / 48000.0;
  unsigned w = atomic_load(&ring_w);
  for (; pos < n - 1; pos += step) {
    int i = (int)pos;  // i may be -1: interpolate from the previous frame's last sample
    double f = pos - i;
    for (int c = 0; c < 2; c++) {
      double a = i < 0 ? last[c] : in[i * 2 + c], b = in[(i + 1) * 2 + c];
      ring[w % RING * 2 + c] = (int16_t)(a + (b - a) * f);
    }
    if (w - atomic_load(&ring_r) < RING - 1) w++;  // full: drop
  }
  atomic_store(&ring_w, w);
  last[0] = in[(n - 1) * 2];
  last[1] = in[(n - 1) * 2 + 1];
  pos -= n;
}

// ---------------- pad ----------------
struct pad_sample {  // 120 bytes as the console writes it
  uint32_t buttons;
  uint8_t left_x, left_y, right_x, right_y, l2, r2;
  uint8_t pad_[2];
  float orientation[4];       // quaternion
  float accel[3];             // G, controller axes
  float angular_velocity[3];  // rad/s
  uint8_t touches, touch_res[3];
  uint32_t touch_res2;
  struct { uint16_t x, y; uint8_t id, res[3]; } touch[2];  // 0..1919 across
  int32_t connected;
  uint64_t timestamp_us;
  uint8_t extension[16];
  uint8_t connected_count;
  uint8_t remaining[15];
};
_Static_assert(sizeof(struct pad_sample) == 120, "pad samples are 120 bytes");
enum { PAD_OPTIONS = 0x8, PAD_UP = 0x10, PAD_RIGHT = 0x20, PAD_DOWN = 0x40, PAD_LEFT = 0x80, PAD_L1 = 0x400, PAD_R1 = 0x800,
       PAD_TRIANGLE = 0x1000, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000, PAD_SQUARE = 0x8000, PAD_TOUCH_PAD = 0x100000,
       PAD_INTERCEPTED = 0x80000000u };
enum { PAD_L2 = 0x100, PAD_R2 = 0x200, PAD_L3 = 0x2 };  // also from the analog triggers below
enum { GBA_A = 1, GBA_B = 2, GBA_SELECT = 4, GBA_START = 8, GBA_RIGHT = 16, GBA_LEFT = 32, GBA_UP = 64, GBA_DOWN = 128, GBA_R = 256, GBA_L = 512 };
static int pad = -1;

static struct pad_sample last_pad;

// This frame's DualSense buttons (0 while the shell has the pad). L2/R2 count from half-way down.
static uint32_t pad_read(int h, struct pad_sample *ls) {
  static struct pad_sample s[8];
  int n = h < 0 ? 0 : scePadRead(h, s, 8);
  if (n > 0) *ls = s[n - 1];
  if (ls->buttons & PAD_INTERCEPTED) return 0;
  return ls->buttons | (ls->l2 > 127 ? PAD_L2 : 0) | (ls->r2 > 127 ? PAD_R2 : 0);
}

// GBA keys, 1 = pressed. Nintendo layout by position: A right (circle), B bottom (cross).
static uint16_t gba_keys(uint32_t b, const struct pad_sample *ls) {
  uint16_t keys = 0;
  uint32_t a = st.swap_ab ? PAD_CROSS : PAD_CIRCLE, bb = st.swap_ab ? PAD_CIRCLE : PAD_CROSS;  // OPTIONS > A BUTTON
  if (b & (a | PAD_TRIANGLE)) keys |= GBA_A;
  if (b & (bb | PAD_SQUARE)) keys |= GBA_B;
  if (b & PAD_TOUCH_PAD) keys |= GBA_SELECT;
  if (b & PAD_OPTIONS) keys |= GBA_START;
  if (b & PAD_L1) keys |= GBA_L;
  if (b & PAD_R1) keys |= GBA_R;
  if (!b && ls->buttons & PAD_INTERCEPTED) return 0;
  if (b & PAD_RIGHT || ls->left_x > 192) keys |= GBA_RIGHT;
  if (b & PAD_LEFT || ls->left_x < 64) keys |= GBA_LEFT;
  if (b & PAD_UP || ls->left_y < 64) keys |= GBA_UP;
  if (b & PAD_DOWN || ls->left_y > 192) keys |= GBA_DOWN;
  return keys;
}

// ---------------- files ----------------
static uint8_t *slurp(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
  if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
  fclose(f);
  *len = d ? (size_t)n : 0;
  return d;
}

// Write next to the old save, then rename: a crash mid-write never loses the last good one.
static void save_flush(void *m, const char *sav_path, uint32_t *last_hash) {
  size_t len;
  const uint8_t *s = gba_save(m, &len);
  if (!len) return;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) h = (h ^ s[i]) * 16777619u;
  if (h == *last_hash) return;
  char tmp[88];
  snprintf(tmp, sizeof tmp, "%s.tmp", sav_path);
  FILE *f = fopen(tmp, "wb");
  if (!f) { say("save: cannot write"); return; }
  int ok = fwrite(s, 1, len, f) == len;
  ok &= fclose(f) == 0;
  if (ok && rename(tmp, sav_path) == 0) {
    chmod(sav_path, 0666);  // FTP (another process) must be able to read it
    *last_hash = h;
  } else {
    say("save: write failed");
  }
}

// ---------------- cartridge sensors from the DualSense ----------------
// Scales after mGBA's SDL frontend: gyro 1 rad/s = 52 units around 0x700; tilt 1 m/s^2 = 8 units
// around 0x3A0. OPTIONS > SENS scales them by level / 5 (1 = a fifth, 10 = double).
#define GYRO_UNITS 52.0f
#define TILT_UNITS (8.0f * 9.81f)  // the DualSense reports G
static float tilt_cx, tilt_cy;      // the controller held level (CENTRAR INCLINACION)
static int tilt_centred;

static void recenter(void) {
  tilt_cx = last_pad.accel[0];
  tilt_cy = last_pad.accel[1];
  tilt_centred = 1;
  say("tilt: level at %.2f %.2f", tilt_cx, tilt_cy);
}

static void sensors(void *m, unsigned hw) {
  static const int lux[11] = {0, 5, 11, 18, 27, 42, 62, 84, 109, 139, 183};  // mGBA's solar levels
  static int last_touch = -1;
  if (hw & HW_LIGHT && last_pad.touches && !(last_pad.buttons & PAD_INTERCEPTED)) {
    int level = last_pad.touch[0].x * 11 / 1920;  // finger across the touchpad: dark .. full sun
    level = level < 0 ? 0 : level > 10 ? 10 : level;
    if (level != last_touch || st.sun != level) sun_frames = 120;  // show the meter for 2 s
    if (st.sun != level) { st.sun = level; settings_save(set_path, &st); }
    last_touch = level;
  } else if (!last_pad.touches) {
    last_touch = -1;
  }
  if (hw & HW_TILT && !tilt_centred) recenter();  // first frame: however the player holds it is level
  int axis = st.gyro / 2, sign = st.gyro % 2 ? -1 : 1;
  float gk = st.gyro_sens / 5.0f, tk = st.tilt_sens / 5.0f;
  int gyro = (int)(sign * last_pad.angular_velocity[axis] * GYRO_UNITS * gk);
  int tx = (int)((last_pad.accel[0] - tilt_cx) * TILT_UNITS * tk), ty = (int)((last_pad.accel[1] - tilt_cy) * TILT_UNITS * tk);
  gba_hw_input(m, gyro, tx, ty, (uint8_t)lux[st.sun]);
}

static void rumble(int duty) {
  static int last = -1;
  if (pad < 0 || duty == last) return;
  const uint8_t motors[2] = {(uint8_t)duty, (uint8_t)(duty / 2)};  // big motor, a little of the small one
  scePadSetVibration(pad, motors);
  last = duty;
}

// ---------------- cheats from the internet: RA Hub fetches libretro-database's file over HTTPS ----------------
char *ra_hub_get(const char *path, size_t *len);  // ra.c

static void update_cheats(const char *src, int es) {
  char path[512], msg[48];
  size_t o = (size_t)snprintf(path, sizeof path, "/cheats?f=");
  for (const unsigned char *c = (const unsigned char *)src; *c && o + 4 < sizeof path; c++)  // URL-encode the name
    o += (size_t)((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || strchr("-_.~", *c)
                      ? snprintf(path + o, sizeof path - o, "%c", *c) : snprintf(path + o, sizeof path - o, "%%%02X", *c));
  size_t len;
  char *cht = ra_hub_get(path, &len);
  int n = cht ? cheats_update(cht, len) : -1;
  free(cht);
  if (!cht) snprintf(msg, sizeof msg, "%s", es ? "SIN CONEXION CON RA HUB" : "NO CONNECTION TO RA HUB");
  else if (n < 0) snprintf(msg, sizeof msg, "%s", es ? "NO SE PUDO ACTUALIZAR" : "COULD NOT UPDATE");
  else snprintf(msg, sizeof msg, es ? "CHEATS ACTUALIZADOS: %d" : "CHEATS UPDATED: %d", n);
  menu_status(msg);
  say("cheats update: %s", msg);
}

// ---------------- save states: "GBAS", machine length + bytes (gba_rt), RA progress length + bytes ----------------
static int states_present(void) {
  int mask = 0;
  char path[64];
  struct stat sb;
  for (int s = 1; s <= 3; s++) {
    snprintf(path, sizeof path, "%s/state%d.bin", udir, s);
    if (stat(path, &sb) == 0 && sb.st_size > 12) mask |= 1 << s;
  }
  return mask;
}

static void state_save(void *m, int slot) {
  char path[64], tmp[72];
  size_t mlen = 0, rlen = ra_state_size();
  uint8_t *mb = gba_state_save(m, &mlen), *rb = rlen ? malloc(rlen) : NULL;
  if (rb && ra_state_save(rb, rlen)) { free(rb); rb = NULL; }
  if (!rb) rlen = 0;
  snprintf(path, sizeof path, "%s/state%d.bin", udir, slot);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = mb ? fopen(tmp, "wb") : NULL;
  uint32_t ml = (uint32_t)mlen, rl = (uint32_t)rlen;
  int ok = f && fwrite("GBAS", 1, 4, f) == 4 && fwrite(&ml, 4, 1, f) == 1 && fwrite(mb, 1, mlen, f) == mlen &&
           fwrite(&rl, 4, 1, f) == 1 && (!rlen || fwrite(rb, 1, rlen, f) == rlen);
  if (f) ok &= fclose(f) == 0;
  if (ok && rename(tmp, path) == 0) chmod(path, 0666);  // as game.sav: never half a state under the real name
  say("state %d: %s (%zu + %zu bytes)", slot, ok ? "saved" : "save FAILED", mlen, rlen);
  gba_state_free(mb, mlen);
  free(rb);
}

static int state_load(void *m, int slot) {
  char path[64];
  size_t len;
  snprintf(path, sizeof path, "%s/state%d.bin", udir, slot);
  uint8_t *d = slurp(path, &len);
  uint32_t ml = 0, rl = 0;
  int ok = d && len >= 12 && !memcmp(d, "GBAS", 4);
  if (ok) memcpy(&ml, d + 4, 4);
  ok = ok && 8 + (size_t)ml + 4 <= len;
  if (ok) memcpy(&rl, d + 8 + ml, 4);
  ok = ok && 12 + (size_t)ml + rl <= len && gba_state_load(m, d + 8, ml) == 0;
  if (ok) ra_state_load(d + 12 + ml, rl);
  say("state %d: %s", slot, ok ? "loaded" : "load FAILED (missing, damaged or from another version)");
  free(d);
  return ok;
}

// The folder of the PS5 user who opened the game. Files still in the title folder (an older GB5, or a .sav
// that gb5.py deploy uploaded) move to whoever opens the game first, and are theirs from then on.
static void user_files(int32_t user) {
  static const char *mine[] = {"game.sav", "settings.txt", "state1.bin", "state2.bin", "state3.bin", "cheats_on.txt", "ra_pending.txt"};
  char from[64], to[80];
  snprintf(udir, sizeof udir, USER_DIR "/%08x", (uint32_t)user);
  mkdir(USER_DIR, 0777);
  mkdir(udir, 0777);
  chmod(USER_DIR, 0777);  // FTP (another process) lists and backs them up
  chmod(udir, 0777);
  for (size_t i = 0; i < sizeof mine / sizeof *mine; i++) {
    snprintf(from, sizeof from, "/app0/%s", mine[i]);
    snprintf(to, sizeof to, "%s/%s", udir, mine[i]);
    if (rename(from, to) == 0) say("%s: now user %08x's", mine[i], (uint32_t)user);
  }
  snprintf(sav_path, sizeof sav_path, "%s/game.sav", udir);
  snprintf(set_path, sizeof set_path, "%s/settings.txt", udir);
  snprintf(cheats_on_path, sizeof cheats_on_path, "%s/cheats_on.txt", udir);
}

// ---------------- PLAYERS 2-4: more GBAs on the link cable, each another PS5 user's controller and save ----------------
#define MAX_PLAYERS 4
static void *ms[MAX_PLAYERS], *link_;  // ms[0] is player 1's machine; link_ is set while np > 1
static int np = 1;                        // players
static int pads[MAX_PLAYERS] = {-1, -1, -1, -1};  // [0] unused: player 1 is `pad`
static int32_t users[MAX_PLAYERS];
static struct pad_sample last_pads[MAX_PLAYERS];
static char savs[MAX_PLAYERS][112];
static uint32_t hash1, hashes[MAX_PLAYERS];  // save_flush: what each game.sav holds
static uint16_t grid[480 * 320];  // the pictures: side by side (2), or 2x2 (3-4, the 4th black with 3)

// New players' choices: this game, then the other GB5 titles installed (/data/homebrew/GBA*/game.gba), by name.
#define HOMEBREW "/data/homebrew"
#define MAX_GAMES 128
static struct game { char id[16], name[40]; } games[MAX_GAMES];
static const char *game_names[MAX_GAMES];
static int game_count;

// "key": "value" in a param.json, copied (UTF-8 made ASCII for the menu font); "" when missing.
static void json_str(const char *path, const char *key, char *out, size_t n) {
  size_t len;
  char *j = (char *)slurp(path, &len), raw[160], *p;
  out[0] = 0;
  if (!j) return;
  j = realloc(j, len + 1);
  j[len] = 0;
  if ((p = strstr(j, key)) && (p = strchr(p + strlen(key), ':')) && (p = strchr(p, '"'))) {
    size_t k = 0;
    for (p++; *p && *p != '"' && k + 1 < sizeof raw; p++) raw[k++] = *p;
    raw[k] = 0;
    text_ascii(out, n, raw);
  }
  free(j);
}

static int by_name(const void *a, const void *b) { return strcmp(((const struct game *)a)->name, ((const struct game *)b)->name); }

static void find_games(int es) {
  char own[16], path[96];
  struct stat sb;
  json_str("/app0/sce_sys/param.json", "\"titleId\"", own, sizeof own);
  snprintf(games[0].name, sizeof games[0].name, "%s", es ? "ESTE JUEGO" : "THIS GAME");
  game_count = 1;
  DIR *d = opendir(HOMEBREW);
  struct dirent *e;
  while (d && (e = readdir(d)) && game_count < MAX_GAMES) {
    if (strncmp(e->d_name, "GBA", 3) || strlen(e->d_name) >= sizeof games[0].id || !strcmp(e->d_name, own)) continue;
    snprintf(path, sizeof path, HOMEBREW "/%s/game.gba", e->d_name);
    if (stat(path, &sb)) continue;
    struct game *g = &games[game_count++];
    snprintf(g->id, sizeof g->id, "%s", e->d_name);
    snprintf(path, sizeof path, HOMEBREW "/%s/sce_sys/param.json", e->d_name);
    json_str(path, "\"titleName\"", g->name, sizeof g->name);
    if (!g->name[0]) snprintf(g->name, sizeof g->name, "%s", g->id);
  }
  if (d) closedir(d);
  else say("players: cannot list " HOMEBREW ": only this game");
  qsort(games + 1, (size_t)game_count - 1, sizeof *games, by_name);
  for (int i = 0; i < game_count; i++) game_names[i] = games[i].name;
  menu_games(game_names, game_count);
  say("players: %d games to choose from", game_count);
}

// The next player is a signed-in PS5 user not playing yet (turning on another DualSense asks the console who
// uses it). They play games[game] (0 = this one) from their own save in that title's folder, written back there.
// NULL when added, else why not (for the menu).
static const char *player_add(int game, const uint8_t *rom, size_t rom_len, const uint8_t *bios, size_t bios_len, int es) {
  int32_t ids[4] = {-1, -1, -1, -1}, u = -1;
  sceUserServiceGetLoginUserIdList(ids);
  for (int i = 0; i < 4 && u == -1; i++) {
    u = ids[i];
    for (int p = 0; p < np; p++)
      if (u == users[p]) u = -1;
  }
  if (u == -1) return es ? "ENCIENDE OTRO MANDO\nY ELIGE OTRO\nUSUARIO" : "TURN ON ANOTHER\nCONTROLLER AND PICK\nANOTHER USER";
  int pad_n = scePadOpen(u, 0, 0, NULL);
  if (pad_n < 0) {
    say("players: scePadOpen 0x%x", pad_n);
    return es ? "NO SE PUDO ABRIR\nEL OTRO MANDO" : "COULD NOT OPEN\nTHE OTHER CONTROLLER";
  }
  char title[48], dir[72], path[96];
  size_t len, rom_n_len = rom_len;
  const uint8_t *rom_n = rom;
  uint8_t *other = NULL;
  snprintf(title, sizeof title, game ? HOMEBREW "/%s" : "/app0", games[game].id);
  if (game) {
    snprintf(path, sizeof path, "%s/game.gba", title);
    if (!(other = slurp(path, &rom_n_len))) {
      scePadClose(pad_n);
      say("players: cannot read %s", path);
      return es ? "NO SE PUDO LEER\nESE JUEGO" : "COULD NOT READ\nTHAT GAME";
    }
    rom_n = other;
  }
  snprintf(dir, sizeof dir, "%s/user", title);  // as user_files() lays it out
  mkdir(dir, 0777);
  chmod(dir, 0777);
  snprintf(dir, sizeof dir, "%s/user/%08x", title, (uint32_t)u);
  mkdir(dir, 0777);
  chmod(dir, 0777);
  // ponytail: a title not opened since per-user saves still has its game.sav at the top: this player starts
  // without it (whoever opens that title first gets it). Open it once alone to sort that out.
  int p = np++;
  snprintf(savs[p], sizeof savs[p], "%s/game.sav", dir);
  ms[p] = bios ? gba_new_bios(rom_n, rom_n_len, bios, bios_len) : gba_new(rom_n, rom_n_len);
  free(other);  // gba_new copied it
  uint8_t *sav = slurp(savs[p], &len);
  if (sav) { gba_load_save(ms[p], sav, len); free(sav); }
  pads[p] = pad_n;
  users[p] = u;
  hashes[p] = 0;
  say("player %d: user %08x, %s, %s", p + 1, (uint32_t)u, game ? games[game].id : "this game", sav ? "save loaded" : "new save");
  return NULL;
}

// Goes to `want` players (1..4): the last ones leave (saves written), new ones join with the games the menu
// picked; stops at the first that can't (its reason returned for the menu). The cable is plugged in again.
static const char *players_set(void *m, int32_t user, int want, const uint8_t *rom, size_t rom_len, const uint8_t *bios, size_t bios_len, int es) {
  const char *why = NULL;
  if (link_) gba_link_free(link_);
  link_ = NULL;
  ms[0] = m;
  users[0] = user;
  while (np > want) {
    np--;
    save_flush(ms[np], savs[np], &hashes[np]);
    gba_free(ms[np]);
    ms[np] = NULL;
    scePadClose(pads[np]);
    pads[np] = -1;
  }
  while (np < want && !(why = player_add(menu_game(np), rom, rom_len, bios, bios_len, es))) {}
  if (np > 1) link_ = gba_link_new(ms, (size_t)np);  // ponytail: a game mid-session sees the cable unplugged and back
  fbw = np > 1 ? 480 : 240;
  fbh = np > 2 ? 320 : 160;
  if (np == 3)  // the 4th quadrant stays black
    for (int y = 160; y < 320; y++) memset(grid + y * 480 + 240, 0, 240 * 2);
  layout();
  menu_players(np);
  say("players: %d", np);
  return why;
}

int main(void) {
  if ((logfile = fopen(LOG_PATH, "w"))) chmod(LOG_PATH, 0666);
  sceSystemServiceHideSplashScreen();
  size_t rom_len, sav_len;
  uint8_t *rom = slurp(ROM_PATH, &rom_len);
  if (!rom) { say("no %s", ROM_PATH); return 1; }
  size_t bios_len;
  uint8_t *bios = slurp(BIOS_PATH, &bios_len);  // rom and bios stay in memory: RESET builds the machine again
  if (bios && bios_len != 16384) { say("bios.bin is %zu bytes, not 16384: ignored", bios_len); free(bios); bios = NULL; }
  void *m = bios ? gba_new_bios(rom, rom_len, bios, bios_len) : gba_new(rom, rom_len);
  say("bios: %s", bios ? "real (bios.bin)" : "built-in (HLE)");
  int32_t user = 0xff;
  sceUserServiceInitialize(NULL);
  sceUserServiceGetInitialUser(&user);  // who opened the game: their saves, options and achievements
  user_files(user);
  settings_defaults(&st);
  settings_read(DEFAULTS_PATH, &st);
  settings_read(set_path, &st);
  ra_lb_popups(st.lb_popups);
  ra_start(m, rom, rom_len, st.hardcore, user, udir);  // async: signs in through RA Hub, notifies when connected
  uint8_t *sav = slurp(sav_path, &sav_len);
  say("cartridge saves to: %s", gba_save_kind(m));
  if (sav) { gba_load_save(m, sav, sav_len); free(sav); say("save loaded: %zu bytes", sav_len); }
  int lang = -1, es = sceSystemServiceParamGetInt(1, &lang) == 0 && (lang == 3 || lang == 20);  // es-ES, es-419
  lang_es = es;
  unsigned hw = gba_hw(m);  // cartridge extras: solar sensor, gyro, rumble, tilt
  cheats_load(CHEATS_PATH, cheats_on_path);
  size_t src_len;
  static char cheat_src[256];  // "Name (USA) (Code Breaker).cht"
  uint8_t *src = slurp(CHEATS_SRC_PATH, &src_len);
  for (size_t k = 0; src && k < src_len && k + 1 < sizeof cheat_src && src[k] != '\n' && src[k] != '\r'; k++) cheat_src[k] = (char)src[k];
  free(src);  // slurp's buffer has no terminator: copy up to the first line break
  menu_cheats_updatable(cheat_src[0] != 0);
  find_games(es);

  if (scePadInit() >= 0) pad = scePadOpen(user, 0, 0, NULL);
  if (pad >= 0) {
    scePadSetMotionSensorState(pad, 1);  // gyro / tilt carts read the controller's motion
    scePadSetVibrationMode(pad, 2);      // two-motor rumble (not the DualSense haptics mode)
  }
  say("pad %d, cartridge extras 0x%x", pad, hw);
  if (video_init()) return 1;
  audio_init();
  say("running %zu-byte ROM", rom_len);

  static uint16_t paused[240 * 160];  // the frame under the menu
  static int16_t scratch[4096 * 2];
  const uint16_t *fb = NULL;
  uint32_t prev_b = 0;
  int rw_mode = 0, rw_sel = 0, rw_tick = 0;
  for (unsigned frame = 1;;) {
    uint32_t b = pad_read(pad, &last_pad), press = b & ~prev_b;
    prev_b = b;
    // ---- rewind: L3 stops on the timeline; right stick / d-pad scrub; GBA A goes back there, GBA B or L3 cancels
    if (!rw_mode && fb && !menu_is_open() && !st.hardcore && np == 1 && press & PAD_L3) {  // no rewind in hardcore or with players linked
      rewind_mark(m, fb);  // "now" is the newest point
      rw_mode = 1;
      rumble(0);
      rw_sel = rw_tick = 0;
      continue;
    }
    if (rw_mode) {
      uint32_t ga = st.swap_ab ? PAD_CROSS : PAD_CIRCLE, gb = st.swap_ab ? PAD_CIRCLE : PAD_CROSS;
      int dx = (int)last_pad.right_x - 128, n = rewind_count();
      if (!n || press & (gb | PAD_L3)) {  // nothing recorded (out of memory?) or cancelled
        rw_mode = 0;
      } else if (press & ga) {
        if (rewind_restore(m, rw_sel) == 0) fb = NULL;  // the framebuffer moved: the next gba_frame gives it
        rw_mode = 0;
      } else {
        // stick: the further over, the faster (every 8, 3 or 1 frames); left = further back
        int every = dx < -110 || dx > 110 ? 1 : dx < -75 || dx > 75 ? 3 : dx < -40 || dx > 40 ? 8 : 0;
        if (every && ++rw_tick % every == 0) rw_sel += dx < 0 ? 1 : -1;
        if (press & PAD_LEFT) rw_sel++;
        if (press & PAD_RIGHT) rw_sel--;
        rw_sel = rw_sel < 0 ? 0 : rw_sel >= n ? n - 1 : rw_sel;
        memcpy(paused, rewind_thumb(rw_sel), sizeof paused);
        rewind_draw(paused, rw_sel, es, st.swap_ab);
        present(paused);
        continue;
      }
      if (!fb) continue;  // restored: run a frame first
    }
    if (fb && !menu_is_open() && (b & (PAD_L2 | PAD_R2)) == (PAD_L2 | PAD_R2)) { menu_open(states_present(), hw); rumble(0); }
    if (menu_is_open()) {  // paused: the game doesn't run, the menu draws over its last frame
      struct settings before = st;
      int act = menu_input(b, &st);
      if (memcmp(&before, &st, sizeof st)) layout();
      if (act != MENU_NONE) settings_save(set_path, &st);
      if (act == MENU_QUIT) { players_set(m, user, 1, rom, rom_len, bios, bios_len, es); break; }
      if (act == MENU_RECENTER) recenter();
      if (act == MENU_UPDATE_CHEATS) update_cheats(cheat_src, es);
      if (act == MENU_SAVE_STATE) state_save(m, menu_slot());
      if (act == MENU_LOAD_STATE && !st.hardcore && state_load(m, menu_slot())) fb = NULL;  // the framebuffer moved: next gba_frame gives it
      if (act == MENU_PLAYERS) {  // the picture changes, so back to the game unless someone couldn't join
        int was = np;
        const char *why = players_set(m, user, menu_players_wanted(), rom, rom_len, bios, bios_len, es);
        if (why && was == 1 && np > 1) {  // some joined: the menu stays over player 1's picture, now in the grid
          for (int y = 0; y < 160; y++) memcpy(grid + y * 480, fb + y * 240, 240 * 2);
          fb = grid;
        }
        if (why) menu_status(why);
        else { menu_close(); fb = NULL; continue; }
      }
      if (act == MENU_HARDCORE) ra_set_hardcore(st.hardcore);
      ra_lb_popups(st.lb_popups);
      if (act == MENU_RESET || (act == MENU_HARDCORE && st.hardcore)) {  // hardcore on: from the start, as RA requires
        players_set(m, user, 1, rom, rom_len, bios, bios_len, es);  // the link holds the old machine
        save_flush(m, sav_path, &hash1);
        gba_free(m);
        m = bios ? gba_new_bios(rom, rom_len, bios, bios_len) : gba_new(rom, rom_len);
        if ((sav = slurp(sav_path, &sav_len))) { gba_load_save(m, sav, sav_len); free(sav); }
        ra_reset(m);
        fb = NULL;
        say("reset");
        continue;
      }
      if (menu_is_open()) {  // over player 1's picture
        for (int y = 0; y < 160; y++) memcpy(paused + y * 240, fb + y * fbw, 240 * 2);
        menu_draw(paused, &st, es);
        if (np > 1)  // player 1 is top left in every layout
          for (int y = 0; y < 160; y++) memcpy(grid + y * 480, paused + y * 240, 240 * 2);
        present(np > 1 ? grid : paused);
        continue;
      }
    }
    while (aport >= 0 && atomic_load(&ring_w) - atomic_load(&ring_r) > AHEAD) sceKernelUsleep(1000);
    struct timespec t0, t1;
    if (np > 1) clock_gettime(CLOCK_MONOTONIC, &t0);
    // TURBO runs several GBA frames per shown frame; only the last one's sound is played, so the
    // audio clock above keeps the shown frames at normal speed and the game runs st.turbo times faster.
    for (int t = 0; t < st.turbo; t++, frame++) {
      if (!st.hardcore) cheats_apply(m, gba_keys(b, &last_pad));  // no cheats in hardcore
      if (hw) sensors(m, hw);  // ponytail: cartridge sensors and rumble are player 1's only
      if (link_) {
        uint16_t keys[MAX_PLAYERS] = {gba_keys(b, &last_pad)};
        for (int p = 1; p < np; p++) keys[p] = gba_keys(pad_read(pads[p], &last_pads[p]), &last_pads[p]);
        gba_link_frame(link_, keys);
        for (int p = 0; p < np; p++)
          for (int y = 0; y < 160; y++) memcpy(grid + (p / 2 * 160 + y) * 480 + p % 2 * 240, gba_link_fb(link_, (size_t)p) + y * 240, 240 * 2);
        fb = grid;
      } else {
        fb = gba_frame(m, gba_keys(b, &last_pad));
        rewind_record(m, fb);
      }
      ra_frame();  // RetroAchievements: player 1
      if (t + 1 < st.turbo) gba_audio(m, scratch, sizeof scratch / sizeof *scratch);
      for (int p = 1; p < np; p++) gba_audio(ms[p], scratch, sizeof scratch / sizeof *scratch);  // sound: player 1's only
      if (frame % 30 == 0) {  // ~0.5 s after the game saves; writes only when it changed
        save_flush(m, sav_path, &hash1);
        for (int p = 1; p < np; p++) save_flush(ms[p], savs[p], &hashes[p]);
      }
    }
    if (np > 1) {  // linked: every machine runs on this thread; log.txt gets the cost about once a second
      static double emu_ms;
      static int emu_n;
      clock_gettime(CLOCK_MONOTONIC, &t1);
      emu_ms += (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
      if (++emu_n == 60) {  // ponytail: one thread for all; a thread per machine if this nears 16.7 ms
        say("players %d, turbo x%d: %.2f ms per frame emulating (of 16.7)", np, st.turbo, emu_ms / 60);
        emu_ms = 0;
        emu_n = 0;
      }
    }
    audio_push(m);
    if (hw & HW_RUMBLE) {  // the share of these frames the cart's motor was on; OPTIONS > RUMBLE can mute it
      int duty = gba_hw_rumble(m);
      rumble(st.rumble ? duty : 0);
    }
    present(fb);
  }
  rumble(0);
  save_flush(m, sav_path, &hash1);  // QUIT from the menu: the CRT then closes the title (catchReturnFromMain)
  return 0;
}
