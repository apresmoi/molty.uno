#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""firmware-boards.py — the boards under firmware/boards/, for scripts and CI.

A board is a folder firmware/boards/<vendor>/<model>/ with a board.json; folders
starting with "_" (the _template) are not boards. See docs/PORTING.md.

  scripts/firmware-boards.py list                 board folders, one per line
  scripts/firmware-boards.py matrix               GitHub Actions matrix JSON: [{"board","name"}]
  scripts/firmware-boards.py name <board>         board.json "name" of a folder (or of a name)
  scripts/firmware-boards.py dir <board>          the folder of a name (or of a folder)
  scripts/firmware-boards.py check                every board is wired up consistently
  scripts/firmware-boards.py check-image <bin> <board>
                                                  the app image carries that board's tag
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BOARDS = ROOT / "firmware" / "boards"
KCONFIG = ROOT / "firmware" / "main" / "Kconfig.projbuild"

REQUIRED = ("name", "title", "vendor", "target", "kconfig", "display", "flash", "psram", "caps", "usb", "detect", "status")
HEX16 = re.compile(r"^0x[0-9a-f]{4}$")
DETECT_PSRAM = ("none", "2MB", "8MB")
CAPS = ("display", "touch", "imu", "mic", "speaker", "btn_accept", "btn_reject", "battery", "usb_net", "serial")
NAME_RE = re.compile(r"^[a-z0-9][a-z0-9.-]{0,63}$")
# firmware/main/board/molty_board.h: MOLTY_IMAGE_TAG_OFFSET / _MAGIC
TAG_OFFSET, TAG_MAGIC = 0x20 + 256, b"MOLTYBRD"


def boards():
    out = []
    for js in sorted(BOARDS.rglob("board.json")):
        rel = js.parent.relative_to(BOARDS)
        if any(part.startswith("_") for part in rel.parts):
            continue
        out.append((rel.as_posix(), json.loads(js.read_text(encoding="utf-8"))))
    return out


def resolve(arg):
    for d, cfg in boards():
        if arg in (d, cfg.get("name")):
            return d, cfg
    sys.exit(f"firmware-boards: no board '{arg}' (try: scripts/firmware-boards.py list)")


def check():
    errors = []
    kconfig = KCONFIG.read_text(encoding="utf-8")
    names = set()
    for d, cfg in boards():
        where = f"boards/{d}"
        bad = lambda msg: errors.append(f"{where}: {msg}")
        for key in REQUIRED:
            if key not in cfg:
                bad(f'board.json lacks "{key}"')
        name = cfg.get("name", "")
        if not NAME_RE.match(name):
            bad(f'name "{name}" must be lowercase letters, digits, "." and "-"')
        if name in names:
            bad(f'name "{name}" is taken')
        names.add(name)
        if name != f"{d.split('/')[0]}-{d.split('/')[-1]}":
            bad(f'name "{name}" should be "<vendor>-<model>" of its folder')
        if len(d) > 55:
            bad("folder path longer than 55 characters (the image tag holds 55)")
        sym = cfg.get("kconfig", "")
        bare = sym.removeprefix("CONFIG_")
        if not re.search(rf"^config {re.escape(bare)}$", kconfig, re.M):
            bad(f"{sym}: no 'config {bare}' in the MOLTY_BOARD choice (main/Kconfig.projbuild)")
        if not re.search(rf'^\s*default "{re.escape(d)}" if {re.escape(bare)}$', kconfig, re.M):
            bad(f'MOLTY_BOARD_DIR has no `default "{d}" if {bare}` line (main/Kconfig.projbuild)')
        frag = BOARDS / d / "sdkconfig.board"
        if not frag.exists() or not re.search(rf"^{re.escape(sym)}=y$", frag.read_text(encoding="utf-8"), re.M):
            bad(f"sdkconfig.board must set {sym}=y")
        for f in ("board.cmake", "board.c", "board.h"):
            if not (BOARDS / d / f).exists():
                bad(f"{f} missing")
        board_c = BOARDS / d / "board.c"
        if board_c.exists() and not re.search(rf'\.id\s*=\s*"{re.escape(name)}"', board_c.read_text(encoding="utf-8")):
            bad(f'board.c: molty_board_desc .id must be "{name}"')
        caps = cfg.get("caps", [])
        if "display" not in caps or any(c not in CAPS for c in caps):
            bad(f"caps {caps}: needs display; known: {', '.join(CAPS)}")
        disp = cfg.get("display", {})
        if not (isinstance(disp.get("width"), int) and isinstance(disp.get("height"), int)
                and disp.get("shape") in ("round", "rect")):
            bad('display needs integer width/height and shape "round" or "rect"')
        if cfg.get("target") != "esp32s3":
            bad('target must be "esp32s3" (the only target built so far)')
        # identification for molty.uno/setup (website/src/features/setup/match.ts)
        usb = cfg.get("usb", [])
        if not usb or any(not HEX16.match(str(u.get("vid", ""))) or not HEX16.match(str(u.get("pid", ""))) for u in usb):
            bad('usb: a list of {"vid": "0x1a86", "pid": "0x55d3"} (lowercase hex), at least one')
        det = cfg.get("detect", {})
        if det.get("chip") != "ESP32-S3" or det.get("psram") not in DETECT_PSRAM or not re.match(r"^\d+MB$", str(det.get("flash", ""))):
            bad(f'detect: {{"chip": "ESP32-S3", "psram": one of {DETECT_PSRAM}, "flash": "16MB"}}')
        if board_c.exists():
            src = board_c.read_text(encoding="utf-8")
            pm = re.search(r"\.psram_mb\s*=\s*(\d+)", src)
            fm = re.search(r"\.flash_mb\s*=\s*(\d+)", src)
            want_p = 0 if det.get("psram") == "none" else int(str(det.get("psram", "0MB"))[:-2] or 0)
            if not pm or int(pm.group(1)) != want_p or not fm or f"{fm.group(1)}MB" != det.get("flash"):
                bad("board.c .psram_mb / .flash_mb must equal board.json detect (the firmware checks them at boot)")
    # two boards that identify the same way would make /setup refuse both
    all_b = boards()
    for i, (da, a) in enumerate(all_b):
        for db, b in all_b[i + 1:]:
            ua = {(u.get("vid"), u.get("pid")) for u in a.get("usb", [])}
            ub = {(u.get("vid"), u.get("pid")) for u in b.get("usb", [])}
            if a.get("detect") == b.get("detect") and ua & ub:
                errors.append(f"boards/{da} and boards/{db}: same detect fingerprint on a shared USB ID; /setup could not tell them apart")
    for e in errors:
        print(f"error: {e}", file=sys.stderr)
    if errors:
        sys.exit(1)
    print(f"firmware-boards: {len(boards())} board(s) OK: {', '.join(d for d, _ in boards())}")


def check_image(path, arg):
    d, _ = resolve(arg)
    data = Path(path).read_bytes()[: TAG_OFFSET + 64]
    if len(data) < TAG_OFFSET + 64 or data[0] != 0xE9 or data[0x20:0x24] != bytes.fromhex("3254cdab"):
        sys.exit(f"firmware-boards: {path} is not an ESP app image")
    if data[TAG_OFFSET:TAG_OFFSET + 8] != TAG_MAGIC:
        sys.exit(f"firmware-boards: {path} has no board tag at 0x{TAG_OFFSET:x}")
    tagged = data[TAG_OFFSET + 8:TAG_OFFSET + 64].split(b"\0")[0].decode()
    if tagged != d:
        sys.exit(f"firmware-boards: {path} is tagged '{tagged}', expected '{d}'")
    print(f"firmware-boards: {path} is tagged for {d}")


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    cmd = argv[1]
    if cmd == "list":
        print("\n".join(d for d, _ in boards()))
    elif cmd == "matrix":
        print(json.dumps([{"board": d, "name": c["name"]} for d, c in boards()]))
    elif cmd == "name" and len(argv) == 3:
        print(resolve(argv[2])[1]["name"])
    elif cmd == "dir" and len(argv) == 3:
        print(resolve(argv[2])[0])
    elif cmd == "check":
        check()
    elif cmd == "check-image" and len(argv) == 4:
        check_image(argv[2], argv[3])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
