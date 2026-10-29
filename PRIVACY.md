# GB5 privacy policy

[English version](#english) · [Versión en español](#espanol)

<a id="english"></a>

## English

Last updated: 2026-10-29.

### In short

GB5 has **no servers, no telemetry, no analytics and no accounts of its own**. Its author receives no
data from you, ever. Everything GB5 makes stays on your computer and your PS5 until you delete it.

### What leaves your computer, and to whom

GB5 (the CLI, the web page and the packaged app) only makes these connections, and only when you use the
feature that needs them:

| Where | What is sent | When |
|---|---|---|
| [SteamGridDB](https://www.steamgriddb.com) | the game's name as a search term and your SteamGridDB API key | when a build searches for art (`--sgdb`, or the web page with a key set) |
| GitHub (`api.github.com`, `raw.githubusercontent.com`) | a request for the [libretro-database](https://github.com/libretro/libretro-database) cheat list, then one `.cht` file for the game being built | when a build looks for cheats (the list is cached in `cheatdb/`) |
| your PS5, on your local network | the title's files over FTP (port 2121) and the registration payload to elfldr (port 9021) | when you deploy, update or uninstall |

The web page is served by GB5 itself on `127.0.0.1` only: other devices cannot reach it. Those services
handle requests under their own policies.

### On the console: RetroAchievements

A GB5 title talks to [RA Hub](https://github.com/DevVeda/rahub), a separate payload you run on the PS5,
over `127.0.0.1` only. RA Hub forwards it to [RetroAchievements](https://retroachievements.org) over HTTPS:

- your RetroAchievements username and Connect token (RA Hub signs in; GB5 only reads the session),
- the ROM's MD5 hash, to identify the game,
- achievement unlocks, rich presence (what you are doing in the game), leaderboard entries,
- GB5's User-Agent: `GB5/<version> (PS5) gba-core/<version> rcheevos/<version>`.

RetroAchievements handles that data under its own [terms and privacy policy](https://retroachievements.org/terms).
Achievement badges and in-console cheat updates also come through RA Hub (from RetroAchievements and GitHub).
Without RA Hub running and signed in, a title makes no network connections at all.

### What is stored, and where

- **Your computer:** `settings.json` (options and the SteamGridDB key; the key is only ever sent to
  SteamGridDB and never back to the web page), `bios/`, `cheatdb/` (cache), `out/` (built titles).
- **Your PS5:** each title's folder (`/data/homebrew/<ID>/`: ROM, saves, states, `settings.txt`,
  `log.txt`) and `/data/gb5/register.txt`. RA Hub keeps the RetroAchievements session token on the
  console only.

### Retention, server location and GDPR

GB5 keeps nothing remotely, so there is no retention period, no server location and no data warehouse:
no personal data is collected, processed or stored by GB5 or its author. Local files stay until you delete
them (deleting a title folder or `settings.json` removes them completely). For data held by
RetroAchievements, SteamGridDB or GitHub, use the rights and contacts in their own policies.

### Changes

Changes to this policy are made in this file, visible in the repository's history.

<a id="espanol"></a>

## Español

Última actualización: 29-10-2026.

### En resumen

GB5 **no tiene servidores, telemetría, analíticas ni cuentas propias**. Su autor no recibe ningún dato
tuyo, nunca. Todo lo que GB5 crea se queda en tu ordenador y tu PS5 hasta que lo borres.

### Qué sale de tu ordenador, y hacia quién

GB5 (la CLI, la web y la app empaquetada) solo hace estas conexiones, y solo cuando usas la función que
las necesita:

| Destino | Qué se envía | Cuándo |
|---|---|---|
| [SteamGridDB](https://www.steamgriddb.com) | el nombre del juego como búsqueda y tu clave de API de SteamGridDB | cuando un build busca arte (`--sgdb`, o la web con clave puesta) |
| GitHub (`api.github.com`, `raw.githubusercontent.com`) | una petición de la lista de cheats de [libretro-database](https://github.com/libretro/libretro-database), y luego un `.cht` del juego que se construye | cuando un build busca cheats (la lista se guarda en `cheatdb/`) |
| tu PS5, en tu red local | los archivos del título por FTP (puerto 2121) y el payload de registro a elfldr (puerto 9021) | al desplegar, actualizar o desinstalar |

La web la sirve el propio GB5 solo en `127.0.0.1`: otros dispositivos no pueden entrar. Esos servicios
tratan las peticiones según sus propias políticas.

### En la consola: RetroAchievements

Un título de GB5 habla con [RA Hub](https://github.com/DevVeda/rahub), un payload aparte que corres en la
PS5, solo por `127.0.0.1`. RA Hub lo reenvía a [RetroAchievements](https://retroachievements.org) por HTTPS:

- tu usuario de RetroAchievements y el token Connect (inicia sesión RA Hub; GB5 solo lee la sesión),
- el hash MD5 del ROM, para identificar el juego,
- logros desbloqueados, rich presence (qué haces en el juego), entradas de leaderboards,
- el User-Agent de GB5: `GB5/<versión> (PS5) gba-core/<versión> rcheevos/<versión>`.

RetroAchievements trata esos datos según sus propios [términos y política de privacidad](https://retroachievements.org/terms).
Los badges de los logros y la actualización de cheats en la consola también pasan por RA Hub (desde
RetroAchievements y GitHub). Sin RA Hub corriendo y con sesión, un título no hace ninguna conexión de red.

### Qué se guarda, y dónde

- **Tu ordenador:** `settings.json` (opciones y la clave de SteamGridDB; la clave solo se envía a
  SteamGridDB y nunca vuelve a la web), `bios/`, `cheatdb/` (caché), `out/` (títulos construidos).
- **Tu PS5:** la carpeta de cada título (`/data/homebrew/<ID>/`: ROM, partidas, estados, `settings.txt`,
  `log.txt`) y `/data/gb5/register.txt`. RA Hub guarda el token de sesión de RetroAchievements solo en la
  consola.

### Conservación, ubicación de servidores y RGPD

GB5 no guarda nada en remoto, así que no hay plazo de conservación, ubicación de servidores ni almacén
de datos: ni GB5 ni su autor recogen, tratan o guardan datos personales. Los archivos locales se quedan
hasta que los borres (borrar la carpeta de un título o `settings.json` los elimina del todo). Para los
datos que guardan RetroAchievements, SteamGridDB o GitHub, usa los derechos y contactos de sus propias
políticas.

### Cambios

Los cambios a esta política se hacen en este archivo, visibles en el historial del repositorio.
