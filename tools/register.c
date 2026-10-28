// register.elf: payload sent to elfldr (port 9021) by `gb5.py deploy`.
// ShadowMountPlus's API only answers on the console itself (127.0.0.1:10101), so this runs there:
// for each line of /data/gb5/register.txt it POSTs manual/add (a /data/homebrew/<ID> folder) or
// games/uninstall (-<TITLEID>), then one scan,
// and writes what happened to /data/gb5/register.log (read back over FTP).
// Build: make -C tools  (ps5-payload-sdk)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DIR "/data/gb5"

static FILE *out;

// One HTTP/1.0 POST; returns the status code, or -1 when nothing listens.
static int post(const char *path, const char *body) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(10101), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
    if (s >= 0) close(s);
    return -1;
  }
  char req[1024], resp[512] = {0};
  int n = snprintf(req, sizeof req,
                   "POST %s HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
                   path, strlen(body), body);
  write(s, req, (size_t)n);
  read(s, resp, sizeof resp - 1);
  close(s);
  int code = -1;
  sscanf(resp, "HTTP/%*s %d", &code);
  fprintf(out, "%s %s -> %d %s\n", path, body, code, strchr(resp, '{') ? strchr(resp, '{') : "");
  return code;
}

int main(void) {
  if (!(out = fopen(DIR "/register.log", "w"))) return 1;
  FILE *in = fopen(DIR "/register.txt", "r");
  char line[256], body[320];
  int ok = 0;
  while (in && fgets(line, sizeof line, in)) {
    line[strcspn(line, "\r\n")] = 0;
    if (strpbrk(line, "\"\\")) continue;  // nothing that could break the JSON
    int code;
    if (line[0] == '-' && strlen(line) == 10) {  // -TITLEID: uninstall
      snprintf(body, sizeof body, "{\"title_id\":\"%s\"}", line + 1);
      code = post("/api/v1/games/uninstall", body);
    } else if (!strncmp(line, "/data/homebrew/", 15)) {  // a title folder: add
      snprintf(body, sizeof body, "{\"path\":\"%s\"}", line);
      code = post("/api/v1/manual/add", body);
    } else {
      continue;
    }
    if (code < 0) {
      fprintf(out, "ShadowMountPlus is not running (nothing on 127.0.0.1:10101)\n");
      break;
    }
    ok += code / 100 == 2;
  }
  if (in) fclose(in);
  if (ok) post("/api/v1/scan", "{}");
  fprintf(out, "done %d\n", ok);
  fclose(out);
  return 0;
}
