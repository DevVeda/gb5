# GB5

Turns a Game Boy Advance ROM into a native PS5 title (jailbroken console). Two front ends over one
script: a CLI and a local web UI. Machine-specific facts (console IP, local paths) live in
`CLAUDE.local.md`, which is not in git.

## Hard rules

- **Talk to the user in Spanish.** Code, comments and commit-style text are in English.
- **Never open `.env` files.** The SteamGridDB key now lives in `settings.json`, set from the web UI's
  Settings tab. Never print that key either.
- **Git:** never commit, push, stage, tag or open PRs unless the user asks in that message. Never add
  `Co-Authored-By` or any AI attribution. Read-only git is fine. After edits, list the files to `git add`.
- **Deploy only on "DEPLOY".** No `runtime/build.sh` into `runtime/dist` and no uploads to the console
  until the user says DEPLOY. To check that the runtime compiles, build a scratch copy (see Testing).
- **No personal data in tracked files:** no names, emails, IPs, home paths or private project names.
  Those go in `CLAUDE.local.md`.
- **Button names:** in user-facing text (web, CLI output, README, in-game menu, release notes) write
  ✕ ○ △ □ (or X / O), never Cross/Cruz, Circle/Círculo, Triangle, Square. Internal values stay as words
  (`a_button=cross|circle`, `PAD_CROSS`).
- **Bilingual:** every web UI string has `es` and `en` entries in `T` (web/index.html). The README has an
  English and a Spanish half (`#english`, `#espanol`); change both. The in-game menu has `en`/`sp` arrays.
- **Never break RetroAchievements hardcore compliance.** No change may put GB5's approval at risk
  (https://docs.retroachievements.org/general/hardcore-compliance-requirements.html). Check every feature
  against it before building it. In hardcore, these stay blocked:
  - loading states and rewind
  - slowdown, frame advance
  - cheats, memory editing, input playback
  These rules also hold:
  - turning hardcore on resets the game
  - the User-Agent stays unique and honest
  - achievement list, progress and challenge indicators stay visible
  - leaderboards keep running and submitting (only their on-screen alerts and trackers may be hidden)
  - unlocks made offline are queued
  If a request conflicts with this, say so and propose a compliant version (for example, allow it only in
  casual).
- **Thanks:** every project GB5 uses (code, data, tools, services) gets a line with a link in the README's
  "Thanks" / "Agradecimientos" sections (both halves). Adding a dependency or source means adding it there.
- **Style:** match the surrounding code. Short diffs, stdlib first, no new dependencies.
  `gb5.py` is standard library only. Mark deliberate shortcuts with `ponytail:` comments.

## Layout

```
setup.sh               installs the prerequisites (brew/apt/dnf/pacman); --runtime adds the runtime toolchain
gb5.py                 CLI + web server (stdlib Python). build, deploy, uninstall, update-all, cheats-update, serve
web/index.html         the whole web UI (HTML + CSS + JS, no build step), tabs Create / Settings, EN/ES
assets/logo.svg        logo: README, favicon (served at /logo.svg)
mksys.c + third_party/ icon0.png (512²) and pic0/pic1.dds (3840×2160 BC7, DX10, no mips) from images
runtime/               the PS5 title (one eboot.bin for every game)
  src/lib.rs           gba_rt: C API over gba-core (no_std + alloc, panic=abort)
  ps5/main.c           video (tiled RGBA 1920×1080), audio, DualSense, save states, main loop
  ps5/menu.c|h         Veda Menu (L2+R2, tabs GAME/MULTI/CHEEVOS/CHEATS/OPTIONS), settings.txt, rewind screen
  ps5/ra.c|h           RetroAchievements (rcheevos rc_client) through RA Hub
  ps5/card.c|h         achievement toast + sun meter
  ps5/cheats.c|h       CodeBreaker engine, in-console cheat update
  ps5/rewind.c|h       rewind ring (120 snapshots, 1 per second)
  vendor/gba-core/src/link.rs  link cable (SIO multiplayer) + lockstep of 2-4 machines (gba_link_* in src/lib.rs)
  vendor/              gba-core + armv4t (patched, see vendor/README.md), rcheevos v12.5.0, stb
  overrides.tsv        save type / hardware per game (from mGBA, MPL-2.0)
  build.sh             cargo (host) + Docker image `ps5title` (link + fake-sign)
  dist/                prebuilt eboot.bin + sce_module/libc.prx: COMMITTED, the release packages them
tools/register.c       payload sent to elfldr (9021): registers/unregisters titles with ShadowMountPlus
tools/register.elf     prebuilt, committed
.github/workflows/release.yml  tag v* → PyInstaller one-file builds for linux-x64, macos-arm64, windows-x64
PRIVACY.md             privacy policy (EN/ES), required by RetroAchievements
LICENSE                GPL-3.0 text: all of GB5 is GPL-3.0-or-later (third-party parts keep theirs)
TODO.md                backlog, Spanish, NOT in git
```

Not in git: `out/` (built titles), `ROMS/`, `bios/`, `cheatdb/` (cache), `settings.json`, `.env`,
`runtime/target/`, `runtime/local.env`, `CLAUDE.local.md`.

## How it works

- **Title folder** `out/<TITLEID>/`: `eboot.bin`, `sce_module/libc.prx`, `game.gba`, `defaults.txt`,
  optional `bios.bin`, `game.sav`, `cheats.txt`, `cheats_src.txt`, and `sce_sys/` (`param.json`,
  `icon0.png`, `pic0.dds`, `pic1.dds`, optional `snd0.at9`). On the console it lives in
  `/data/homebrew/<ID>/` (`/app0` at runtime). The runtime writes `log.txt` there, and per PS5 user
  (`sceUserServiceGetInitialUser`, `%08x`) in `user/<id>/`: `game.sav`, `state1-3.bin`, `settings.txt`,
  `cheats_on.txt`, `ra_pending.txt`, `ra_*_<rauser>.json`. Those files found in the title folder itself
  (older GB5, or `--save` via deploy) move to the first user who opens the game (`user_files()` in main.c).
- **Title ID:** `GBA` + region letter + 5 digits, from the game code. Codes with digits use 88000–99999.
  Homebrew without a code: `GBAX` + a number from the ROM.
- **Deploy:** FTP (port 2121) uploads only changed files (sha256 record in `out/<ID>.deployed.json`).
  Changed `sce_sys` art means reinstall. `register.elf` goes to elfldr and reads `/data/gb5/register.txt`,
  then talks to ShadowMountPlus (≥1.7beta9, API on 127.0.0.1:10101 only). The console's FTP answers
  "226" to DELE; that is accepted.
- **Options precedence:** web Settings (`settings.json`) or CLI flags → `defaults.txt` in the title →
  `settings.txt` written by the in-game menu, which always wins.
- **Settings tab:** Veda Menu start options (size, A button, LCD, GBA colours, hardcore, leaderboard alerts, multiplayer sound), SteamGridDB key
  (never sent back to the page, only `sgdb_key_set`), GBA BIOS (saved as `bios/gba_bios.bin`, 16 KB,
  md5 check for the official one).
- **Web build:** "Create and download .zip" builds, then the browser downloads `<name>_<ID>.zip` from
  `/api/zip`. "Create and upload to the PS5" deploys. The server binds 127.0.0.1 only; a packaged app
  falls back to a free port if 8080 is taken.
- **Packaged app** (`FROZEN`): read-only files come from the bundle (`RES`), user files sit next to the
  executable (`ROOT`). Double-click starts `serve` and opens the browser.
- **Cheats:** libretro-database `.cht` (CodeBreaker; types 5 and 9 excluded). One request lists the
  names (`cheatdb/index.json`), then only the matched game's file downloads. Match = title without
  spaces, prefer same region, warn on revision mismatch. All off by default. In game: Veda Menu → Cheats,
  updatable through RA Hub `/cheats`.
- **Saves:** every type (SRAM 32 KB mirrored, Flash 64/128, EEPROM 512/8K, RTC). `.sav` import.
- **Cartridge hardware → DualSense:** gyro, tilt, rumble (scePadSetVibration mode 2), solar sensor on
  the touchpad.
- **Picture sizes:** integer ×6 (1440×960, exact), fill height (×6.75), 16:9 (×8 × ×6.75, stretched).
  Nearest neighbour.

## RetroAchievements

- **Through RA Hub** (separate repo, DevVeda/rahub), a payload on the console:
  - `GET /session?u=<ps5 user %08x>`: that PS5 profile's user + token. RA Hub keeps one session per
    profile (`/data/rahub/session.<id>`); its web follows the foreground user. The old single `session`
    file goes to the first profile that asks.
  - `POST /dorequest`: forwards rcheevos calls over HTTPS, needs the `X-RAHub` header.
  - `/badge?u=`: badge PNG.
  - `/cheats?f=`: a libretro `.cht`.
- **User-Agent:** GB5 sends `GB5/<version> (PS5) gba-core/<version> rcheevos/12.5`, and RA Hub forwards it.
  The GB5 version is the last git tag, the core's comes from `vendor/gba-core/Cargo.toml` (`build.sh` passes
  `GB5_VERSION` and `CORE_VERSION`). Up to v0.2.x requests went out as RA Hub's `RAHub/1.0.0 (PS5)`: declare
  it when applying.
- **Hardcore:**
  - Set in Settings or Veda Menu → Options.
  - Blocks loading states, rewind (L3) and cheats. Saving states and turbo are allowed.
  - Turning it on in game resets the game; turning it off is free.
  - The connect notification says hardcore or casual.
- **RA approval:** until RA approves GB5 ("hardcore-compliant"), its server says "Unknown Emulator"
  and only casual unlocks count. Requests open from late April 2027 (6 months after v0.1.0). Everything
  in the checklist is done in code (TODO.md): hardcore, unique User-Agent with the core, `PRIVACY.md`,
  licences with upstream links, "everything is free", in-game indicators, leaderboards, offline queue.
- **In-game indicators:** `ra_hud()` (ra.c state) → `card_hud()` (card.c pixels) → drawn bottom right in
  `present()`: leaderboard trackers, challenges, measured progress (rcheevos hides it after 2 s).
  Leaderboard started/failed/submitted/rank are console notifications, off with `lbpopups=0` (Settings
  `lb_popups`, `--no-lb-popups`, OPTIONS > LEADERBOARD ALERTS; RA allows it). Leaderboards run in hardcore only.
- **Offline:** `call_main` caches `achievementsets` / `startsession` answers in `ra_<r>_<user>.json` and serves
  them (plus a login2 built from RA Hub's session) when RA doesn't answer. Unanswered calls go to rcheevos as
  retryable (RA Hub's 502 JSON would read as success). Awards and leaderboard entries wait in
  `ra_pending.txt` (`A|L id h|score time user md5(token|…)`) until RA answers; earlier sessions' records are
  replayed from `ra_frame` once loaded and every minute. The token is never written to a file.
- **Host test:** `ra.c` builds on the Mac with `-DAPP0=<dir>` and stubs, against a fake RA Hub on 8045
  (needs `dangerouslyDisableSandbox`; link with `SDKROOT=.../MacOSX26.5.sdk`).

## Commands

```bash
./setup.sh [--runtime]                                # prerequisites, like npm install (re-runnable)
python3 gb5.py serve --port 8080                      # web UI (dev preview: .claude/launch.json "gb5-web", port 8091)
python3 gb5.py build ROM [--sgdb] [--size 16:9|fill|pixel] [--a-button cross|circle] [--[no-]lcd] [--[no-]gba-colors] [--[no-]hardcore] [--save SAV] [--bios BIOS]
python3 gb5.py deploy out/<ID> --host <PS5_IP>
python3 gb5.py update-all --host <PS5_IP>             # latest runtime to every installed title
python3 gb5.py uninstall <ID> --host <PS5_IP> [--delete]
bash runtime/build.sh                                 # runtime → runtime/dist (DEPLOY only)
make -C tools                                         # register.elf
```

DEPLOY means `bash runtime/build.sh`, then `python3 gb5.py update-all --host <IP>`. Then tell the user that
`runtime/dist/*` changed and needs committing.

## Testing

- **Runtime compile check without touching `runtime/dist`:** rsync `runtime/` (minus `dist`) into the
  scratchpad and run `bash build.sh` there. Check for warnings, and `grep -a` the eboot.elf for new strings.
- **gb5.py:** `python3 gb5.py build "ROMS/<game>.zip" --out $(mktemp -d)`, and import gb5 for function
  checks.
- **Web:** restart the `gb5-web` preview (the server does not reload), then check with
  `javascript_tool` and screenshots. Don't press destructive buttons (Remove BIOS) on the user's real data.
- **Linux:** the `ps5title` Docker image has python3 + gcc.

## Mac toolchain quirks

- CLT `ld` can't read the newest SDK: mksys falls back to `SDKROOT=.../MacOSX26.5.sdk`.
- Rust comes from Homebrew rustup (`/opt/homebrew/opt/rustup/bin`), with target `x86_64-unknown-freebsd`.
- `LLVM_CONFIG=/opt/homebrew/opt/llvm@18/bin/llvm-config`. tools/Makefile also finds apt's llvm-18.
- Title constraints: no TLS segment, no unresolved imports or direct syscalls. That is why gba-core is
  no_std and uses `rust_eh_personality` / libm stubs. build.sh checks both.
- zsh: no word splitting of `$VAR`, a failed glob aborts the command. Prefer Python for edits.
- Sandbox: writing outside the repo (RA Hub repo, `~/.claude`) and Docker/network need
  `dangerouslyDisableSandbox`.

## GitHub and releases

- Repos: DevVeda/gb5 and DevVeda/rahub, both public (Actions are free).
- **Release flow:** commit, `git push`, then `git tag vX.Y.Z && git push origin vX.Y.Z`. The workflow
  builds the 3 zips into a draft release, and the user publishes it. Never reuse a tag; for an existing
  one: `git tag -f` + `git push --force origin <tag>`.
- RA Hub has its own `release.yml`: SDK from pacbrew-repo `ps5-payload-dev.tar.gz` v0.41.1
  (sha256-pinned), `make`, `make test`, uploads `rahub.elf` + `rahub-test.elf`.
- Versions: v0.1.0 (first), v0.2.0 (Settings tab). Next is v0.2.1 or v0.3.0 (zip download, hardcore,
  integer naming).
- README: bilingual, logo + Ko-fi button at the top, "AI disclosure" / "Uso de IA" sections, licenses.
  All of GB5 is GPL-3.0-or-later (`LICENSE`, also in the release zips).

## Status

- **Verified on console:**
  - gameplay, saves (EEPROM incl.), rewind, states, BIOS intro
  - RA connection and toasts
  - cheats (incl. update), sensors and sensitivity
  - ✕/○ glyphs, menu cursor memory
  - Boktai, Yoshi Topsy-Turvy, WarioWare Twisted
- **Not yet on console:** hardcore mode and RA Hub User-Agent forwarding, incl. the `gba-core/` segment
  (needs DEPLOY + new RA Hub); in-game indicators, leaderboards and the offline queue (tested on the Mac
  against a fake RA Hub only). Veda Menu → MULTI > PLAYERS 1-4, a PLAYER n row per new one opening the game list, APPLY (link cable; each extra player is another signed-in
  user's pad, any installed GB5 title from /data/homebrew with that user's save in that title; 2 side by side
  ×4, 3-4 in a 2×2 grid ×3; log.txt gets "players N ... ms per frame" once a second). Sound: MULTI > SOUND
  (`mpaudio`, Settings `mp_audio`, `--mp-audio`): TV + pads / TV only / pads only. Each player's controller is a
  PADSPK port (type 4, mono, opened at volume 0 so ours is set to 0 dB); PERSONAL (type 3) opens but never routes.
  Headphones in any controller also take the main port (PS5 setting). The main port always runs: it paces the game:
  verified on console: 2 players (Ruby ↔ Emerald trade), 3 players (Mario Kart Super Circuit, ~8.3 ms/frame avg,
  10.1 max of 16.7, one thread); 4 not yet.
- **Next big task:** the gba-recomp recompilation (ROM code → native x86 instead of gba-core's
  interpreter). Before that, maybe the "code refactor" TODO item.
- **Veda Menu** (runtime/ps5/menu.c): full-screen 240x160, Pokémon OPTION-screen look; header "VEDA MENU", L1 / R1,
  HARDCORE/CASUAL; tabs GAME (resume, save/load 1-3, turbo, reset, quit), MULTI, CHEEVOS (ES LOGROS), CHEATS, OPTIONS.
  Rows come from `rows_of()`, shared by input and drawing. Not yet on console (needs DEPLOY).
- **Deferred:** menu music (no free ATRAC9 encoder), e-Reader, in-game ✕/○ prompts, native
  PS5 app.
