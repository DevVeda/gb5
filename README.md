# GB5

<p align="center"><img src="assets/logo.svg" width="128" alt="GB5 logo"></p>

<p align="center"><a href="https://ko-fi.com/T1K329H7HC"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="Support me on Ko-fi"></a></p>

[English version](#english) · [Versión en español](#espanol)

<a id="english"></a>

## English

Turns a Game Boy Advance ROM into a **native PS5 title** (jailbroken console), with its own icon,
background and name on the home screen. The game runs on the core of
[gba-recomp](https://github.com/JRickey/gba-recomp) (`gba-core`), and comes with:

- **RetroAchievements** through [RA Hub](#ra-en): a notice when it connects, a PS5-trophy-style toast
  for each unlock and the achievement list in the menu.
- **GB5 Menu** on **L2 + R2**: save states, achievements, cheats, options, turbo, reset.
- **Rewind** on **L3**: the last 2 minutes, with thumbnails.
- **Cheats** from [libretro-database](https://github.com/libretro/libretro-database) (CodeBreaker),
  updatable from the console itself.
- **Cartridge hardware** on the DualSense: gyro (turn the controller), tilt, rumble and the solar
  sensor (the touchpad).
- **Artwork** from [SteamGridDB](https://www.steamgriddb.com), or your own.
- Optional **real BIOS** (yours): the GBA boot intro.
- Cartridge saves in every format (SRAM, Flash 64/128 KB, EEPROM) and the real-time clock (RTC).

Use it from the terminal (`gb5.py`) or from a local web page that previews the PS5 home screen.

> **Legal:** only use ROMs and BIOS you dumped yourself. None of that goes in this repository.

### Quick start (no compiling)

1. Download the zip for your system from [Releases](../../releases/latest) and unzip it.
2. Open `gb5` (`gb5.exe` on Windows): the web UI opens in your browser. Everything it makes (`out/`, `cheatdb/`) stays next to it.
   - macOS: the first time, right-click → Open (the app isn't signed). If it still refuses:
     `xattr -d com.apple.quarantine gb5`.
   - Windows: SmartScreen may warn; "More info" → "Run anyway".
3. Optional, next to `gb5`: `bios/gba_bios.bin` (your BIOS) and `.env` with `STEAMGRIDDBAPI=<your key>`.

The same file works in a terminal: `./gb5 build ROM --sgdb`, `./gb5 deploy out/<ID> --host <PS5 IP>`…
The requirements below are only for building GB5 yourself.

### Requirements

**Mac, Linux or Windows with WSL 2** (Ubuntu commands in brackets):

| What | For |
|---|---|
| Python 3.10+ | `gb5.py` (standard library only) |
| A C compiler: Xcode Command Line Tools (`xcode-select --install`) [`sudo apt install build-essential`] | building `mksys` (icon and backgrounds) |
| Rust (`brew install rustup` [[rustup.rs](https://rustup.rs)], then `rustup default stable` and `rustup target add x86_64-unknown-freebsd`) | the core |
| Docker with the `ps5title` image (Arch amd64 + clang 18); on WSL, Docker Desktop with WSL integration | linking and signing `eboot.bin` |
| [PS5_Vulkan + PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_Vulkan), built (default `~/ps5gpu`) | the title's CRT, platform layer, `ps5-native-tool` and `libc.prx` |
| [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) + LLVM 18 (`brew install llvm@18` [`sudo apt install llvm-18 clang-18 lld-18`]) | the payload that registers titles |
| `.env` with `STEAMGRIDDBAPI=<your key>` | artwork search on SteamGridDB (optional) |

**WSL:** keep the project inside WSL (`~/gb5`, not `/mnt/c/...`: much faster), and open the web UI
from Windows at `http://localhost:8080`.

**PS5**, jailbroken: kstuff, **ShadowMountPlus 1.7beta9 or newer** (older versions only accept
`PPSA`/`CUSA`/`FAKE`/`LAPY` IDs), elfldr (port 9021) and an FTP server (port 2121).
Optional: [RA Hub](https://github.com/DevVeda/rahub) for achievements ([how it's used](#ra-en)).

### Setup

```bash
# image libraries (public domain / MIT)
mkdir -p third_party && cd third_party
curl -fLO https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
curl -fLO https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h
curl -fLO https://raw.githubusercontent.com/richgel999/bc7enc/master/bc7enc.c
curl -fLO https://raw.githubusercontent.com/richgel999/bc7enc/master/bc7enc.h
cd ..

runtime/build.sh                  # eboot.bin (runtime/dist/), once and after every runtime change
make -C tools                     # register.elf
```

`runtime/build.sh` reads `PS5GPU` (where PS5_Vulkan and PS5_VulkanTemplate live) and `IMAGE` (the Docker
image) when the defaults don't fit; also from `runtime/local.env` (local, not versioned), e.g.
`PS5GPU=/path/to/ps5gpu`.

Optional: your BIOS at `bios/gba_bios.bin` (16 KB). Every game gets it.

### Usage

#### Terminal

```bash
# build: name, icon and background from SteamGridDB, cheats, the BIOS if it's in bios/
python3 gb5.py build "roms/Pokemon - Emerald Version (USA, Europe).zip" --sgdb

# upload to the PS5 and register it on the home screen (only what changed goes up)
python3 gb5.py deploy out/GBAE05350 --host YOUR-PS5-IP

# remove it from the home screen (--delete also deletes its folder, ROM and save included)
python3 gb5.py uninstall GBAE05350 --host YOUR-PS5-IP --delete

# after a runtime change (runtime/build.sh): bring it to every game installed on that PS5
python3 gb5.py update-all --host YOUR-PS5-IP
```

`update-all` only touches the games in `out/` that are installed on the console, uploads only what
changed and skips (and reports) the ones that are open.

`build` options:

| Option | |
|---|---|
| `--sgdb` | name, icon and background from SteamGridDB (whatever you pass by hand wins) |
| `--name`, `--icon`, `--hero`, `--launch` | your own name and images (any format; centre-cropped) |
| `--music` | a ready `snd0.at9` (48 kHz, 2 MiB max; there's no free ATRAC9 encoder) |
| `--size pixel\|fill\|16:9` | picture size to start with (default `16:9`) |
| `--a-button cross\|circle` | which button is the GBA's A (default `cross`) |
| `--lcd`, `--gba-colors` | LCD grid / GBA screen colours (default off) |
| `--save` | import a `.sav` (mGBA, VBA, cartridge). **On deploy it replaces the save on the console** |
| `--bios` | your BIOS; `--bios none` to leave it out |
| `--title-id` | force the ID |

Takes `.gba` and `.zip` (as No-Intro sets come). While building it tells you the game's save type, the
cheats it gets and the extra hardware it uses.

The `--size`, `--a-button`, `--lcd` and `--gba-colors` options are only the starting point
(`defaults.txt` in the title). Whatever you change in the game (GB5 Menu → Options) goes to
`settings.txt` and always wins, even if you build and deploy again.

#### Web

```bash
python3 gb5.py serve   # http://127.0.0.1:8080
```

Pick the ROM, artwork (yours or SteamGridDB's), music, BIOS, save and starting options; see how it will
look on the PS5 home screen and which cheats it gets; then build or upload to the console. The
**Update all games on the PS5** button does what `update-all` does.

#### The title ID

A PS5 wants 4 letters + 5 digits. It's `GBA` + the region letter of the cartridge's game code, with the
rest of the code and the revision in the digits: Pokémon Emerald USA (`BPEE`) is `GBAE05350`. It's
unique per game and version, so building again replaces the same icon. Codes with digits (Boktai
`U3IE`) go to the 88000–99999 range; no code (homebrew): `GBAX` + a number taken from the ROM.

### In the game

| Controller | GBA |
|---|---|
| Cross / Triangle | A |
| Circle / Square | B |
| L1 / R1 | L / R |
| Options | Start |
| Touchpad (click) | Select |
| D-pad / left stick | D-pad |

**Options** can switch it to A = Circle, B = Cross (Nintendo's positions).

**L2 + R2** opens the **GB5 Menu** (the game pauses; reopened, the cursor is where you left it):

- **Save / Load state** 1–3 (left/right changes the slot)
- **Achievements**: the RetroAchievements list with progress
- **Cheats**: turn them on and off one by one (all start off). **Update from the internet** fetches the
  game's latest file from libretro-database through RA Hub, without the computer, and keeps the ones you had on.
- **Options**: size (16:9, fill height, pixel perfect ×6), LCD grid, GBA colours, A button ✕/○, and
  only in the games that have them: rumble on/off, sunlight, gyro axis and sensitivity, tilt sensitivity
  and levelling
- **Turbo** ×1 / ×2 / ×4
- **Reset**, **Quit**

#### Cartridge hardware

| Game (examples) | On the GBA | On the DualSense |
|---|---|---|
| Boktai | solar sensor | slide a finger across the touchpad (left dark, right full sun); or Options → Sun |
| WarioWare: Twisted! | gyro | turn the controller like a steering wheel; axis and sensitivity in Options |
| Yoshi Topsy-Turvy | tilt | tilt the controller; how you hold it at start is "level" (Options → Level tilt here) |
| Drill Dozer, WarioWare: Twisted! | rumble motor | the controller's rumble (Options → Rumble) |

Which cartridge has what comes from `runtime/overrides.tsv` (mGBA's table).

**L3** opens rewind: the right stick scrubs the last 2 minutes (the further over, the faster; the
D-pad, a second at a time). The GBA's A goes back to that point; B or L3 cancels.

With the real BIOS, **Select + Start** during the intro hides the Nintendo logo (and A, B or the D-pad
make a sound): the original easter egg.

<a id="ra-en"></a>

### RetroAchievements

With [RA Hub](https://github.com/DevVeda/rahub) running on the PS5 and signed in:

- The game takes the session from `127.0.0.1:8045/session`.
- All its RetroAchievements calls go through `/dorequest`, since the title has no TLS of its own.
- Badges come from `/badge` and cheat updates from `/cheats`.
- An unlock shows a toast like the PS5's trophy one (badge + name), top right.

Play is **softcore**. RetroAchievements shows "Unknown Emulator" until it approves `GB5` as a client.
Achievements depend on the ROM's MD5: use clean (No-Intro) dumps.

### What's in each title

`/data/homebrew/<ID>/` on the console (`/app0` for the game):

| File | |
|---|---|
| `eboot.bin`, `sce_module/libc.prx` | the runtime |
| `game.gba` | the ROM |
| `sce_sys/` | `param.json`, `icon0.png`, `pic0.dds` / `pic1.dds` (4K BC7 backgrounds), `snd0.at9` |
| `bios.bin`, `cheats.txt`, `cheats_src.txt` | optional (the BIOS; the cheats and the libretro file they come from) |
| `defaults.txt` | the options it was built with |
| `game.sav` | the cartridge save |
| `state1-3.bin`, `settings.txt`, `cheats_on.txt` | states, options changed in the game and cheats turned on |
| `log.txt` | what happened: detected save type, BIOS, RetroAchievements, states |

### Layout

```
gb5.py           CLI + the web page's server (standard library only)
web/index.html       the web page
mksys.c              icon0.png and BC7 DDS backgrounds
runtime/
  build.sh           builds (Rust on this machine) and links + signs (Docker) eboot.bin
  src/lib.rs         gba_rt: C API over gba-core (frames, audio, saves, states, RAM)
  ps5/main.c         video (VideoOut), audio (AudioOut), controller, main loop
  ps5/menu.c         GB5 Menu and the rewind screen
  ps5/ra.c, card.c   RetroAchievements, the unlock toast and the sun meter
  ps5/cheats.c       CodeBreaker engine
  ps5/rewind.c       rewind history
  overrides.tsv      save type / hardware per game (from mGBA)
  vendor/            gba-core and armv4t (patched, see vendor/README.md), rcheevos, stb
tools/register.c     payload that registers/uninstalls titles in ShadowMountPlus
```

### Known limits

- The game runs on `gba-core`'s **interpreter**, not on gba-recomp's recompiled code (yet). It's more
  than fast enough.
- No e-Reader and no link cable.
- Home-screen music: only with a ready `.at9`.
- Cheats: plain CodeBreaker only. Encrypted GameShark/Action Replay ones aren't listed.
- Save states belong to a runtime version: after some updates, old ones are refused (the cartridge
  save is never touched).

What's pending is in `TODO.md` (local, not versioned).

### Troubleshooting

- **It's not on the home screen:** look at `/data/shadowmount/debug.log`.
  `game info unavailable` means ShadowMountPlus is too old.
- **The game doesn't start or acts up:** `/data/homebrew/<ID>/log.txt`.
- **`is in use` on deploy:** the game is open on the PS5; close it.
- **The icon doesn't change:** `deploy` reinstalls only when the artwork changed, because the home
  screen caches it.

### Licences

- The runtime is **GPL-3.0-or-later**: it links PS5_Vulkan's CRT and platform layer.
- `gba-core` and `armv4t`: MIT / Apache-2.0.
- rcheevos: MIT.
- stb: public domain / MIT.
- bc7enc: MIT / Unlicense.
- `runtime/overrides.tsv` derives from mGBA's table: MPL-2.0.

### AI disclosure

AI was used creating this piece of software, use at own discretion.

<a id="espanol"></a>

## Español

Convierte un ROM de Game Boy Advance en un **título nativo de PS5** (con jailbreak), con su icono, su
fondo y su nombre en el menú de inicio. El juego corre con el núcleo de
[gba-recomp](https://github.com/JRickey/gba-recomp) (`gba-core`), y trae de serie:

- **RetroAchievements** a través de [RA Hub](#ra-es): aviso al conectar, aviso de logro
  como el de trofeos de la PS5 y lista de logros en el menú.
- **GB5 Menu** con **L2 + R2**: estados de guardado, logros, cheats, opciones, turbo, reinicio.
- **Rebobinado** con **L3**: los últimos 2 minutos, con miniaturas.
- **Cheats** de [libretro-database](https://github.com/libretro/libretro-database) (CodeBreaker),
  actualizables desde la propia consola.
- **Hardware de cartucho** con el DualSense: giroscopio (girar el mando), inclinación, vibración y
  sensor solar (el touchpad).
- **Arte** de [SteamGridDB](https://www.steamgriddb.com), o el tuyo.
- **BIOS real** opcional (la tuya): intro de GBA al arrancar.
- Guardado de cartucho en todos los formatos (SRAM, Flash 64/128 KB, EEPROM) y reloj (RTC).

Se usa desde la terminal (`gb5.py`) o desde una web local con vista previa del menú de la PS5.

> **Legal:** usa solo ROMs y BIOS que hayas volcado tú. Nada de eso va en este repositorio.

### Inicio rápido (sin compilar)

1. Baja el zip de tu sistema en [Releases](../../releases/latest) y descomprímelo.
2. Abre `gb5` (`gb5.exe` en Windows): la web se abre en tu navegador. Todo lo que crea (`out/`, `cheatdb/`) queda junto al programa.
   - macOS: la primera vez, clic derecho → Abrir (la app no está firmada). Si aun así no abre:
     `xattr -d com.apple.quarantine gb5`.
   - Windows: SmartScreen puede avisar; "Más información" → "Ejecutar de todas formas".
3. Opcional, junto a `gb5`: `bios/gba_bios.bin` (tu BIOS) y `.env` con `STEAMGRIDDBAPI=<tu clave>`.

El mismo archivo sirve en terminal: `./gb5 build ROM --sgdb`, `./gb5 deploy out/<ID> --host <IP de la PS5>`…
Los requisitos de abajo son solo para compilar GB5 tú mismo.

### Requisitos

**Mac, Linux o Windows con WSL 2** (entre corchetes, los comandos de Ubuntu):

| Qué | Para qué |
|---|---|
| Python 3.10+ | `gb5.py` (solo librería estándar) |
| Un compilador de C: Xcode Command Line Tools (`xcode-select --install`) [`sudo apt install build-essential`] | compilar `mksys` (icono y fondos) |
| Rust (`brew install rustup` [[rustup.rs](https://rustup.rs)], luego `rustup default stable` y `rustup target add x86_64-unknown-freebsd`) | el núcleo |
| Docker con la imagen `ps5title` (Arch amd64 + clang 18); en WSL, Docker Desktop con la integración de WSL | enlazar y firmar el `eboot.bin` |
| [PS5_Vulkan + PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_Vulkan) ya compilados (por defecto en `~/ps5gpu`) | CRT, platform layer, `ps5-native-tool` y `libc.prx` del título |
| [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) + LLVM 18 (`brew install llvm@18` [`sudo apt install llvm-18 clang-18 lld-18`]) | el payload que registra los títulos |
| `.env` con `STEAMGRIDDBAPI=<tu clave>` | buscar arte en SteamGridDB (opcional) |

**WSL:** deja el proyecto dentro de WSL (`~/gb5`, no en `/mnt/c/...`: es mucho más rápido) y abre la
web desde Windows en `http://localhost:8080`.

**PS5** con jailbreak: kstuff, **ShadowMountPlus 1.7beta9 o más nuevo** (las versiones anteriores solo
aceptan IDs `PPSA`/`CUSA`/`FAKE`/`LAPY`), elfldr (puerto 9021) y un servidor FTP (puerto 2121).
Opcional: [RA Hub](https://github.com/DevVeda/rahub) para los logros ([cómo se usa](#ra-es)).

### Instalación

```bash
# librerías de imagen (dominio público / MIT)
mkdir -p third_party && cd third_party
curl -fLO https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
curl -fLO https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h
curl -fLO https://raw.githubusercontent.com/richgel999/bc7enc/master/bc7enc.c
curl -fLO https://raw.githubusercontent.com/richgel999/bc7enc/master/bc7enc.h
cd ..

runtime/build.sh                  # el eboot.bin (runtime/dist/), una vez y tras cada cambio del runtime
make -C tools                     # register.elf
```

`runtime/build.sh` lee `PS5GPU` (dónde están PS5_Vulkan y PS5_VulkanTemplate) e `IMAGE` (la imagen de
Docker) si no usas los valores por defecto; también desde `runtime/local.env` (local, no se versiona),
por ejemplo `PS5GPU=/ruta/a/ps5gpu`.

Opcional: tu BIOS en `bios/gba_bios.bin` (16 KB). Se incluye sola en cada juego.

### Uso

#### Terminal

```bash
# construir: nombre, icono y fondo de SteamGridDB, cheats, BIOS si está en bios/
python3 gb5.py build "roms/Pokemon - Emerald Version (USA, Europe).zip" --sgdb

# subir a la PS5 y registrarlo en el menú (solo sube lo que cambió)
python3 gb5.py deploy out/GBAE05350 --host IP-DE-TU-PS5

# quitarlo del menú (--delete borra también la carpeta, con ROM y partida)
python3 gb5.py uninstall GBAE05350 --host IP-DE-TU-PS5 --delete

# tras cambiar el runtime (runtime/build.sh): llevarlo a todos los juegos instalados en esa PS5
python3 gb5.py update-all --host IP-DE-TU-PS5
```

`update-all` solo toca los juegos de `out/` que están instalados en la consola, sube solo lo que cambió
y salta (avisando) los que estén abiertos.

Opciones de `build`:

| Opción | |
|---|---|
| `--sgdb` | nombre, icono y fondo de SteamGridDB (lo que pases a mano tiene prioridad) |
| `--name`, `--icon`, `--hero`, `--launch` | nombre e imágenes propias (cualquier formato; se recortan al centro) |
| `--music` | `snd0.at9` ya codificado (48 kHz, máx. 2 MiB; no hay encoder ATRAC9 libre) |
| `--size pixel\|fill\|16:9` | tamaño de la imagen al empezar (por defecto `16:9`) |
| `--a-button cross\|circle` | qué botón es la A de la GBA (por defecto `cross`) |
| `--lcd`, `--gba-colors` | filtro LCD / colores de la pantalla de GBA (por defecto apagados) |
| `--save` | importar un `.sav` (mGBA, VBA, cartucho). **Al desplegar reemplaza la partida de la consola** |
| `--bios` | tu BIOS; `--bios none` para no incluirla |
| `--title-id` | forzar el ID |

Acepta `.gba` y `.zip` (como vienen los sets No-Intro). Al construir dice qué tipo de guardado tiene el
juego, qué cheats lleva y qué hardware extra usa.

Las opciones de `--size`, `--a-button`, `--lcd` y `--gba-colors` son solo el punto de partida
(`defaults.txt` en el título). Lo que cambies en el juego (GB5 Menu → Opciones) se guarda en
`settings.txt` y siempre gana, aunque vuelvas a construir y desplegar.

#### Web

```bash
python3 gb5.py serve   # http://127.0.0.1:8080
```

Eliges ROM, arte (tuyo o de SteamGridDB), música, BIOS, partida y opciones de inicio; ves cómo quedará
en el menú de la PS5 y la lista de cheats; y construyes o subes a la consola. El botón **Actualizar todos
los juegos en la PS5** hace lo mismo que `update-all`.

#### El ID del título

Una PS5 exige 4 letras + 5 números. Se usa `GBA` + la región del código del cartucho y, en los números,
el resto del código y la revisión: Pokémon Emerald USA (`BPEE`) es `GBAE05350`. Es único por juego y
versión, así que reconstruir reemplaza el mismo icono. Los códigos con dígitos (Boktai `U3IE`) van al
rango 88000–99999; sin código (homebrew): `GBAX` + un número sacado del ROM.

### En el juego

| Mando | GBA |
|---|---|
| Cruz / Triángulo | A |
| Círculo / Cuadrado | B |
| L1 / R1 | L / R |
| Options | Start |
| Touchpad (clic) | Select |
| Cruceta / stick izquierdo | cruceta |

En **Opciones** se puede cambiar a A = Círculo, B = Cruz (las posiciones de Nintendo).

**L2 + R2** abre el **GB5 Menu** (el juego se pausa; al reabrirlo, el cursor sigue donde lo dejaste):

- **Guardar / Cargar estado** 1–3 (izquierda/derecha cambia la ranura)
- **Logros**: lista de RetroAchievements con progreso
- **Cheats**: activar o desactivar uno a uno (todos empiezan apagados). **Actualizar desde internet**
  baja la versión más reciente del juego en libretro-database a través de RA Hub, sin el computador, y mantiene
  activados los que tenías.
- **Opciones**: tamaño (16:9, llenar alto, píxel perfecto ×6), filtro LCD, colores de GBA, botón A ✕/○,
  y solo en los juegos que los tienen: vibración sí/no, sol, eje y sensibilidad del giroscopio,
  sensibilidad y centrado de la inclinación
- **Turbo** ×1 / ×2 / ×4
- **Reiniciar**, **Salir**

#### Hardware del cartucho

| Juego (ejemplos) | En la GBA | En el DualSense |
|---|---|---|
| Boktai | sensor solar | deslizar un dedo por el touchpad (izquierda oscuro, derecha pleno sol); o Opciones → Sol |
| WarioWare: Twisted! | giroscopio | girar el mando como un volante; eje y sensibilidad en Opciones |
| Yoshi Topsy-Turvy | inclinación | inclinar el mando; la posición al arrancar es "nivelado" (Opciones → Centrar inclinación) |
| Drill Dozer, WarioWare: Twisted! | motor de vibración | la vibración del mando (Opciones → Vibración) |

Qué cartucho tiene qué sale de `runtime/overrides.tsv` (la tabla de mGBA).

**L3** abre el rebobinado: el stick derecho recorre los últimos 2 minutos (más inclinado, más rápido;
la cruceta, de segundo en segundo). A de la GBA vuelve a ese punto; B o L3 cancela.

Con la BIOS real, **Select + Start** durante la intro esconde el logo de Nintendo (y A, B o la cruceta
suenan): el huevo de pascua original.

<a id="ra-es"></a>

### RetroAchievements

Con [RA Hub](https://github.com/DevVeda/rahub) corriendo en la PS5 y la sesión iniciada:

- El juego toma la sesión de `127.0.0.1:8045/session`.
- Todas sus llamadas a RetroAchievements pasan por `/dorequest`, porque el título no tiene TLS propio.
- Los badges llegan por `/badge` y la actualización de cheats por `/cheats`.
- Al desbloquear un logro sale un aviso como el de trofeos de la PS5 (badge + nombre), arriba a la derecha.

Se juega en **softcore**. RetroAchievements muestra "Unknown Emulator" hasta que apruebe `GB5`
como cliente. Los logros dependen del MD5 del ROM: usa dumps limpios (No-Intro).

### Qué hay en cada título

`/data/homebrew/<ID>/` en la consola (`/app0` para el juego):

| Archivo | |
|---|---|
| `eboot.bin`, `sce_module/libc.prx` | el runtime |
| `game.gba` | el ROM |
| `sce_sys/` | `param.json`, `icon0.png`, `pic0.dds` / `pic1.dds` (fondos BC7 4K), `snd0.at9` |
| `bios.bin`, `cheats.txt`, `cheats_src.txt` | opcionales (la BIOS; los cheats y de qué archivo de libretro salen) |
| `defaults.txt` | las opciones con que se construyó |
| `game.sav` | la partida del cartucho |
| `state1-3.bin`, `settings.txt`, `cheats_on.txt` | estados, opciones cambiadas en el juego y cheats activos |
| `log.txt` | qué pasó: guardado detectado, BIOS, RetroAchievements, estados |

### Estructura

```
gb5.py           CLI + servidor de la web (solo librería estándar)
web/index.html       la web
mksys.c              icon0.png y fondos DDS BC7
runtime/
  build.sh           compila (Rust en este equipo) y enlaza + firma (Docker) el eboot.bin
  src/lib.rs         gba_rt: API de C sobre gba-core (frames, audio, guardado, estados, RAM)
  ps5/main.c         video (VideoOut), audio (AudioOut), mando, bucle
  ps5/menu.c         GB5 Menu y pantalla de rebobinado
  ps5/ra.c, card.c   RetroAchievements, el aviso de logro y el medidor de sol
  ps5/cheats.c       motor CodeBreaker
  ps5/rewind.c       historial de rebobinado
  overrides.tsv      tipo de guardado / hardware por juego (de mGBA)
  vendor/            gba-core y armv4t (parcheados, ver vendor/README.md), rcheevos, stb
tools/register.c     payload que registra/desinstala títulos en ShadowMountPlus
```

### Límites conocidos

- El juego corre con el **intérprete** de `gba-core`, no con el código recompilado de gba-recomp
  (todavía). Va de sobra de rápido.
- No se emulan el e-Reader ni el cable link.
- Música del menú: solo con un `.at9` ya hecho.
- Cheats: solo CodeBreaker en claro. Los GameShark/Action Replay cifrados no se listan.
- Los estados de guardado son de la versión del runtime: tras ciertas actualizaciones, los viejos se
  rechazan (la partida del cartucho no se toca).

Lo pendiente está en `TODO.md` (local, no se versiona).

### Si algo falla

- **No aparece en el menú:** mira `/data/shadowmount/debug.log`.
  `game info unavailable` significa que ShadowMountPlus es demasiado viejo.
- **El juego no arranca o se comporta raro:** `/data/homebrew/<ID>/log.txt`.
- **`is in use` al desplegar:** el juego está abierto en la PS5; ciérralo.
- **El icono no cambia:** `deploy` reinstala solo cuando cambió el arte, porque el menú lo guarda en caché.

### Licencias

- El runtime es **GPL-3.0-or-later**: enlaza con el CRT y el platform layer de PS5_Vulkan.
- `gba-core` y `armv4t`: MIT / Apache-2.0.
- rcheevos: MIT.
- stb: dominio público / MIT.
- bc7enc: MIT / Unlicense.
- `runtime/overrides.tsv` deriva de la tabla de mGBA: MPL-2.0.

### Uso de IA

Se usó IA para crear este software; úsalo bajo tu propio criterio.
