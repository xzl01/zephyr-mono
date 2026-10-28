#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the real UI snapshot with native LVGL; keep artifacts out of tests/."""
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parents[1]
REPO = HERE.parents[1]
VOICE_PAGES = ["remote-voice", "remote-voice-streaming", "remote-voice-error"]
PAGES = ["settings", "sleep", "sleep-weather", "sleep-weather-extreme",
         "sleep-weather-flat", "sleep-no-weather", "sleep-no-date",
         "sleep-reading", "sleep-reading-home", "sleep-reading-rss",
         "sleep-reading-record", "sleep-reading-empty",
         "sleep-tomorrow", "sleep-tomorrow-empty", "sleep-blank", "wifi-scan", "wifi-list", "wifi-list-2", "wifi-password",
         "wifi-symbols", "wifi-63chars", "wifi-connecting", "wifi-failure",
         "wifi-scan-failure", "wifi-empty", "wifi-forget",
         "home", "home-populated", "launcher-1", "launcher-2", "library", "library-populated", "weread", "calendar",
         "news", "news-detail", "news-empty", "news-error", "news-syncing",
         "news-cached-error", "article-long",
         "news-ai", "news-four-sources", "news-no-sources", "news-table",
         "news-table-wide", "news-table-long", "news-table-after",
         "news-table-dense",
         "tasks", "tasks-populated", "notes", "notes-populated", "info", "content-center", "remote", "remote-pairing",
         "remote-passkey", "remote-media", "remote-presentation", "remote-mouse",
         "tools", "system",
         "system-unformatted", "power", "confirm-sleep", "confirm-shutdown",
         "confirm-ble", "confirm-reset", "confirm-format"]
MODULES = ["app", "interaction", "settings", "content_store", "content_validation",
           "organizer", "reader", "personal_data", "text_codec", "text_layout",
           "reader_cache", "epub", "archive_safety", "weread", "weread_view",
           "info_provider", "touch_gesture", "wifi_input", "fs_read",
           "almanac", "quotes", "rss_table"]
EXCLUDED = {"drivers", "others"}
EXCLUDED_LIBS = {"freetype", "tiny_ttf", "lodepng", "libpng", "bmp", "tjpgd",
                 "libjpeg_turbo", "libwebp", "gif", "qrcode", "barcode", "rlottie",
                 "svg", "thorvg", "ffmpeg", "gltf", "gstreamer", "frogfs", "expat",
                 "vg_lite_driver", "FT800-FT813"}
EXCLUDED_DRAW = {"vg_lite", "nema_gfx", "opengles", "dma2d", "pxp", "g2d",
                 "espressif", "renesas", "sdl"}


def run(args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def digest(paths):
    result = hashlib.sha256()
    for path in sorted(paths):
        result.update(str(path).encode())
        result.update(path.read_bytes())
    return result.hexdigest()


def main():
    # Matches PAPERMONO_BLE_MIC's current default-y firmware configuration.
    # The override retains the supported three-tab build for comparison.
    mic = os.environ.get("PAPERMONO_PREVIEW_BLE_MIC", "1")
    if mic not in {"0", "1"}:
        raise SystemExit("PAPERMONO_PREVIEW_BLE_MIC must be 0 or 1")
    available = PAGES + (VOICE_PAGES if mic == "1" else [])
    pages = sys.argv[1:] or available
    unknown = set(pages) - set(available)
    if unknown:
        raise SystemExit(f"Unknown fixture(s): {sorted(unknown)}; choose {available}; voice fixtures require PAPERMONO_PREVIEW_BLE_MIC=1")
    lvgl = Path(os.environ.get("LVGL", str(REPO.parent / "modules/lib/gui/lvgl"))).resolve()
    if not (lvgl / "lvgl.h").is_file():
        raise SystemExit("LVGL source missing; set LVGL to your LVGL 9.x checkout")
    out = Path(os.environ.get("OUT", str(REPO / "tmp/wifi-preview"))).resolve()
    # Prevent an accidental OUT pointing at tracked sources. Never clean/delete.
    if out.is_relative_to(REPO) and not out.is_relative_to(REPO / "tmp"):
        raise SystemExit(f"Refusing source-tree output directory: {out}")
    build = out / "build"
    obj = build / "obj"
    fsroot = build / "fs"
    for directory in (out, obj, fsroot):
        directory.mkdir(parents=True, exist_ok=True)
    snapshot = build / "papermono_ui_snapshot.c"
    snapshot.write_bytes((REPO / "src/papermono_ui.c").read_bytes())
    manifest_paths = [snapshot, HERE / "lv_conf.h"]
    manifest_paths += sorted((REPO / "include/papermono").glob("*.h"))
    manifest_paths += list((REPO / "src/generated").glob("*.c"))
    manifest_paths += [REPO / f"src/papermono_{module}.c" for module in MODULES]
    manifest_paths += [p for p in HERE.rglob("*")
                       if p.is_file() and "__pycache__" not in p.parts]
    revision = subprocess.run(["git", "-C", str(lvgl), "rev-parse", "HEAD"],
                              capture_output=True, text=True)
    manifest = "Synthetic content, hardware and Wi-Fi; real UI + native LVGL I1, 480x800; LVGL heap 65536 bytes\n"
    manifest += f"repo: {REPO}\nlvgl: {lvgl}\nlvgl revision: {revision.stdout.strip() or 'unknown'}\nfixtures: {' '.join(pages)}\nBLE microphone UI: {'enabled' if mic == '1' else 'disabled'}\n"
    manifest += "\n".join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p}" for p in manifest_paths)
    (out / "SOURCES.txt").write_text(manifest + "\n")
    run([sys.executable, HERE / "scripts/patch_ui.py", snapshot, build / "papermono_ui_preview.c"])
    cc = shlex.split(os.environ.get("CC", "cc"))
    flags = ["-std=gnu17", "-O1", "-g0", "-fno-strict-aliasing",
             "-DCONFIG_PAPERMONO_CONTENT_SERVER=1", "-DCONFIG_PAPERMONO_BLE_HID=1",
             "-DCONFIG_PAPERMONO_BLE_SYNC=1",
             "-DLV_CONF_INCLUDE_SIMPLE", f"-I{HERE}", f"-I{HERE / 'shims'}",
             f"-I{REPO / 'include'}", f"-I{lvgl}"]
    sources = []
    for source in sorted((lvgl / "src").rglob("*.c")):
        parts = source.relative_to(lvgl / "src").parts
        if parts[0] in EXCLUDED:
            continue
        if parts[0] == "libs" and parts[1] in EXCLUDED_LIBS:
            continue
        if parts[0] == "draw" and parts[1] in EXCLUDED_DRAW:
            continue
        if "arm2d" in parts or "asm" in parts or ("blend" in parts and ({"neon", "helium"} & set(parts))):
            continue
        sources.append(source)
    config_hash = digest(list(lvgl.rglob("*.h")) + list((HERE / "shims").rglob("*.h")) + [HERE / "lv_conf.h"])
    objects = []
    for source in sources:
        name = "lvgl_" + "_".join(source.relative_to(lvgl / "src").parts) + ".o"
        target = obj / name
        stamp = obj / (name + ".sha256")
        key = hashlib.sha256((config_hash + repr(cc + flags)).encode() + source.read_bytes()).hexdigest()
        if not target.exists() or not stamp.exists() or stamp.read_text() != key:
            run(cc + flags + ["-w", "-c", source, "-o", target])
            stamp.write_text(key)
        objects.append(target)
    print(f"LVGL: {len(sources)} translation units (content-addressed compile cache)", flush=True)
    ui_flags = flags + (["-DCONFIG_PAPERMONO_BLE_MIC=1"] if mic == "1" else [])
    real = [REPO / f"src/papermono_{module}.c" for module in MODULES]
    real += sorted((REPO / "src/generated").glob("*.c"))
    real += [HERE / "scripts/zephyr_host.c", HERE / "scripts/fs_host.c",
             HERE / "stubs/papermono_stubs.c", HERE / "scripts/preview_main.c",
             build / "papermono_ui_preview.c"]
    for index, source in enumerate(real):
        target = obj / f"real_{index}_{source.stem}.o"
        warnings = ["-Wall", "-Wextra"] if source.name == "papermono_ui_preview.c" else ["-w"]
        run(cc + ui_flags + warnings + ["-c", source, "-o", target])
        objects.append(target)
    run(cc + ["-o", build / "preview"] + objects + ["-lm"])
    for page in pages:
        # Preserve each synthetic filesystem and prevent seeded books/events in
        # one screenshot from silently populating a later empty-state fixture.
        fixture_fs = Path(tempfile.mkdtemp(prefix=f"{page}-", dir=fsroot))
        env = dict(os.environ, PAPERMONO_PREVIEW_FS=str(fixture_fs))
        run([build / "preview", page, build / f"{page}.pbm"], env=env, timeout=30)
        run([sys.executable, HERE / "scripts/pbm_to_png.py", build / f"{page}.pbm", out / f"{page}.png", page])
    run([sys.executable, HERE / "scripts/verify.py", out] + pages)
    if snapshot.read_bytes() != (REPO / "src/papermono_ui.c").read_bytes():
        print("NOTE: UI changed during build; rerun to preview latest shared revision.")
    print(f"PREVIEW BUILD OK: {len(pages)} fixtures in {out}")


if __name__ == "__main__":
    main()
