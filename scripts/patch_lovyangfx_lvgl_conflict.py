"""Patch LovyanGFX's library.json to skip its bundled lv_font/ subsystem.

Background: LovyanGFX 1.2 ships an internal mini-LVGL compatibility layer
under `src/lgfx/v1/lv_font/` so it can render LVGL font files directly.
That layer redefines `LV_COLOR_FORMAT_*` enums, which conflict at compile
time with the real LVGL library (lvgl/lvgl@^9.2.0) when both are linked
into the same env.

We don't need LovyanGFX to render LVGL fonts -- LVGL itself owns that --
so we tell PIO not to compile `lv_font/*.c` by injecting a srcFilter into
LovyanGFX's `library.json`.

This script runs as a `pre:` extra_script in the LVGL env. It edits the
extracted library copy under `.pio/libdeps/<env>/LovyanGFX/library.json`
in-place. Idempotent: if our srcFilter is already present, the script is
a no-op. If LovyanGFX is re-extracted, the patch is re-applied on the
next build.
"""

import json
import os
from pathlib import Path

# pylint: disable=undefined-variable
Import("env")  # noqa: F821 -- provided by SCons under PIO

PROJECT_DIR = Path(env["PROJECT_DIR"])  # noqa: F821
ENV_NAME    = env["PIOENV"]             # noqa: F821

LIB_JSON = PROJECT_DIR / ".pio" / "libdeps" / ENV_NAME / "LovyanGFX" / "library.json"

SRC_FILTER_VALUE = "+<*> -<lgfx/v1/lv_font/**>"

if not LIB_JSON.is_file():
    print(f"[patch_lovyangfx] library.json not yet present at {LIB_JSON} — "
          f"will be applied on the next build after libs are extracted.")
else:
    with LIB_JSON.open("r", encoding="utf-8") as fp:
        data = json.load(fp)

    build = data.setdefault("build", {})
    current = build.get("srcFilter")

    if current == SRC_FILTER_VALUE:
        # Already patched.
        pass
    else:
        build["srcFilter"] = SRC_FILTER_VALUE
        with LIB_JSON.open("w", encoding="utf-8") as fp:
            json.dump(data, fp, indent=2)
        print(f"[patch_lovyangfx] applied lv_font/ exclusion to {LIB_JSON}")
