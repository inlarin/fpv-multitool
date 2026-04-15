"""Inject FW_VERSION as a -D build flag.

Priority:
  1. PLATFORMIO_BUILD_FLAGS contains FW_VERSION (CI sets this) -> leave it alone.
  2. Current commit has an annotated tag -> use it.
  3. Fallback -> "dev-<short-sha>" or "dev".
"""
import os
import subprocess

Import("env")  # noqa: F821  (injected by PlatformIO)


def _existing_flag_has_version(flags):
    return any("FW_VERSION" in f for f in flags)


def _git(args):
    try:
        return subprocess.check_output(
            ["git"] + args,
            stderr=subprocess.DEVNULL,
            cwd=env["PROJECT_DIR"],  # noqa: F821
        ).decode().strip()
    except Exception:
        return ""


def _detect_version():
    env_flags = os.environ.get("PLATFORMIO_BUILD_FLAGS", "")
    if "FW_VERSION" in env_flags:
        return None  # CI already provided it
    tag = _git(["describe", "--tags", "--exact-match"])
    if tag:
        return tag
    sha = _git(["rev-parse", "--short", "HEAD"])
    return f"dev-{sha}" if sha else "dev"


version = _detect_version()
if version:
    env.Append(BUILD_FLAGS=[f'-DFW_VERSION=\\"{version}\\"'])  # noqa: F821
    print(f"[inject_version] FW_VERSION = {version}")
