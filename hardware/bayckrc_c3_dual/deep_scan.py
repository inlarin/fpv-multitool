#!/usr/bin/env python3
"""Deep scan of Bayck RC C3 Dual dump — hunts for:

  1. WiFi AP password (the one the RX shows when it opens its own hotspot)
  2. Hidden / operator-only config strings
  3. CRSF frame type + extended-type constants
  4. CRSF command / parameter dispatch tables

Assumes the dump layout we already established:
  app0 @ 0x00010000..0x001f0000   (vanilla ExpressLRS 3.5.3)
  app1 @ 0x001f0000..0x003d0000   (MILELRS v3.48)
"""

from __future__ import annotations
import argparse
import json
import re
from pathlib import Path
from typing import Any

APP0 = (0x00010000, 0x001e0000)
APP1 = (0x001f0000, 0x001e0000)


def slice_app(data: bytes, base: int, size: int) -> bytes:
    return data[base:base + size]


# ---------------------------------------------------------------------------
# Part 1 — WiFi AP password hunt
# ---------------------------------------------------------------------------

def find_ap_creds(data: bytes, app_label: str, app_bytes: bytes, app_base: int) -> list[dict[str, Any]]:
    """ELRS AP credentials are passed to `WiFi.softAP(ssid, password)`.
    String literals line up back-to-back in rodata. We locate every
    plausible SSID and sniff the bytes right after for a password-shaped
    literal.

    Heuristics for the SSID:
      - "ExpressLRS RX"           (vanilla default)
      - "MILELRS v3.48 RX"        (observed via scan)
      - "MILELRS v3.48 RX 1016"
      - any ASCII string containing ' RX ' after 'ELRS' or 'MILELRS'

    Heuristics for the password (follows SSID in .rodata, NUL-terminated):
      - 8..63 printable ASCII chars, no spaces, not a format specifier,
        not itself an SSID.
    """
    hits: list[dict[str, Any]] = []

    ssid_patterns = [
        re.compile(rb"ExpressLRS RX[^\x00]{0,32}\x00"),
        re.compile(rb"MILELRS[^\x00]{0,40}\x00"),
    ]

    for pat in ssid_patterns:
        for m in pat.finditer(app_bytes):
            ssid = m.group(0).rstrip(b"\x00").decode("utf-8", "replace")
            # Peek the next printable run — may be the password immediately after,
            # or a short padding field, or another string entirely.
            after_start = m.end()
            # skip any trailing NULs
            while after_start < len(app_bytes) and app_bytes[after_start] == 0:
                after_start += 1
            # grab the next printable run up to 96 chars
            end = after_start
            while end < len(app_bytes) and 0x20 <= app_bytes[end] < 0x7F and end - after_start < 96:
                end += 1
            trailer = app_bytes[after_start:end].decode("ascii", "replace")
            # password must be 8..63 printable, no spaces, not a format spec
            looks_like_pwd = (
                len(trailer) >= 8 and len(trailer) <= 63 and
                " " not in trailer and "%" not in trailer and
                "/" not in trailer and "@" not in trailer and
                not trailer.startswith(("http", "ExpressLRS", "MILELRS", "BAYCKRC", "/"))
            )
            hits.append({
                "app": app_label,
                "ssid_offset": f"0x{app_base + m.start():08x}",
                "ssid": ssid,
                "trailer": trailer,
                "trailer_offset": f"0x{app_base + after_start:08x}",
                "looks_like_password": looks_like_pwd,
            })

    return hits


# Known ELRS default:  SSID "ExpressLRS RX",  password "expresslrs"
# We also grep for any standalone occurrence of the literal "expresslrs"
# and any other lower-case "*lrs" token with length 8+ — those are common
# default-password patterns.

def find_standalone_passwords(data: bytes, base: int) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    # Known plaintext defaults and custom strings worth surfacing.
    candidates = [
        b"expresslrs",
        b"milelrs",
        b"MILELRS",
        b"rubikon",
        b"RUBIKON",
        b"bayck",
        b"BAYCK",
    ]
    for tag in candidates:
        for m in re.finditer(re.escape(tag), data):
            ctx = data[max(0, m.start() - 16):m.end() + 32]
            out.append({
                "at": f"0x{base + m.start():08x}",
                "match": tag.decode(),
                "context_hex": ctx.hex(),
                "context_ascii": "".join(chr(b) if 32 <= b < 127 else "." for b in ctx),
            })
    return out


# ---------------------------------------------------------------------------
# Part 2 — CRSF command / parameter scan
# ---------------------------------------------------------------------------

# Reference CRSF frame types (ELRS + TBS):
CRSF_TYPES = {
    0x02: "GPS",
    0x08: "BATTERY_SENSOR",
    0x09: "BAROALT_VARIO",
    0x14: "LINK_STATISTICS",
    0x16: "RC_CHANNELS_PACKED",
    0x17: "SUBSET_RC_CHANNELS_PACKED",
    0x1C: "LINK_STATISTICS_RX",
    0x1D: "LINK_STATISTICS_TX",
    0x1E: "ATTITUDE",
    0x21: "FLIGHT_MODE",
    0x28: "DEVICE_PING",
    0x29: "DEVICE_INFO",
    0x2B: "PARAMETER_SETTINGS_ENTRY",
    0x2C: "PARAMETER_READ",
    0x2D: "PARAMETER_WRITE",
    0x2E: "ELRS_STATUS",
    0x32: "COMMAND",
    0x34: "LOGGING",
    0x36: "SUB_TYPE",
    0x38: "REMOTE_RELATED",
    0x3A: "RADIO_ID",
    0x7A: "KISS_REQ",
    0x7B: "KISS_RESP",
    0x7F: "MSP_REQ",
    0x80: "MSP_RESP",
    0x81: "MSP_WRITE",
}

# CRSF COMMAND subtype tables — ELRS-specific extended commands after
# type=0x32. ELRS uses realm byte + cmd byte.
# Ref: https://github.com/ExpressLRS/ExpressLRS/blob/master/src/lib/CrsfProtocol/crsf_protocol.h
CRSF_COMMAND_REALMS = {
    0x01: "FC",
    0x03: "BLUETOOTH",
    0x05: "OSD",
    0x08: "VTX",
    0x09: "LED",
    0x0A: "GENERAL",       # COMMAND_REBOOT_BOOTLOADER lives here
    0x10: "CROSSFIRE",
    0x20: "FLOW_CONTROL",
    0x22: "SCREEN",
    0x28: "DISPLAY",
}


def scan_crsf_types(app_bytes: bytes, app_base: int) -> list[dict[str, Any]]:
    """Find every byte sequence that looks like a CRSF frame-type constant
    in .rodata (they often appear as bytes in switch-case jump tables).

    A single byte constant on its own isn't meaningful — but when several
    known CRSF type bytes cluster together (e.g. the jump table for the
    CrsfService receiver parser), that's a very strong signal. We return
    clusters of 4+ known types within 64-byte windows.
    """
    # Build a set of known-type bytes
    type_set = bytes(sorted(CRSF_TYPES.keys()))
    hits: list[dict[str, Any]] = []
    window = 64
    for i in range(0, len(app_bytes) - window):
        w = app_bytes[i:i + window]
        found = [(j, b) for j, b in enumerate(w) if b in type_set]
        # require 4+ distinct type bytes AND at least 2 of those in the
        # "high-activity" subset (0x2B/0x2C/0x2D/0x32/0x28/0x29)
        distinct = len(set(b for _, b in found))
        core = len(set(b for _, b in found) & {0x2B, 0x2C, 0x2D, 0x32, 0x28, 0x29})
        if distinct >= 4 and core >= 2:
            hits.append({
                "at": f"0x{app_base + i:08x}",
                "types_found": sorted({CRSF_TYPES[b] for _, b in found}),
                "raw_hex": w.hex(),
            })
            # jump ahead to avoid overlapping duplicates
            i += window
    # dedupe by type-set signature within 256-byte proximity
    deduped: list[dict[str, Any]] = []
    last_at = -1
    for h in hits:
        at = int(h["at"], 16)
        if at - last_at > 128:
            deduped.append(h)
            last_at = at
    return deduped[:50]  # keep the first 50 clusters


def find_crsf_strings(app_bytes: bytes, app_base: int) -> list[dict[str, Any]]:
    """Any string containing CRSF/ELRS/PARAM keywords — likely menu labels
    for the param tree shown in the ELRS configurator UI."""
    keywords = [
        b"Packet Rate", b"Telem Ratio", b"Switch Mode", b"Dynamic Power",
        b"Model Match", b"VTX Power", b"VTX Band", b"VTX Channel",
        b"Band", b"Channel", b"Power",
        b"Enable WiFi", b"Update", b"BLE",
        b"Bind", b"Binding", b"UID",
        b"Region", b"Domain",
        # MILELRS-specific hints
        b"EW Scanner", b"Swarm", b"Encrypt", b"Custom Freq",
        b"VX", b"RUBIKON",
        b"Lock on first connection",
        # Reboot / flash commands
        b"bootloader", b"Reboot",
    ]
    out: list[dict[str, Any]] = []
    for kw in keywords:
        for m in re.finditer(re.escape(kw), app_bytes, re.IGNORECASE):
            # expand to the full printable run around the hit
            start = m.start()
            end = m.end()
            while start > 0 and 0x20 <= app_bytes[start - 1] < 0x7F:
                start -= 1
            while end < len(app_bytes) and 0x20 <= app_bytes[end] < 0x7F:
                end += 1
            if end - start < 3:
                continue
            s = app_bytes[start:end].decode("ascii", "replace")
            out.append({
                "at": f"0x{app_base + m.start():08x}",
                "kw": kw.decode(),
                "string": s,
            })
    return out


# ---------------------------------------------------------------------------
# Part 3 — ELRS parameter tree inference
# ---------------------------------------------------------------------------

# ELRS param entries are serialised into CRSF frames; at rest in .rodata
# they're declared as C structs. We can at least enumerate the labels by
# grepping for recognisable param-name strings used by the open-source
# config tree and then looking for same-offset groupings.

def inventory_param_labels(app_bytes: bytes, app_base: int) -> list[dict[str, Any]]:
    """List all printable strings 3..48 chars long that look like
    parameter-tree labels (ASCII, spaces allowed, maybe punctuation)."""
    pat = re.compile(rb"[A-Za-z][A-Za-z0-9 _\-:/().,%+!?]{2,47}")
    out: list[dict[str, Any]] = []
    for m in pat.finditer(app_bytes):
        s = m.group(0).decode("ascii", "replace")
        # filter: we want labels, not generic strings — short, looks like
        # a UI caption (starts with cap, has at least one space or ends
        # in a setting-y suffix)
        if (s[0].isupper() and
            (" " in s or any(s.endswith(suf) for suf in
                              (":", "...", ")", "s", "e", "d", "y", "n", "t", "x")))):
            pass
        else:
            continue
        # filter out things that look like error messages or paths
        if s.startswith(("E (", "I (", "W (", "D (", "/", "C:", "http")):
            continue
        if "%" in s or "\\" in s:
            continue
        if len(s) < 4:
            continue
        out.append({
            "at": f"0x{app_base + m.start():08x}",
            "label": s,
        })
    # unique by label, keeping earliest offset
    seen: dict[str, dict[str, Any]] = {}
    for row in out:
        if row["label"] not in seen:
            seen[row["label"]] = row
    return list(seen.values())


# ---------------------------------------------------------------------------

def render_md(data: bytes, report: dict[str, Any]) -> str:
    L: list[str] = []
    W = L.append
    W("# Bayck RC C3 Dual - deep scan\n")
    W(f"Source dump: `{report['dump']}`")
    W(f"Size: {report['size']:,} bytes\n")

    # 1. WiFi
    W("## WiFi AP — SSID + password pairs\n")
    for h in report["ap_creds"]:
        marker = "[LIKELY PASSWORD]" if h["looks_like_password"] else "[next string]"
        W(f"- `{h['app']}` SSID at `{h['ssid_offset']}`: **`{h['ssid']}`**")
        W(f"  - {marker} at `{h['trailer_offset']}`: **`{h['trailer']}`**")
    W("")

    W("### Stand-alone password-candidate strings\n")
    for h in report["standalone_pwds"]:
        W(f"- `{h['at']}` — `{h['match']}` — ctx: `{h['context_ascii']}`")
    W("")

    # 2. CRSF
    W("## CRSF\n")
    for app_label in ("app0", "app1"):
        W(f"### {app_label} — CRSF frame-type clusters\n")
        clusters = report["crsf_types"].get(app_label, [])
        if not clusters:
            W("_(no dense cluster found)_")
            W("")
            continue
        for c in clusters[:10]:
            W(f"- at `{c['at']}` — types: {', '.join(c['types_found'])}")
        W("")
        W(f"### {app_label} — CRSF-related strings\n")
        strs = report["crsf_strings"].get(app_label, [])
        # dedupe by string
        seen: set[str] = set()
        rows = []
        for s in strs:
            if s["string"] in seen:
                continue
            seen.add(s["string"])
            rows.append(s)
        W(f"_(total {len(rows)} unique)_")
        W("")
        W("| Offset | Label |")
        W("|--------|-------|")
        for s in rows[:80]:
            esc = s["string"].replace("|", r"\|")
            W(f"| `{s['at']}` | `{esc}` |")
        W("")

    # 3. Param labels
    W("## Parameter-tree labels (heuristic)\n")
    W("Likely menu labels discovered in each app's rodata. Not every hit is a param — "
      "false-positives expected. Useful for mapping ELRS Configurator UI field to source offset.\n")
    for app_label in ("app0", "app1"):
        rows = report["param_labels"].get(app_label, [])
        W(f"### {app_label}\n")
        W(f"_(total {len(rows)} unique labels)_")
        W("")
        W("| Offset | Label |")
        W("|--------|-------|")
        # sort by offset, cap at 120 per app
        for r in sorted(rows, key=lambda x: int(x["at"], 16))[:120]:
            esc = r["label"].replace("|", r"\|")
            W(f"| `{r['at']}` | `{esc}` |")
        W("")

    return "\n".join(L)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("dump", help="Path to raw flash dump (.bin)")
    ap.add_argument("--md", help="Markdown output (default: <dump>.deepscan.md)")
    ap.add_argument("--json", dest="json_out", help="JSON output (default: <dump>.deepscan.json)")
    args = ap.parse_args()

    dump_path = Path(args.dump)
    data = dump_path.read_bytes()
    print(f"[+] Loaded {len(data):,} bytes")

    report: dict[str, Any] = {"dump": str(dump_path), "size": len(data)}

    apps = [
        ("app0", slice_app(data, *APP0), APP0[0]),
        ("app1", slice_app(data, *APP1), APP1[0]),
    ]

    # 1. AP creds
    ap_creds: list[dict[str, Any]] = []
    for name, sl, base in apps:
        ap_creds.extend(find_ap_creds(data, name, sl, base))
    report["ap_creds"] = ap_creds
    print(f"[+] SSID hits: {len(ap_creds)}")
    for h in ap_creds[:8]:
        mark = " ** password? **" if h["looks_like_password"] else ""
        print(f"    [{h['app']}] ssid={h['ssid']!r} -> {h['trailer']!r}{mark}")

    # 2. standalone password candidates
    stand: list[dict[str, Any]] = []
    for name, sl, base in apps:
        stand.extend(find_standalone_passwords(sl, base))
    report["standalone_pwds"] = stand

    # 3. CRSF type clusters
    report["crsf_types"] = {}
    for name, sl, base in apps:
        report["crsf_types"][name] = scan_crsf_types(sl, base)
        print(f"[+] {name} CRSF type clusters: {len(report['crsf_types'][name])}")

    # 4. CRSF-related strings
    report["crsf_strings"] = {}
    for name, sl, base in apps:
        report["crsf_strings"][name] = find_crsf_strings(sl, base)

    # 5. Param-tree label inventory
    report["param_labels"] = {}
    for name, sl, base in apps:
        report["param_labels"][name] = inventory_param_labels(sl, base)
        print(f"[+] {name} heuristic param labels: {len(report['param_labels'][name])}")

    # write outputs
    md_path = Path(args.md) if args.md else dump_path.with_suffix(".deepscan.md")
    md_path.write_text(render_md(data, report), encoding="utf-8")
    print(f"[+] Wrote {md_path}")

    json_path = Path(args.json_out) if args.json_out else dump_path.with_suffix(".deepscan.json")
    json_path.write_text(
        json.dumps(report, indent=2, ensure_ascii=False, default=str),
        encoding="utf-8")
    print(f"[+] Wrote {json_path}")


if __name__ == "__main__":
    main()
