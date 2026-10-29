#!/usr/bin/env python3
"""GBA ROM -> PS5 home-menu title folder (Python 3 stdlib only).

  gb5.py build ROM [--sgdb] [--size 16:9] [--a-button cross] [--lcd] [--gba-colors] [--bios BIOS] [--save SAV] [--name N] [--icon IMG] [--hero IMG] [--launch IMG] [--music SND.at9] [--eboot EBOOT] [--out DIR]
  gb5.py deploy DIR --host PS5_IP
  gb5.py uninstall TITLE_ID --host PS5_IP [--delete]
  gb5.py update-all --host PS5_IP        latest runtime to every game in out/ (after runtime/build.sh)
  gb5.py cheats-update                 forget cached cheats (the next build gets the latest)
  gb5.py serve [--port 8080]          web UI: pick files, preview the home menu, build and deploy
"""
import argparse, base64, ftplib, hashlib, http.server, io, json, os, re, shutil, socket, struct, subprocess, sys, tempfile, time, urllib.parse, urllib.request, zipfile, zlib
from pathlib import Path

# Packaged (PyInstaller, see .github/workflows/release.yml): read-only files come from the bundle (RES),
# the user's files (settings.json, bios/, cheatdb/, out/) live next to the executable (ROOT).
FROZEN = getattr(sys, "frozen", False)
ROOT = Path(sys.executable).parent if FROZEN else Path(__file__).resolve().parent
RES = Path(getattr(sys, "_MEIPASS", ROOT))
MKSYS_NAME = "mksys.exe" if os.name == "nt" else "mksys"
MKSYS = RES / MKSYS_NAME if FROZEN else ROOT / "build" / MKSYS_NAME
AT9_GUID = bytes.fromhex("d242e147ba368d4d88fc61654f8c836c")  # KSDATAFORMAT_SUBTYPE_ATRAC9


def mksys(*args):
    if not FROZEN and (not MKSYS.exists() or MKSYS.stat().st_mtime < (ROOT / "mksys.c").stat().st_mtime):
        MKSYS.parent.mkdir(exist_ok=True)
        cc = os.environ.get("CC", "cc")
        cmd = [cc, "-O2", "-o", MKSYS, ROOT / "mksys.c", ROOT / "third_party" / "bc7enc.c", "-lm"]
        # macOS: a Command Line Tools ld older than its newest SDK can't read that SDK's .tbd files;
        # fall back to the older SDKs installed beside it.
        sdks = sorted(Path("/Library/Developer/CommandLineTools/SDKs").glob("MacOSX[0-9]*.sdk"), reverse=True) if sys.platform == "darwin" else []
        for sdk in [None, *sdks]:
            env = dict(os.environ, SDKROOT=str(sdk)) if sdk else None
            if subprocess.run(cmd, env=env, capture_output=True).returncode == 0:
                break
        else:
            raise RuntimeError("cannot build mksys with %r (on macOS: update the Command Line Tools; or set CC)" % cc)
    subprocess.run([MKSYS, *map(str, args)], check=True)


def check_at9(data):
    """The shell's snd0 rules: RIFF/WAVE ATRAC9, 48 kHz, <= 2 MiB."""
    if len(data) > 2 * 1024 * 1024:
        raise ValueError("snd0.at9 is over 2 MiB (stereo 192 kb/s caps at ~87 s)")
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE" or data[12:16] != b"fmt ":
        raise ValueError("music must be an ATRAC9 .at9 (RIFF/WAVE); there is no free AT9 encoder, use at9tool")
    if struct.unpack_from("<H", data, 20)[0] != 0xFFFE or data[44:60] != AT9_GUID:
        raise ValueError("music is a WAV but not ATRAC9")
    if struct.unpack_from("<I", data, 24)[0] != 48000:
        raise ValueError("snd0.at9 must be 48 kHz")


CHEATS_DIR = ROOT / "cheatdb"
CHEATS_REPO = "https://api.github.com/repos/libretro/libretro-database/contents/cht/Nintendo%20-%20Game%20Boy%20Advance"


def cheat_index():
    """{file name: download url} of libretro-database's GBA .cht files: fetched once (one request) and kept in
    cheatdb/index.json; offline and never fetched, the .cht files already in cheatdb/."""
    idx = CHEATS_DIR / "index.json"
    if idx.exists():
        return json.loads(idx.read_text())
    try:
        req = urllib.request.Request(CHEATS_REPO, headers={"User-Agent": "gb5", "Accept": "application/vnd.github+json"})
        with urllib.request.urlopen(req, timeout=30) as r:
            index = {Path(f["name"]).name: f["download_url"] for f in json.load(r)  # never a path from the listing
                     if f.get("name", "").endswith(".cht") and f.get("download_url")
                     and urllib.parse.urlparse(f["download_url"]).hostname == "raw.githubusercontent.com"}
    except (OSError, ValueError) as e:
        print("cheats: no se pudo bajar la lista (%s)" % e, file=sys.stderr)
        return {p.name: None for p in CHEATS_DIR.glob("*.cht")}
    CHEATS_DIR.mkdir(parents=True, exist_ok=True)
    idx.write_text(json.dumps(index))
    return index


def cheats_update():
    """Forget the cached list and .cht files: the next build fetches the latest of each game it needs."""
    shutil.rmtree(CHEATS_DIR, ignore_errors=True)
    print("cheats: %d juegos en libretro-database" % len(cheat_index()))


def _title_words(name):
    name = re.sub(r"\([^)]*\)|\[[^]]*\]", " ", Path(name).stem if name.lower().endswith((".zip", ".gba", ".cht")) else name)
    return re.sub(r"[^a-z0-9]+", "", name.lower())  # no spaces either: "Leaf Green" = "LeafGreen"


def find_cheats(rom_name, rom):
    """The libretro .cht for this ROM by title (its names aren't exact No-Intro: '(Code Breaker)', '_' for '&'),
    preferring one that shares the ROM name's region. Returns (path, warning) or (None, None)."""
    index = cheat_index()
    want = _title_words(rom_name)
    hits = [n for n in index if _title_words(n) == want]
    if not hits:
        return None, None
    regions = set(re.findall(r"USA|Europe|Japan|World", rom_name))
    hits.sort(key=lambda n: -len(regions & set(re.findall(r"USA|Europe|Japan|World", n))))
    path = CHEATS_DIR / hits[0]
    if not path.exists():  # only the games actually built get downloaded
        try:
            with urllib.request.urlopen(urllib.request.Request(index[hits[0]], headers={"User-Agent": "gb5"}), timeout=30) as r:
                data = r.read(1 << 20)
        except (OSError, ValueError) as e:
            print("cheats: no se pudo bajar %s (%s)" % (hits[0], e), file=sys.stderr)
            return None, None
        path.write_bytes(data)
    rev = re.search(r"Rev (\d+)|V1\.(\d)", path.name)
    warn = None
    if rev and rom is not None and int(rev.group(1) or rev.group(2)) != rom[0xBC]:
        warn = "los cheats son para la revisión %s del juego y este ROM es la %d: algunos pueden no funcionar" % (rev.group(1) or rev.group(2), rom[0xBC])
    return path, warn


CB_TYPES = set("012346789ABCDEF") - {"5", "9"}  # what runtime/ps5/cheats.c applies


def parse_cheats(path):
    """[(name, code, supported)] from a libretro .cht. Supported = CodeBreaker pairs (8+4 hex) the runtime runs."""
    text = path.read_text(errors="replace")
    out = []
    for i in range(int((re.search(r"^cheats\s*=\s*(\d+)", text, re.M) or [0, 0])[1])):
        name = re.search(r'^cheat%d_desc\s*=\s*"([^"]*)"' % i, text, re.M)
        code = re.search(r'^cheat%d_code\s*=\s*"([^"]*)"' % i, text, re.M)
        if not (name and code):
            continue
        parts = code[1].strip().split("+")
        pairs = list(zip(parts[::2], parts[1::2]))
        ok = len(parts) % 2 == 0 and bool(pairs) and all(
            re.fullmatch(r"[0-9A-Fa-f]{8}", a) and re.fullmatch(r"[0-9A-Fa-f]{4}", v) for a, v in pairs)
        ok = ok and all(a[0].upper() in CB_TYPES for a, v in pairs)
        out.append((name[1].replace("\t", " ").strip(), code[1].strip(), ok))
    return out


def save_info(rom):
    """(save type, extra hardware) the same way the runtime decides (gba-core backup.rs, rtc.rs):
    runtime/overrides.tsv first, then the SDK library strings in the ROM."""
    save, hw = "-", []
    for line in (RES / "runtime" / "overrides.tsv").read_text().splitlines():
        f = line.split("\t")
        if not line.startswith("#") and len(f) == 3 and f[0].encode() == rom[0xAC:0xB0]:
            save, hw = f[1], [h for h in f[2].split(",") if h != "-"]
    if save == "-":
        save = next((kind for needle, kind in ((b"FLASH1M_V", "flash128"), (b"FLASH512_V", "flash64"), (b"FLASH_V", "flash64"),
                                                (b"EEPROM_V", "eeprom"), (b"SRAM_V", "sram"), (b"SRAM_F_V", "sram")) if needle in rom), "none")
    if b"SIIRTC_V" in rom and "rtc" not in hw:
        hw.append("rtc")
    return save, hw


def title_id(rom):
    """PS5 ids are 4 letters + 5 digits, so "GBA" + the cartridge's game code (0xAC, e.g. BPPE)
    can't fit as text. Keep GBA + the code's region letter (E, J, P...), and pack its first three
    letters with the revision (0xBC) into the digits: 26^3 * 5 = 87,880 < 1e5, one-to-one, so two
    games never share a tile and a rebuild overwrites the same one."""
    code = rom[0xAC:0xB0].decode("ascii", "replace")
    if len(code) == 4 and all("A" <= c <= "Z" for c in code):
        n = ((ord(code[0]) - 65) * 26 + ord(code[1]) - 65) * 26 + ord(code[2]) - 65
        return "GBA%s%05d" % (code[3], n * 5 + min(rom[0xBC], 4))
    if len(code) == 4 and code.isalnum() and code.isupper() and "A" <= code[3] <= "Z":
        # codes with digits (U3IE Boktai, AA2E Super Mario Advance 2): keep the region, and put the rest in
        # 88000-99999, which letter-only codes never reach. ponytail: 12,000 buckets; --title-id on a clash
        return "GBA%s%05d" % (code[3], 88000 + zlib.crc32(rom[0xAC:0xAF] + bytes([rom[0xBC]])) % 12000)
    # No game code (homebrew): stable pseudo-random from the ROM, same range.
    return "GBAX%05d" % (88000 + zlib.crc32(rom) % 12000)


def param_json(tid, name, loudness):
    p = {
        "ageLevel": {"default": 0},
        "applicationCategoryType": 0,
        "applicationDrmType": "free",
        "attribute": 0, "attribute2": 0, "attribute3": 0,
        "conceptId": tid[4:],
        "contentBadgeType": 1,
        "contentId": "UP9000-%s_00-%s" % (tid, hashlib.md5(tid.encode()).hexdigest()[:16].upper()),
        "contentVersion": "01.000.000",
        "downloadDataSize": 256,
        "gameIntent": {"permittedIntents": [{"intentType": "launchActivity"}]},
        "localizedParameters": {"defaultLanguage": "en-US", "en-US": {"titleName": name}},
        "masterVersion": "01.00",
        "pubtools": {"creationDate": "2026-01-01 00:00:00", "toolVersion": "2.00"},
        "requiredSystemSoftwareVersion": "0x0000000000000000",
        "sdkVersion": "0x0000000000000000",
        "titleId": tid,
        "versionFileUri": "",
    }
    if loudness:
        p["pubtools"]["loudnessSnd0"] = "-28.00"
    return json.dumps(p, indent=2, ensure_ascii=False)


def read_rom(path):
    """The ROM bytes; a .zip (how No-Intro sets ship) must hold exactly one .gba/.agb."""
    path = Path(path)
    if path.suffix.lower() != ".zip":
        return path.read_bytes()
    with zipfile.ZipFile(path) as z:
        roms = [i for i in z.infolist() if Path(i.filename).suffix.lower() in (".gba", ".agb")]
        if len(roms) != 1:
            raise ValueError("%s: expected one .gba inside, found %d" % (path.name, len(roms)))
        if roms[0].file_size > 32 * 1024 * 1024:
            raise ValueError("%s: %s is bigger than any GBA cartridge" % (path.name, roms[0].filename))
        return z.read(roms[0])


GBA_BIOS_MD5 = "a860e8c0b6d573d191e4ec7db1b1e4f6"  # the GBA's own BIOS (AGB); NDS/other revisions differ
DEFAULT_BIOS = ROOT / "bios" / "gba_bios.bin"


SAVE_SIZES = {"sram": (32768,), "sram64": (65536,), "flash64": (65536,), "flash128": (131072,), "eeprom": (512, 8192), "eeprom512": (512,)}


SIZES = {"pixel": 0, "fill": 1, "16:9": 2}  # runtime/ps5/menu.h SIZE_*


def build(rom_path, name=None, icon=None, hero=None, launch=None, music=None, eboot=None, out="out", tid=None, bios=None, rom_name=None, save=None,
          size="16:9", a_button="cross", lcd=False, gba_colors=False):
    rom = read_rom(rom_path)
    if len(rom) < 0xC0 or len(rom) > 32 * 1024 * 1024 or rom[0xB2] != 0x96:
        raise ValueError("not a GBA ROM (bad size or header byte 0xB2 != 0x96)")
    name = name or rom[0xA0:0xAC].decode("ascii", "replace").strip("\0 ") or Path(rom_path).stem
    tid = tid or title_id(rom)
    if len(tid) != 9 or not tid[:4].isalpha() or not tid[4:].isdigit():
        raise ValueError("title id must look like ABCD12345")
    d = Path(out) / tid
    sys_ = d / "sce_sys"
    sys_.mkdir(parents=True, exist_ok=True)
    save_kind, hw = save_info(rom)
    names = {"none": "no guarda", "sram": "SRAM 32 KB", "sram64": "SRAM 64 KB", "flash64": "Flash 64 KB", "flash128": "Flash 128 KB",
             "eeprom": "EEPROM (512 B u 8 KB, se detecta al jugar)", "eeprom512": "EEPROM 512 B"}
    print("guardado: %s%s" % (names.get(save_kind, save_kind), " + reloj (RTC)" if "rtc" in hw else ""))
    how = {"light_sensor": "sensor solar: touchpad (dedo de izquierda a derecha) u OPCIONES > SOL",
           "gyro": "giroscopio: girar el mando (eje en OPCIONES > GIRO)", "rumble": "vibracion: la del mando",
           "tilt": "inclinacion: inclinar el mando (OPCIONES > CENTRAR INCLINACION)"}
    for h in hw:
        if h in how:
            print("hardware: " + how[h])
    missing = [h for h in hw if h != "rtc" and h not in how]
    if missing:  # gba-core has no model for these cartridge parts
        print("aviso: el cartucho usa %s, que no se emula: partes del juego pueden no funcionar" % ", ".join(missing), file=sys.stderr)
    (d / "game.gba").write_bytes(rom)
    # The game's starting options (the in-game OPTIONS menu still changes them; what the player sets wins).
    if size not in SIZES or a_button not in ("cross", "circle"):
        raise ValueError("size must be pixel, fill or 16:9; a_button cross or circle")
    (d / "defaults.txt").write_text("size=%d\nlcd=%d\ncolor=%d\nswapab=%d\n" % (SIZES[size], bool(lcd), bool(gba_colors), a_button == "cross"))
    print("opciones: tamaño %s, A = %s, LCD %s, colores GBA %s" % (size, "Cruz" if a_button == "cross" else "Círculo",
                                                                      "sí" if lcd else "no", "sí" if gba_colors else "no"))
    if save:  # a .sav from another emulator or a cartridge dump (raw, the usual format)
        data = Path(save).read_bytes()
        if not data:
            raise ValueError("%s is empty" % save)
        if len(data) not in SAVE_SIZES.get(save_kind, ()):
            print("aviso: el .sav pesa %d bytes y este juego guarda en %s (%s): se carga lo que quepa" % (
                len(data), save_kind, " o ".join(str(n) for n in SAVE_SIZES.get(save_kind, ())) or "nada"), file=sys.stderr)
        (d / "game.sav").write_bytes(data)
        print("partida: %s importada; al desplegar REEMPLAZA la partida guardada en la consola" % Path(save).name)
    # Cheats from libretro-database (just this game's file, cached in cheatdb/), all off until turned on in the game's menu.
    cht, warn = find_cheats(rom_name or Path(rom_path).name, rom)
    usable = [(n, c) for n, c, ok in (parse_cheats(cht) if cht else []) if ok]
    if cht:  # the game's menu can refresh it later through RA Hub (CHEATS > UPDATE FROM THE INTERNET)
        (d / "cheats_src.txt").write_text(cht.name + "\n", encoding="utf-8")
    elif (d / "cheats_src.txt").exists():
        (d / "cheats_src.txt").unlink()
    if usable:
        (d / "cheats.txt").write_text("".join("%s\t%s\n" % nc for nc in usable), encoding="utf-8")
        print("cheats: %d de %s" % (len(usable), cht.name))
        if warn:
            print("aviso: " + warn, file=sys.stderr)
    else:
        if (d / "cheats.txt").exists():
            (d / "cheats.txt").unlink()
        print("cheats: ninguno")
    # The player's own BIOS (never shipped): --bios, else bios/gba_bios.bin if it's there; "none" = built-in HLE.
    bios = None if bios == "none" else Path(bios) if bios else DEFAULT_BIOS if DEFAULT_BIOS.exists() else None
    if bios:
        data = bios.read_bytes()
        if len(data) != 16384:
            raise ValueError("%s: a GBA BIOS is 16384 bytes, this is %d" % (bios, len(data)))
        if hashlib.md5(data).hexdigest() != GBA_BIOS_MD5:
            print("aviso: %s no es la BIOS oficial de GBA (md5 distinto); puede no arrancar" % bios.name, file=sys.stderr)
        (d / "bios.bin").write_bytes(data)
        print("bios: %s (intro de GBA al arrancar)" % bios.name)
    elif (d / "bios.bin").exists():
        (d / "bios.bin").unlink()  # the runtime reads ROM data through the bus; RetroAchievements hashes it (MD5)
    if icon:
        mksys("icon", icon, sys_ / "icon0.png")
    if hero:
        mksys("pic", hero, sys_ / "pic0.dds")  # selected-tile background
        mksys("pic", launch or hero, sys_ / "pic1.dds")  # launch transition
    if music:
        data = Path(music).read_bytes()
        check_at9(data)
        (sys_ / "snd0.at9").write_bytes(data)
    eboot = Path(eboot) if eboot else RES / "runtime" / "dist" / "eboot.bin"  # runtime/build.sh
    if eboot.exists():
        shutil.copy(eboot, d / "eboot.bin")
        if (eboot.parent / "sce_module").is_dir():  # libc.prx the eboot loads
            shutil.copytree(eboot.parent / "sce_module", d / "sce_module", dirs_exist_ok=True)
    elif not (d / "eboot.bin").exists():
        print("note: no eboot.bin (run runtime/build.sh); the folder has art and ROM but will not launch", file=sys.stderr)
    (sys_ / "param.json").write_text(param_json(tid, name, (sys_ / "snd0.at9").exists()), encoding="utf-8")
    print("built", d)
    return d


REG_DIR = "/data/gb5"


def connect(host, port=2121):
    ftp = ftplib.FTP()
    try:
        ftp.connect(host, port, timeout=10)
    except OSError as e:
        raise SystemExit("cannot reach the PS5 FTP server at %s:%d (%s): is the console on, jailbroken, with FTP running?" % (host, port, e))
    ftp.login()
    return ftp


def upload_changed(ftp, folder, remote, sent):
    """Upload what differs from `sent` (relative path -> sha256 already on the console), updating it as
    files go up. Returns the relative paths uploaded."""
    changed = []
    for p in sorted([folder, *folder.rglob("*")]):
        rel = p.relative_to(folder).as_posix()
        r = remote + "/" + rel if p != folder else remote
        if p.is_dir():
            try:
                ftp.mkd(r)
            except ftplib.error_perm:
                pass  # exists
            try:
                ftp.sendcmd("SITE CHMOD 777 " + r)  # without it the title fails with CE-107750-0
            except ftplib.Error:
                pass
            continue
        digest = hashlib.sha256(p.read_bytes()).hexdigest()
        if sent.get(rel) == digest:
            continue
        with p.open("rb") as f:
            try:
                ftp.storbinary("STOR " + r, f)
            except ftplib.error_perm as e:
                if "busy" in str(e):  # the console has the running title's eboot.bin open
                    raise SystemExit("%s is in use: close the game on the PS5, then deploy again" % r)
                raise
        sent[rel] = digest
        changed.append(rel)
        print("up", r)
    return changed


def deploy(folder, host):
    folder = Path(folder)
    remote = "/data/homebrew/" + folder.name
    # What this console already has (sha256 per file, from the last deploy): only changes go up.
    # ponytail: trusts the record; delete out/<ID>.deployed.json to force a full upload
    record = folder.parent / (folder.name + ".deployed.json")
    try:
        all_hosts = json.loads(record.read_text())
    except (OSError, ValueError):
        all_hosts = {}
    sent = all_hosts.setdefault(host, {})
    ftp = connect(host)
    try:
        changed = upload_changed(ftp, folder, remote, sent)
    finally:  # what got up stays recorded, even when a busy eboot.bin stops the upload
        record.write_text(json.dumps(all_hosts, indent=1))
    art_changed = any(rel.startswith("sce_sys/") for rel in changed)
    # The home menu keeps the art it took at install time, even if /user/appmeta is rewritten:
    # reinstall an installed title when its art changed. The folder (and game.sav) stays.
    installed = True
    try:
        ftp.dir("/user/app/" + folder.name, lambda _: None)
    except ftplib.error_perm:
        installed = False
    if installed and art_changed:
        smp(ftp, host, ["-" + folder.name])
        time.sleep(5)
    if not installed or art_changed:
        smp(ftp, host, [remote])
    ftp.quit()


def update_all(host, out="out"):
    """The latest runtime (runtime/dist, from runtime/build.sh) into every title in out/ that is installed on
    that console, then deploy each: only what changed goes up. A game open on the console is skipped and
    reported, the rest carry on. Folders of games no longer installed (uninstalled, tests) are left alone."""
    eboot = RES / "runtime" / "dist" / "eboot.bin"
    if not eboot.exists():
        raise SystemExit("no runtime/dist/eboot.bin: run runtime/build.sh first")
    ftp = connect(host)
    lines = []
    ftp.dir("/user/app", lines.append)
    ftp.quit()
    installed = {l.split()[-1] for l in lines}
    results = []
    for d in sorted(p for p in Path(out).iterdir() if p.is_dir() and p.name in installed and (p / "sce_sys" / "param.json").exists()):
        shutil.copy(eboot, d / "eboot.bin")
        if (eboot.parent / "sce_module").is_dir():
            shutil.copytree(eboot.parent / "sce_module", d / "sce_module", dirs_exist_ok=True)
        name = json.loads((d / "sce_sys" / "param.json").read_text(encoding="utf-8"))["localizedParameters"]["en-US"]["titleName"]
        try:
            deploy(d, host)
            results.append({"id": d.name, "name": name, "ok": True})
        except SystemExit as e:  # busy eboot.bin: the game is open
            results.append({"id": d.name, "name": name, "ok": False, "error": str(e)})
        print("%s %s %s" % ("ok " if results[-1]["ok"] else "-- ", d.name, results[-1].get("error", name)))
    return results


def uninstall(title_id, host, delete=False):
    title_id = title_id.upper()
    if len(title_id) != 9 or not title_id[:4].isalpha() or not title_id[4:].isdigit():
        raise SystemExit("title id must look like ABCD12345")
    ftp = connect(host)
    if delete:  # first: with the folder still there, ShadowMountPlus's next scan installs it again
        ftp_rmtree(ftp, "/data/homebrew/" + title_id)
    smp(ftp, host, ["-" + title_id])
    ftp.quit()


def ftp_rmtree(ftp, path):
    def ok(cmd):  # this server answers DELE with "226 File deleted", which ftplib calls an error
        try:
            r = ftp.sendcmd(cmd)
        except ftplib.error_reply as e:
            r = str(e)
        if r[:1] != "2":
            raise ftplib.error_perm(r)
    lines = []
    ftp.dir(path, lines.append)
    for line in lines:
        name = line.split(None, 8)[-1]
        if name not in (".", ".."):
            ftp_rmtree(ftp, path + "/" + name) if line.startswith("d") else ok("DELE %s/%s" % (path, name))
    ok("RMD " + path)


def smp(ftp, host, lines, elfldr_port=9021):
    """Ask ShadowMountPlus to add folders / uninstall (-TITLEID) titles. Its API only answers on the
    console, so tools/register.elf runs there (sent to elfldr) and reads the lines from register.txt."""
    try:
        ftp.mkd(REG_DIR)
    except ftplib.error_perm:
        pass
    ftp.storbinary("STOR %s/register.txt" % REG_DIR, io.BytesIO("".join(l + "\n" for l in lines).encode()))
    try:
        ftp.delete(REG_DIR + "/register.log")
    except (ftplib.error_perm, ftplib.error_reply):  # missing, or this server's "226 File deleted"
        pass
    try:
        with socket.create_connection((host, elfldr_port), timeout=10) as s:
            s.sendall((RES / "tools" / "register.elf").read_bytes())
    except OSError as e:
        print("elfldr did not take register.elf on %s:%d (%s): do it from ShadowMountPlus on the console" % (host, elfldr_port, e))
        return
    for _ in range(20):
        time.sleep(0.5)
        log = io.BytesIO()
        try:
            ftp.retrbinary("RETR %s/register.log" % REG_DIR, log.write)
        except ftplib.error_perm:
            continue
        if b"done" in log.getvalue() or b"not running" in log.getvalue():
            print(log.getvalue().decode(errors="replace").strip())
            return
    print("register.elf sent, but it wrote no log: check that elfldr runs payloads")


SETTINGS = ROOT / "settings.json"  # the web UI's Settings tab; never in git (it holds the API key)
SETTINGS_DEFAULT = {"sgdb_key": "", "size": "16:9", "a_button": "cross", "lcd": False, "gba_colors": False}


def load_settings():
    try:
        saved = json.loads(SETTINGS.read_text())
    except (OSError, ValueError):
        saved = {}
    return {k: saved.get(k, v) for k, v in SETTINGS_DEFAULT.items()}


def bios_info():
    """The BIOS every game gets (bios/gba_bios.bin): whether there is one and whether it's the official one."""
    data = DEFAULT_BIOS.read_bytes() if DEFAULT_BIOS.exists() else None
    return {"bios_set": data is not None, "bios_official": data is not None and hashlib.md5(data).hexdigest() == GBA_BIOS_MD5}


def settings_reply(s):
    """What the page gets: never the key itself, only whether there is one."""
    return dict(s, sgdb_key="", sgdb_key_set=bool(s["sgdb_key"]), **bios_info())


def save_settings(new):
    """Merge the known keys of new into settings.json, checked. An empty sgdb_key keeps the saved one.
    bios (base64) replaces bios/gba_bios.bin, bios_remove deletes it."""
    if new.get("bios"):
        data = base64.b64decode(new["bios"])
        if len(data) != 16384:
            raise ValueError("a GBA BIOS is 16384 bytes, this is %d" % len(data))
        DEFAULT_BIOS.parent.mkdir(exist_ok=True)
        DEFAULT_BIOS.write_bytes(data)
    elif new.get("bios_remove"):
        DEFAULT_BIOS.unlink(missing_ok=True)
    s = load_settings()
    if new.get("size") is not None:
        if new["size"] not in SIZES:
            raise ValueError("size: " + str(new["size"]))
        s["size"] = new["size"]
    if new.get("a_button") is not None:
        if new["a_button"] not in ("cross", "circle"):
            raise ValueError("a_button: " + str(new["a_button"]))
        s["a_button"] = new["a_button"]
    for k in ("lcd", "gba_colors"):
        if new.get(k) is not None:
            s[k] = bool(new[k])
    if new.get("sgdb_key"):
        key = str(new["sgdb_key"]).strip()
        if not re.fullmatch(r"[0-9A-Za-z]{16,128}", key):
            raise ValueError("that doesn't look like a SteamGridDB API key")
        s["sgdb_key"] = key
    SETTINGS.write_text(json.dumps(s, indent=2))
    return s


SGDB = "https://www.steamgriddb.com/api/v2"
SGDB_CDN = ("cdn2.steamgriddb.com", "cdn.steamgriddb.com")


def sgdb(path):
    key = load_settings()["sgdb_key"]
    if not key:
        raise ValueError("no SteamGridDB API key: add it in Settings")
    req = urllib.request.Request(SGDB + path, headers={"Authorization": "Bearer " + key, "User-Agent": "gb5"})
    with urllib.request.urlopen(req, timeout=15) as r:
        return json.load(r).get("data") or []


def fetch_art(url, dest):
    # only SteamGridDB's CDN: the page sends URLs, the server downloads them
    if urllib.parse.urlparse(url).hostname not in SGDB_CDN:
        raise ValueError("art URL must come from SteamGridDB")
    req = urllib.request.Request(url, headers={"User-Agent": "gb5"})
    with urllib.request.urlopen(req, timeout=30) as r:
        dest.write_bytes(r.read(64 * 1024 * 1024))


def sgdb_auto(rom_path, tmp):
    """CLI --sgdb: name, icon and hero from SteamGridDB's first hits for the file name
    ("Pokemon - Emerald Version (USA, Europe).zip" -> "Pokemon Emerald Version")."""
    query = Path(rom_path).stem.split("(")[0].replace(" - ", " ").replace("&", " ").strip()
    games = sgdb("/search/autocomplete/" + urllib.parse.quote(query, safe=""))
    if not games:
        print("SteamGridDB: nothing for %r" % query, file=sys.stderr)
        return None, None, None
    gid = games[0]["id"]
    art = {}
    for kind, params in (("icon", "/grids/game/%d?dimensions=512x512,1024x1024"), ("hero", "/heroes/game/%d")):
        hits = sgdb(params % gid)
        if hits:
            art[kind] = Path(tmp) / (kind + Path(urllib.parse.urlparse(hits[0]["url"]).path).suffix)
            fetch_art(hits[0]["url"], art[kind])
    print("SteamGridDB: %s (%s)" % (games[0]["name"], ", ".join(art) or "no art"))
    return games[0]["name"], art.get("icon"), art.get("hero")


def zip_name(folder):
    """'<home screen name>_<TITLEID>.zip', minus the characters file systems refuse."""
    p = json.loads((folder / "sce_sys" / "param.json").read_text(encoding="utf-8"))["localizedParameters"]
    name = re.sub(r'[\\/:*?"<>|]+', "", p[p["defaultLanguage"]]["titleName"]).strip()
    return "%s_%s.zip" % (name, folder.name)


class Web(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        try:
            if u.path == "/":
                return self.reply(200, (RES / "web" / "index.html").read_bytes(), "text/html; charset=utf-8")
            if u.path == "/logo.svg":
                return self.reply(200, (RES / "assets" / "logo.svg").read_bytes(), "image/svg+xml")
            if u.path == "/api/zip":  # a built folder as <ID>.zip, to copy to the console by hand
                tid = q["id"][0]
                folder = Path("out") / tid
                if not re.fullmatch(r"[A-Z]{4}\d{5}", tid) or not folder.is_dir():
                    raise ValueError("no built folder " + tid)
                buf = io.BytesIO()
                with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
                    for f in sorted(folder.rglob("*")):
                        if f.is_file():
                            z.write(f, Path(tid) / f.relative_to(folder))
                return self.reply(200, buf.getvalue(), "application/zip", {"Content-Disposition": "attachment; filename*=UTF-8''" +
                                  urllib.parse.quote(zip_name(folder))})
            if u.path == "/api/settings":
                data = settings_reply(load_settings())
            elif u.path == "/api/sgdb/search":
                data = sgdb("/search/autocomplete/" + urllib.parse.quote(q["q"][0], safe=""))
            elif u.path == "/api/cheats":  # the cheats a ROM (by file name and header) would get
                rom_hdr = base64.b64decode(q["hdr"][0]) if q.get("hdr") else None  # a .zip: the page can't read it
                cht, warn = find_cheats(q["name"][0], rom_hdr)
                data = {"file": cht.name if cht else None, "warn": warn, "db": bool(cheat_index()),
                        "cheats": [{"name": n, "ok": ok} for n, c, ok in (parse_cheats(cht) if cht else [])]}
            elif u.path == "/api/sgdb/art":
                gid, kind = int(q["id"][0]), q["kind"][0]
                params = {"grids": "?dimensions=512x512,1024x1024", "heroes": ""}[kind]  # square grids make better icons
                data = [{"url": a["url"], "thumb": a["thumb"], "w": a["width"], "h": a["height"]} for a in sgdb("/%s/game/%d%s" % (kind, gid, params))]
            else:
                return self.send_error(404)
            self.reply(200, json.dumps({"ok": True, "data": data}).encode(), "application/json")
        except Exception as e:
            self.reply(400, json.dumps({"ok": False, "error": str(e)}).encode(), "application/json")

    def do_POST(self):
        if self.path == "/api/update-all":
            try:
                req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                data = update_all(req["host"])
                return self.reply(200, json.dumps({"ok": True, "data": data}).encode(), "application/json")
            except (Exception, SystemExit) as e:
                return self.reply(400, json.dumps({"ok": False, "error": str(e)}).encode(), "application/json")
        if self.path == "/api/settings":
            try:
                s = save_settings(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
                return self.reply(200, json.dumps({"ok": True, "data": settings_reply(s)}).encode(), "application/json")
            except (Exception, SystemExit) as e:
                return self.reply(400, json.dumps({"ok": False, "error": str(e)}).encode(), "application/json")
        if self.path not in ("/api/build", "/api/deploy"):
            return self.send_error(404)
        try:
            req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            with tempfile.TemporaryDirectory() as tmp:
                files = {}
                for k in ("rom", "icon", "hero", "launch", "music", "save"):
                    if req.get(k):  # {"name": ..., "data": base64} or {"url": SteamGridDB CDN}
                        src = req[k].get("name") or urllib.parse.urlparse(req[k].get("url", "")).path
                        files[k] = Path(tmp) / (k + Path(src).suffix)
                        if "url" in req[k]:
                            fetch_art(req[k]["url"], files[k])
                        else:
                            files[k].write_bytes(base64.b64decode(req[k]["data"]))
                if "rom" not in files:
                    raise ValueError("pick a ROM")
                s = load_settings()  # the GB5 Menu's starting options: from Settings
                opts = {k: s[k] for k in ("size", "a_button", "lcd", "gba_colors")}
                d = build(files.pop("rom"), name=req.get("name") or None,
                          rom_name=req["rom"].get("name"), **opts, **files)
            if self.path == "/api/deploy":
                deploy(d, req["host"])
            self.reply(200, json.dumps({"ok": True, "folder": str(d.resolve()), "id": d.name, "zip": zip_name(d)}).encode(), "application/json")
        except Exception as e:  # report to the page, keep serving
            self.reply(400, json.dumps({"ok": False, "error": str(e)}).encode(), "application/json")

    def reply(self, code, body, ctype, headers={}):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        for k, v in headers.items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=not FROZEN)
    b = sub.add_parser("build")
    b.add_argument("rom")
    for o in ("name", "icon", "hero", "launch", "music", "eboot", "title-id"):
        b.add_argument("--" + o)
    b.add_argument("--out", default="out")
    # Unset: the web UI's Settings (settings.json), else 16:9, A on Cross, LCD and GBA colours off.
    b.add_argument("--size", choices=list(SIZES), help="picture: pixel (x6), fill (fill height) or 16:9")
    b.add_argument("--a-button", choices=["cross", "circle"], help="GBA A on Cross or Circle")
    b.add_argument("--lcd", action=argparse.BooleanOptionalAction, help="LCD grid")
    b.add_argument("--gba-colors", action=argparse.BooleanOptionalAction, help="GBA screen colours")
    b.add_argument("--save", help="import a .sav (raw, as mGBA/VBA/cartridge dumps); replaces the console's save on deploy")
    b.add_argument("--bios", help="your own GBA BIOS (16 KB) for the boot intro; default bios/gba_bios.bin if present, 'none' to skip")
    b.add_argument("--sgdb", action="store_true", help="name, icon and hero from SteamGridDB (what you pass wins)")
    d = sub.add_parser("deploy")
    d.add_argument("folder")
    d.add_argument("--host", required=True)
    u = sub.add_parser("uninstall")
    u.add_argument("title_id")
    u.add_argument("--host", required=True)
    u.add_argument("--delete", action="store_true", help="also delete /data/homebrew/<ID> (ROM, save) from the console")
    ua = sub.add_parser("update-all", help="push the latest runtime to every game in out/ and deploy them")
    ua.add_argument("--host", required=True)
    sub.add_parser("cheats-update", help="forget cached cheats: the next build gets the latest from libretro-database")
    s = sub.add_parser("serve")
    s.add_argument("--port", type=int, default=8080)
    a = ap.parse_args()
    if a.cmd is None:  # the packaged app opened with a double click: the web UI, in the browser
        os.chdir(ROOT)  # out/ next to the executable, not wherever the OS started it
        a.cmd, a.port = "serve", 8080
    if a.cmd == "build":
        s = load_settings()
        with tempfile.TemporaryDirectory() as tmp:
            name, icon, hero = sgdb_auto(a.rom, tmp) if a.sgdb else (None, None, None)
            build(a.rom, a.name or name, a.icon or icon, a.hero or hero, a.launch, a.music, a.eboot, a.out, a.title_id, a.bios, save=a.save,
                  **{k: s[k] if getattr(a, k) is None else getattr(a, k) for k in ("size", "a_button", "lcd", "gba_colors")})
    elif a.cmd == "deploy":
        deploy(a.folder, a.host)
    elif a.cmd == "update-all":
        update_all(a.host)
    elif a.cmd == "cheats-update":
        cheats_update()
    elif a.cmd == "uninstall":
        uninstall(a.title_id, a.host, a.delete)
    else:
        # localhost only: the page accepts arbitrary file uploads and FTP targets
        try:
            srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Web)
        except OSError:
            if not FROZEN:
                raise
            srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Web)  # port taken: any free one
        url = "http://127.0.0.1:%d" % srv.server_address[1]
        print("open " + url)
        if FROZEN:
            import webbrowser
            webbrowser.open(url)
        srv.serve_forever()


if __name__ == "__main__":
    main()
