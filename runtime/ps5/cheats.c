// CodeBreaker for the GBA, the codes libretro-database's cheat files are (almost all) written in.
// A code is pairs "TAAAAAAA VVVV": T the type, AAAAAAA the address, VVVV the value. They are applied
// every frame on work RAM. A conditional type runs the code right after it only when true, as the
// device does. gb5.py already dropped cheats with encrypted (9), byte-fill (5) or 8+8 codes.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cheats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

uint16_t gba_peek16(void *m, uint32_t addr);
void gba_poke16(void *m, uint32_t addr, uint16_t v);
void gba_poke8(void *m, uint32_t addr, uint8_t v);
void say(const char *fmt, ...);

#define MAX_CHEATS 256
#define MAX_PAIRS 128

struct cheat {
  char name[64];
  int n, on;
  uint32_t a[MAX_PAIRS];  // type << 28 | address
  uint16_t v[MAX_PAIRS];
};

static struct cheat *list;
static int count;
static char on_file[64], list_file[64];

void cheats_load(const char *path, const char *on_path) {
  snprintf(on_file, sizeof on_file, "%s", on_path);
  snprintf(list_file, sizeof list_file, "%s", path);
  free(list);  // a reload after cheats_update
  list = NULL;
  count = 0;
  FILE *f = fopen(path, "r");
  if (!f) return;
  list = calloc(MAX_CHEATS, sizeof *list);
  char line[4096];
  while (list && count < MAX_CHEATS && fgets(line, sizeof line, f)) {  // "name<TAB>AAAAAAAA+VVVV+..."
    char *tab = strchr(line, '\t');
    if (!tab) continue;
    *tab = 0;
    struct cheat *c = &list[count];
    snprintf(c->name, sizeof c->name, "%s", line);
    for (char *p = strtok(tab + 1, "+\r\n"); p && c->n < MAX_PAIRS; p = strtok(NULL, "+\r\n")) {
      char *v = strtok(NULL, "+\r\n");
      if (!v || strlen(p) != 8 || strlen(v) != 4) { c->n = 0; break; }  // not CodeBreaker: drop the cheat
      c->a[c->n] = (uint32_t)strtoul(p, NULL, 16);
      c->v[c->n++] = (uint16_t)strtoul(v, NULL, 16);
    }
    if (c->n) count++;
  }
  fclose(f);
  if ((f = fopen(on_file, "r"))) {
    for (int i; fscanf(f, "%d", &i) == 1;)
      if (i >= 0 && i < count) list[i].on = 1;
    fclose(f);
  }
  say("cheats: %d", count);
}

// The same filter as gb5.py's parse_cheats: CodeBreaker pairs of 8 + 4 hex digits, no 5 or 9 types.
static int usable(const char *code) {
  int n = 0, len = 0;
  for (const char *p = code;; p++) {
    if (*p == '+' || !*p) {
      if (len != (n % 2 == 0 ? 8 : 4)) return 0;
      n++;
      len = 0;
      if (!*p) break;
    } else if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F') || (*p >= 'a' && *p <= 'f')) {
      if (len == 0 && n % 2 == 0 && (*p == '5' || *p == '9')) return 0;
      len++;
    } else {
      return 0;
    }
  }
  return n >= 2 && n % 2 == 0;
}

int cheats_update(const char *cht, size_t len) {
  enum { MAX = 512 };
  static char desc[MAX][64], code[MAX][1024];
  int top = -1;
  memset(desc, 0, sizeof desc);
  memset(code, 0, sizeof code);
  for (const char *p = cht, *end = cht + len; p < end;) {  // cheatN_desc = "..." / cheatN_code = "..."
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
    char line[1200], key[16], val[1100];
    int i;
    snprintf(line, sizeof line, "%.*s", (int)(ll < sizeof line - 1 ? ll : sizeof line - 1), p);
    if (sscanf(line, "cheat%d_%15[a-z] = \"%1099[^\"]\"", &i, key, val) == 3 && i >= 0 && i < MAX) {
      if (!strcmp(key, "desc")) snprintf(desc[i], sizeof desc[i], "%s", val);
      if (!strcmp(key, "code")) snprintf(code[i], sizeof code[i], "%s", val);
      if (i > top) top = i;
    }
    p += ll + 1;
  }
  char tmp[80];
  snprintf(tmp, sizeof tmp, "%s.tmp", list_file);
  FILE *f = fopen(tmp, "w");
  if (!f) return -1;
  int n = 0;
  for (int i = 0; i <= top; i++)
    if (desc[i][0] && usable(code[i])) {
      for (char *c = desc[i]; *c; c++) if (*c == '\t') *c = ' ';
      fprintf(f, "%s\t%s\n", desc[i], code[i]);
      n++;
    }
  if (fclose(f) || !n) { remove(tmp); return -1; }
  // what was on, by name: the new file can add, drop or reorder cheats
  static char was_on[MAX_CHEATS][64];
  int non = 0;
  for (int i = 0; i < count && non < MAX_CHEATS; i++)
    if (list[i].on) snprintf(was_on[non++], sizeof was_on[0], "%s", list[i].name);
  if (rename(tmp, list_file)) return -1;
  chmod(list_file, 0666);
  remove(on_file);
  cheats_load(list_file, on_file);
  for (int i = 0; i < count; i++)
    for (int k = 0; k < non; k++)
      if (!strcmp(list[i].name, was_on[k]) && !list[i].on) cheats_toggle(i);
  say("cheats: updated, %d usable", count);
  return count;
}

int cheats_count(void) { return count; }
const char *cheats_name(int i) { return list[i].name; }
int cheats_enabled(int i) { return list[i].on; }

void cheats_toggle(int i) {
  list[i].on = !list[i].on;
  FILE *f = fopen(on_file, "w");
  if (!f) return;
  for (int k = 0; k < count; k++)
    if (list[k].on) fprintf(f, "%d\n", k);
  fclose(f);
  chmod(on_file, 0666);
}

// The address field holds a GBA address without its top nibble: 2xxxxxx EWRAM, 3xxxxxx IWRAM.
static void run(void *m, const struct cheat *c, uint16_t keys) {
  for (int i = 0; i < c->n; i++) {
    uint32_t t = c->a[i] >> 28, addr = c->a[i] & 0x0FFFFFFF;
    uint16_t v = c->v[i], cur;
    int cond = -1;  // -1: not a conditional; else whether the next code runs
    switch (t) {
      case 0x0: case 0x1: break;                                    // game id / hook: master code bookkeeping
      case 0x2: gba_poke16(m, addr, gba_peek16(m, addr) | v); break;  // OR
      case 0x3: gba_poke8(m, addr, (uint8_t)v); break;              // write 8 bits
      case 0x4:                                                     // slide: next pair = step value, count, step address
        if (i + 1 < c->n) {
          uint16_t vinc = (uint16_t)(c->a[i + 1] >> 16), n = (uint16_t)c->a[i + 1], ainc = c->v[i + 1];
          for (uint16_t k = 0; k < n; k++, addr += ainc, v += vinc) gba_poke16(m, addr, v);
          i++;
        }
        break;
      case 0x6: gba_poke16(m, addr, gba_peek16(m, addr) & v); break;  // AND
      case 0x7: cond = gba_peek16(m, addr) == v; break;               // if equal
      case 0x8: gba_poke16(m, addr, v); break;                        // write 16 bits
      case 0xA: cond = gba_peek16(m, addr) != v; break;               // if not equal
      case 0xB: cond = gba_peek16(m, addr) > v; break;                // if greater
      case 0xC: cond = gba_peek16(m, addr) < v; break;                // if less
      case 0xD: cond = (keys & v) == v; break;                        // if these buttons are held
      case 0xE: cur = gba_peek16(m, addr); gba_poke16(m, addr, (uint16_t)(cur + v)); break;  // add
      case 0xF: cond = (gba_peek16(m, addr) & v) != 0; break;         // if any bit
      default: return;                                                // 5, 9: GB5 never ships them
    }
    if (cond == 0) i += i + 1 < c->n && c->a[i + 1] >> 28 == 0x4 ? 2 : 1;  // skip the guarded code (a slide is two pairs)
  }
}

void cheats_apply(void *m, uint16_t keys) {
  for (int i = 0; i < count; i++)
    if (list[i].on) run(m, &list[i], keys);
}
