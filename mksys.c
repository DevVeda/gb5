// mksys: PS5 home-menu art from any image (PNG/JPG/BMP/TGA/GIF).
//   mksys icon IN OUT.png   512x512 RGB PNG              (sce_sys/icon0.png)
//   mksys pic  IN OUT.dds   3840x2160 BC7 DX10 DDS, 1 mip (sce_sys/pic0.dds, pic1.dds)
// Both centre-crop to the target aspect ("cover"), like the web preview's background-size: cover.
// Plain C + single-file libs so the same code builds later for the native PS5 app.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image.h"
#include "third_party/stb_image_write.h"
#include "third_party/bc7enc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// RGBA resize: each output pixel averages the source area it covers (downscale), or samples
// bilinearly when that area is under a pixel (upscale). stb dropped stb_image_resize2 by 2026.
// ponytail: averages gamma-encoded values; linearise first if dark edges ever show on art
static unsigned char *resize(const unsigned char *src, int w, int h, int stride, int W, int H) {
  unsigned char *out = malloc((size_t)W * H * 4);
  if (!out) { fprintf(stderr, "mksys: out of memory\n"); exit(1); }
  double sx = (double)w / W, sy = (double)h / H;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      unsigned char *o = out + ((size_t)y * W + x) * 4;
      if (sx > 1 && sy > 1) {  // box: every source pixel whose centre falls in the area
        int x0 = (int)(x * sx), x1 = (int)((x + 1) * sx), y0 = (int)(y * sy), y1 = (int)((y + 1) * sy);
        if (x1 <= x0) x1 = x0 + 1;
        if (y1 <= y0) y1 = y0 + 1;
        unsigned long acc[4] = {0}, n = (unsigned long)(x1 - x0) * (y1 - y0);
        for (int j = y0; j < y1; j++)
          for (int i = x0; i < x1; i++)
            for (int c = 0; c < 4; c++) acc[c] += src[(size_t)j * stride + i * 4 + c];
        for (int c = 0; c < 4; c++) o[c] = (unsigned char)((acc[c] + n / 2) / n);
      } else {  // bilinear at the output pixel's centre
        double fx = (x + .5) * sx - .5, fy = (y + .5) * sy - .5;
        if (fx < 0) fx = 0;
        if (fy < 0) fy = 0;
        int x0 = (int)fx, y0 = (int)fy, x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
        double ax = fx - x0, ay = fy - y0;
        for (int c = 0; c < 4; c++) {
          double top = src[(size_t)y0 * stride + x0 * 4 + c] * (1 - ax) + src[(size_t)y0 * stride + x1 * 4 + c] * ax;
          double bot = src[(size_t)y1 * stride + x0 * 4 + c] * (1 - ax) + src[(size_t)y1 * stride + x1 * 4 + c] * ax;
          o[c] = (unsigned char)(top * (1 - ay) + bot * ay + .5);
        }
      }
    }
  return out;
}

// Biggest W:H window, centred, scaled to W x H.
static unsigned char *crop(const unsigned char *in, int w, int h, int W, int H) {
  int cw = w, ch = h;
  if ((long long)w * H > (long long)h * W) cw = (int)((long long)h * W / H); else ch = (int)((long long)w * H / W);
  return resize(in + ((size_t)((h - ch) / 2) * w + (w - cw) / 2) * 4, cw, ch, w * 4, W, H);
}

static unsigned char *cover(const char *path, int W, int H) {
  int w, h, n;
  unsigned char *in = stbi_load(path, &w, &h, &n, 4);
  if (!in) { fprintf(stderr, "mksys: %s: %s\n", path, stbi_failure_reason()); exit(1); }
  // Banners too (SteamGridDB heroes are ~3.1:1): the sides go, the screen stays full. A banner on top
  // of a blurred copy left the lower half of the home screen looking empty on the console.
  unsigned char *out = crop(in, w, h, W, H);
  stbi_image_free(in);
  for (size_t i = 0; i < (size_t)W * H; i++) {  // flatten alpha over black: the shell ignores it
    unsigned a = out[i * 4 + 3];
    for (int c = 0; c < 3; c++) out[i * 4 + c] = (unsigned char)(out[i * 4 + c] * a / 255);
    out[i * 4 + 3] = 255;
  }
  return out;
}

static void put32(FILE *f, uint32_t v) { unsigned char b[4] = {v, v >> 8, v >> 16, v >> 24}; fwrite(b, 1, 4, f); }

static int pic(const char *in, const char *outpath) {
  enum { W = 3840, H = 2160 };
  unsigned char *px = cover(in, W, H);
  FILE *f = fopen(outpath, "wb");
  if (!f) { perror(outpath); return 1; }
  // DDS header, same layout as the boilerplate's validated pic0.dds (148-byte header + W*H bytes).
  fwrite("DDS ", 1, 4, f);
  uint32_t hdr[31] = {0};
  hdr[0] = 124; hdr[1] = 0xA1007; hdr[2] = H; hdr[3] = W; hdr[4] = W * H; hdr[6] = 1;  // size, flags, h, w, linear size, mips
  hdr[18] = 32; hdr[19] = 4; memcpy(&hdr[20], "DX10", 4);                               // pixel format: FourCC DX10
  hdr[26] = 0x1000;                                                                     // caps: texture
  for (int i = 0; i < 31; i++) put32(f, hdr[i]);
  put32(f, 98); put32(f, 3); put32(f, 0); put32(f, 1); put32(f, 0);  // DXGI_FORMAT_BC7_UNORM, 2D, 1 element

  bc7enc_compress_block_init();
  bc7enc_compress_block_params p;
  bc7enc_compress_block_params_init(&p);
  unsigned char blk[16 * 4], out[16];
  for (int by = 0; by < H; by += 4)
    for (int bx = 0; bx < W; bx += 4) {
      for (int y = 0; y < 4; y++) memcpy(blk + y * 16, px + ((size_t)(by + y) * W + bx) * 4, 16);
      bc7enc_compress_block(out, blk, &p);
      fwrite(out, 1, 16, f);
    }
  free(px);
  return fclose(f) ? (perror(outpath), 1) : 0;
}

static int icon(const char *in, const char *out) {
  unsigned char *px = cover(in, 512, 512);
  for (int i = 0; i < 512 * 512; i++) memmove(px + i * 3, px + i * 4, 3);  // RGBA -> RGB in place
  int ok = stbi_write_png(out, 512, 512, 3, px, 512 * 3);
  free(px);
  if (!ok) fprintf(stderr, "mksys: cannot write %s\n", out);
  return !ok;
}

int main(int argc, char **argv) {
  if (argc == 4 && !strcmp(argv[1], "icon")) return icon(argv[2], argv[3]);
  if (argc == 4 && !strcmp(argv[1], "pic")) return pic(argv[2], argv[3]);
  fprintf(stderr, "usage: mksys icon|pic IN OUT\n");
  return 2;
}
