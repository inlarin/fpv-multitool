#!/usr/bin/env python3
"""Bayck RC C3 Dual - ELRS/MILELRS firmware dump parser.

Produces a human-readable Markdown report from a raw ESP32-C3 flash dump:

  - partition table
  - per-partition summary (app0 / app1 / nvs / spiffs / coredump)
  - version + target + git hash per app slot
  - embedded JSON configs (hardware.json from app0, MILELRS user config from
    app1 - the one that actually holds the operator's binding_rate /
    encryption_key / frequencies / VTX table)
  - NVS v2 walk (proper span-aware - doesn't misread continuation slots)
  - WiFi credentials (STA + AP from `nvs.net80211`)
  - ELRS eeprom blob hints (binding phrase / UID candidates)
  - MAC address candidates
  - interesting strings index

Usage:
    python parse.py dump.bin                          # auto-writes .parsed.md + .json
    python parse.py dump.bin --md out.md --json j.json
"""

from __future__ import annotations
import argparse
import json
import re
import struct
from pathlib import Path
from typing import Any, Optional

# ---------------------------------------------------------------------------
# Partition table (ESP-IDF v1, little-endian)
# ---------------------------------------------------------------------------

PARTITION_MAGIC = b"\xaa\x50"
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_ENTRY_SIZE = 32

PART_TYPE_NAMES = {0: "app", 1: "data"}
APP_SUBTYPE_NAMES = {
    0x00: "factory",
    0x10: "ota_0 (app0)", 0x11: "ota_1 (app1)", 0x12: "ota_2", 0x13: "ota_3",
    0x20: "test",
}
DATA_SUBTYPE_NAMES = {
    0x00: "ota (otadata)", 0x01: "phy", 0x02: "nvs", 0x03: "coredump",
    0x04: "nvs_keys", 0x05: "efuse_em", 0x06: "undefined",
    0x80: "esphttpd", 0x81: "fat", 0x82: "spiffs",
}


def parse_partition_table(data: bytes, offset: int) -> list[dict[str, Any]]:
    parts: list[dict[str, Any]] = []
    for i in range(0, 0x1000, PARTITION_ENTRY_SIZE):
        p = offset + i
        if p + PARTITION_ENTRY_SIZE > len(data):
            break
        entry = data[p:p + PARTITION_ENTRY_SIZE]
        if entry[:2] != PARTITION_MAGIC:
            break
        ptype, subtype, off, size = struct.unpack("<BBII", entry[2:12])
        name = entry[12:28].rstrip(b"\x00").decode("utf-8", "replace")
        type_name = PART_TYPE_NAMES.get(ptype, f"?{ptype:02x}")
        sub_name = (APP_SUBTYPE_NAMES if ptype == 0 else DATA_SUBTYPE_NAMES).get(
            subtype, f"?{subtype:02x}")
        parts.append({
            "name": name,
            "type": ptype, "subtype": subtype,
            "type_name": type_name, "subtype_name": sub_name,
            "offset": off, "size": size,
            "range_hex": f"0x{off:08x}..0x{off+size:08x}",
        })
    return parts


def parse_otadata(data: bytes, offset: int) -> dict[str, Any]:
    """ESP-IDF otadata has 2 x 4 KB sectors - each holds a 32-byte record.

    Record layout:
        seq:u32  label[20]  state:u32  crc:u32
    Active slot = (max_seq - 1) & 1, i.e. highest seq wins.
    Blank (all 0xFF) => bootloader falls back to app0.
    """
    result: dict[str, Any] = {"sectors": [], "active_slot": None, "note": ""}
    blank = True
    seqs: list[int] = []
    for i, secoff in enumerate([offset, offset + 0x1000]):
        if secoff + 32 > len(data):
            continue
        rec = data[secoff:secoff + 32]
        if rec != b"\xff" * 32:
            blank = False
        seq = struct.unpack("<I", rec[0:4])[0]
        state = struct.unpack("<I", rec[24:28])[0]
        crc = struct.unpack("<I", rec[28:32])[0]
        result["sectors"].append({
            "index": i, "offset": secoff, "seq": seq,
            "state": state, "crc": crc,
            "blank": rec == b"\xff" * 32,
        })
        if seq != 0xFFFFFFFF:
            seqs.append(seq)

    if blank:
        result["note"] = "otadata blank -> bootloader falls back to app0 (if stock 2nd-stage)"
        result["active_slot"] = 0
    elif seqs:
        maxs = max(seqs)
        result["active_slot"] = (maxs - 1) & 1
        result["note"] = f"highest seq={maxs} -> active slot = ({maxs}-1)&1 = {result['active_slot']}"
    return result


# ---------------------------------------------------------------------------
# NVS v2 walk (ESP-IDF)
# ---------------------------------------------------------------------------

NVS_PAGE_SIZE = 4096
NVS_PAGE_HEADER_SIZE = 32
NVS_ENTRY_SIZE = 32
NVS_PAGE_ENTRIES = 126  # (4096 - 32) / 32

NVS_PAGE_STATE_ACTIVE = 0xFFFFFFFE
NVS_PAGE_STATE_FULL = 0xFFFFFFFC
NVS_PAGE_STATE_FREEING = 0xFFFFFFF8

# Type byte - high nibble encodes category
NVS_TYPE_U8 = 0x01; NVS_TYPE_I8 = 0x11
NVS_TYPE_U16 = 0x02; NVS_TYPE_I16 = 0x12
NVS_TYPE_U32 = 0x04; NVS_TYPE_I32 = 0x14
NVS_TYPE_U64 = 0x08; NVS_TYPE_I64 = 0x18
NVS_TYPE_STR = 0x21
NVS_TYPE_BLOB_DATA = 0x42   # legacy blob (deprecated)
NVS_TYPE_BLOB_IDX = 0x48    # index entry for chunked blob
NVS_TYPE_BLOB_DATA_V2 = 0x41  # v2 chunked blob payload
NVS_TYPE_ANY = 0xff

TYPE_NAMES = {
    NVS_TYPE_U8: "u8", NVS_TYPE_I8: "i8",
    NVS_TYPE_U16: "u16", NVS_TYPE_I16: "i16",
    NVS_TYPE_U32: "u32", NVS_TYPE_I32: "i32",
    NVS_TYPE_U64: "u64", NVS_TYPE_I64: "i64",
    NVS_TYPE_STR: "str",
    NVS_TYPE_BLOB_DATA: "blob",
    NVS_TYPE_BLOB_IDX: "blob_idx",
    NVS_TYPE_BLOB_DATA_V2: "blob_data",
}

# Entry layout (32 bytes):
#   0    ns_id: u8
#   1    type:  u8
#   2    span:  u8
#   3    chunk_index: u8
#   4..7 crc32: u32
#   8..23 key:  char[16] (NUL-padded)
#   24..31 data: 8 bytes - interpreted per type, or header of str/blob payload


def _decode_entry_value(entry: bytes, page: bytes, e_off: int, e_type: int) -> Any:
    """Decode the 8-byte data slot of an entry, or for STR/BLOB pull the
    payload that lives in the following entries' data areas."""
    try:
        if e_type in (NVS_TYPE_U8,):
            return entry[24]
        if e_type in (NVS_TYPE_I8,):
            v = entry[24]; return v - 256 if v > 127 else v
        if e_type in (NVS_TYPE_U16, NVS_TYPE_I16):
            fmt = "<h" if e_type == NVS_TYPE_I16 else "<H"
            return struct.unpack(fmt, entry[24:26])[0]
        if e_type in (NVS_TYPE_U32, NVS_TYPE_I32):
            fmt = "<i" if e_type == NVS_TYPE_I32 else "<I"
            return struct.unpack(fmt, entry[24:28])[0]
        if e_type in (NVS_TYPE_U64, NVS_TYPE_I64):
            fmt = "<q" if e_type == NVS_TYPE_I64 else "<Q"
            return struct.unpack(fmt, entry[24:32])[0]
        if e_type == NVS_TYPE_STR:
            # header: size:u16, crc:u16 (at 24..27), payload in next entries' data.
            size = struct.unpack("<H", entry[24:26])[0]
            data_start = e_off + NVS_ENTRY_SIZE
            raw = page[data_start:data_start + size]
            return raw.rstrip(b"\x00").decode("utf-8", "replace")
        if e_type in (NVS_TYPE_BLOB_DATA, NVS_TYPE_BLOB_DATA_V2):
            size = struct.unpack("<H", entry[24:26])[0]
            data_start = e_off + NVS_ENTRY_SIZE
            raw = page[data_start:data_start + min(size, 4096)]
            return {
                "size": size,
                "hex_preview": raw[:64].hex(),
                "ascii_preview": "".join(
                    chr(b) if 32 <= b < 127 else "." for b in raw[:64]
                ),
            }
        if e_type == NVS_TYPE_BLOB_IDX:
            # layout: size:u32, chunk_count:u8, chunk_start:u8, _:u16
            size = struct.unpack("<I", entry[24:28])[0]
            count = entry[28]; start = entry[29]
            return {"index_size": size, "chunk_count": count, "chunk_start": start}
        return {"raw_type": f"0x{e_type:02x}", "data_hex": entry[24:32].hex()}
    except Exception as ex:
        return {"decode_error": str(ex)}


def parse_nvs(data: bytes, part_offset: int, part_size: int) -> dict[str, Any]:
    """Walk NVS pages, respect `span` so continuation slots don't get
    misinterpreted as entry headers (fixes the garbled keys from v1 parser).
    """
    entries_by_ns: dict[str, dict[str, Any]] = {}
    namespaces: dict[int, str] = {}

    # Two passes: first to collect namespace definitions, then real entries.
    for pass_num in (1, 2):
        for page_base in range(part_offset, part_offset + part_size, NVS_PAGE_SIZE):
            if page_base + NVS_PAGE_SIZE > len(data):
                break
            page = data[page_base:page_base + NVS_PAGE_SIZE]
            state = struct.unpack("<I", page[0:4])[0]
            if state not in (NVS_PAGE_STATE_ACTIVE, NVS_PAGE_STATE_FULL,
                             NVS_PAGE_STATE_FREEING):
                continue

            # Bitmap of used-entry slots (2 bits/entry, 32 bytes -> 126 entries).
            # Good to consult but not strictly required; we just walk + respect span.
            eidx = 0
            while eidx < NVS_PAGE_ENTRIES:
                e_off = NVS_PAGE_HEADER_SIZE + eidx * NVS_ENTRY_SIZE
                if e_off + NVS_ENTRY_SIZE > NVS_PAGE_SIZE:
                    break
                entry = page[e_off:e_off + NVS_ENTRY_SIZE]
                ns_id = entry[0]; e_type = entry[1]; span = entry[2]

                # Skip blank / erased slots
                if entry == b"\xff" * 32 or ns_id == 0xFF:
                    eidx += 1
                    continue

                if span == 0 or span > NVS_PAGE_ENTRIES:
                    span = 1  # pathological, proceed one-at-a-time

                key = entry[8:24].rstrip(b"\x00").decode("utf-8", "replace")
                # Keep only keys that look like valid NVS keys (printable ASCII)
                printable_key = all(32 <= b < 127 or b == 0 for b in entry[8:24])
                if not printable_key:
                    eidx += span
                    continue

                if pass_num == 1:
                    # Namespace mapping entries: ns_id==0 + type==U8 + value==ns_id
                    if ns_id == 0 and e_type == NVS_TYPE_U8 and key:
                        namespaces[entry[24]] = key
                    eidx += span
                    continue

                # Pass 2 - record real entries (skip ns defs which ns_id==0)
                if ns_id == 0 or not key:
                    eidx += span
                    continue

                ns_name = namespaces.get(ns_id, f"ns#{ns_id}")
                value = _decode_entry_value(entry, page, e_off, e_type)
                entries_by_ns.setdefault(ns_name, {})[key] = {
                    "type": TYPE_NAMES.get(e_type, f"0x{e_type:02x}"),
                    "value": value,
                }
                eidx += span

    return {"namespaces": namespaces, "entries": entries_by_ns}


# ---------------------------------------------------------------------------
# App-slot (rodata) analysis
# ---------------------------------------------------------------------------

ELRS_VERSION_RE = re.compile(rb"ExpressLRS v(\d+\.\d+\.\d+)")
MILELRS_VERSION_RE = re.compile(rb"MILELRS[_-]?v?(\d+)")
GIT_HASH_NEAR_RE = re.compile(rb"[0-9a-f]{6,8}")


def analyse_app_slot(data: bytes, name: str, offset: int, size: int) -> dict[str, Any]:
    """Pull version, target, git hash, embedded hardware.json / config json
    from one app partition's rodata.
    """
    sl = data[offset:offset + size]
    info: dict[str, Any] = {
        "partition": name, "offset_hex": f"0x{offset:08x}", "size": size,
    }

    # ELRS version + git hash (app0 style).
    # Two flavours of embedding:
    #   (a) literal "ExpressLRS v3.5.3" — older builds, not present here
    #   (b) length-prefixed "\x1e3.5.3" right after the git hash — newer builds;
    #       we find the hash first (8 hex chars followed by 0x00 0x1e) then
    #       read the next few bytes as the version string.
    m = ELRS_VERSION_RE.search(sl)
    if m:
        info["elrs_version"] = m.group(1).decode()
        near = sl[max(0, m.start() - 64):m.end() + 64]
        for gm in GIT_HASH_NEAR_RE.finditer(near):
            cand = gm.group(0).decode()
            if cand != info["elrs_version"].replace(".", ""):
                info["elrs_git_hash"] = cand
                break

    # Fallback pattern: length-prefixed version right after a git hash,
    # within ~48 bytes of the UNIFIED_... target token. Layout seen in
    # dump: <hash:ASCII 6-8> 00 <len:u8=0x1e> <ver:major.minor.patch> 00
    if "elrs_version" not in info:
        for target in (b"UNIFIED_ESP32C3_LR1121_RX", b"UNIFIED_ESP32C3_SX1280_RX"):
            ti = sl.find(target)
            if ti < 0:
                continue
            window = sl[ti:ti + 160]
            # scan for pattern: hash\0 <len> major.minor.patch \0
            for hm in re.finditer(rb"([0-9a-f]{6,8})\x00+[\x00-\x7f](\d+\.\d+\.\d+)\x00",
                                   window):
                info["elrs_git_hash"] = hm.group(1).decode()
                info["elrs_version"] = hm.group(2).decode()
                break
            if "elrs_version" in info:
                break

    # MILELRS version (app1 style)
    mm = MILELRS_VERSION_RE.search(sl)
    if mm:
        raw = mm.group(1).decode()
        if len(raw) == 3:  # v348 -> 3.48
            info["milelrs_version"] = f"{raw[0]}.{raw[1:]}"
        else:
            info["milelrs_version"] = raw
        near = sl[max(0, mm.start() - 64):mm.end() + 64]
        for gm in GIT_HASH_NEAR_RE.finditer(near):
            cand = gm.group(0).decode()
            if cand not in (raw,):
                info["milelrs_git_hash"] = cand
                break

    # Target token
    for key in (b"UNIFIED_ESP32C3_LR1121_RX",
                b"UNIFIED_ESP32C3_SX1280_RX",
                b"BAYCKRC_C3_DUAL_BAND_GEMINI_RX",
                b"ExpressLRS RX", b"MILELRS"):
        if key in sl:
            info.setdefault("target_tokens", []).append(key.decode())

    # Product string (surface-visible name)
    for pname in (b"BAYCKRC C3 900/2400 Dual Band 100mW Gemini RX",
                  b"BK DB 100 GRX"):
        if pname in sl:
            info.setdefault("product_strings", []).append(pname.decode())

    # Embedded JSON - find the widest {...} block that validates
    info["embedded_json"] = find_embedded_json(sl, file_offset=offset)

    # Build-user leak (MILELRS compiled from danko's pio cache)
    danko = re.search(rb"C:/Users/danko/[^\x00\xff]+\.cpp", sl)
    if danko:
        info["build_user_leak"] = danko.group(0).decode("utf-8", "replace")

    # ESP-IDF arduino framework marker
    fw = re.search(rb"framework-arduinoespressif32@\S+", sl)
    if fw:
        info["framework"] = fw.group(0).decode("utf-8", "replace")

    return info


def find_embedded_json(blob: bytes, file_offset: int = 0) -> list[dict[str, Any]]:
    """Scan for `{` ... `}` ranges that parse as JSON with one of several
    telltale keys. Small & fast because we only try candidates starting
    at `{` where the key appears within 100 bytes."""
    results: list[dict[str, Any]] = []
    telltales = [b'"serial_rx"', b'"binding_rate"', b'"flash-discriminator"',
                 b'"encryption_key"', b'"radio_miso"', b'"lua_name"',
                 b'"product_name"', b'"wifi-on-interval"']
    seen_starts = set()
    for tt in telltales:
        start = 0
        while True:
            i = blob.find(tt, start)
            if i < 0:
                break
            start = i + 1
            # Walk backward to find the enclosing `{`
            j = i
            while j > 0 and blob[j] != ord('{'):
                j -= 1
            if j in seen_starts:
                continue
            seen_starts.add(j)
            # Walk forward to find matching `}`
            depth = 0; end = -1
            for k in range(j, min(j + 8192, len(blob))):
                c = blob[k]
                if c == ord('{'):
                    depth += 1
                elif c == ord('}'):
                    depth -= 1
                    if depth == 0:
                        end = k + 1; break
            if end < 0:
                continue
            snippet = blob[j:end]
            try:
                obj = json.loads(snippet.decode("utf-8", "strict"))
                results.append({
                    "file_offset": f"0x{file_offset + j:08x}",
                    "length": end - j,
                    "value": obj,
                })
            except Exception:
                pass
    return results


# ---------------------------------------------------------------------------
# Incidental artefacts
# ---------------------------------------------------------------------------

ESP_OUIS = [b"\x24\x0a\xc4", b"\x24\xa1\x60", b"\x7c\x9e\xbd",
            b"\x84\xcc\xa8", b"\x94\xb9\x7e", b"\xac\x67\xb2",
            b"\xf4\xcf\xa2", b"\x08\x3a\xf2", b"\x48\x3f\xda",
            b"\x70\x04\x1d", b"\xec\xda\x3b", b"\xd8\xbf\xc0"]


def find_mac_addresses(data: bytes) -> list[dict[str, str]]:
    macs: dict[str, str] = {}
    for oui in ESP_OUIS:
        for m in re.finditer(re.escape(oui), data):
            addr = data[m.start():m.start() + 6]
            if len(addr) != 6:
                continue
            key = ":".join(f"{b:02x}" for b in addr)
            macs.setdefault(key, f"0x{m.start():08x}")
    return [{"mac": k, "at": v} for k, v in sorted(macs.items())]


def strings(data: bytes, min_len: int = 8) -> list[tuple[int, str]]:
    out: list[tuple[int, str]] = []
    pat = re.compile(rb"[\x20-\x7e]{%d,}" % min_len)
    for m in pat.finditer(data):
        out.append((m.start(), m.group(0).decode("ascii", "replace")))
    return out


INTERESTING_KEYWORDS = [
    "ExpressLRS", "MILELRS", "elrs_rx", "CRSF", "LR1121", "SX128",
    "Bayck", "BAYCKRC", "BK DB", "RUBIKON",
    "firmware", "git", "version",
    "binding", "bind_phrase", "UID", "uid",
    "encryption", "swarm", "ew_scanner", "vx_control", "custom_freq",
    "wifi-on", "hardware-version",
    "password", "passphrase", "ssid", "WPA", "secret",
    "http://", "https://",
]


def find_interesting_strings(all_strings: list[tuple[int, str]]) -> list[tuple[int, str]]:
    pat = re.compile("|".join(re.escape(k) for k in INTERESTING_KEYWORDS), re.IGNORECASE)
    seen: set[str] = set()
    out: list[tuple[int, str]] = []
    for off, s in all_strings:
        if pat.search(s) and s not in seen:
            seen.add(s)
            out.append((off, s))
    return out


# ---------------------------------------------------------------------------
# Report (Markdown)
# ---------------------------------------------------------------------------

def render_md(report: dict[str, Any]) -> str:
    lines: list[str] = []
    W = lines.append
    p = report["partitions"]
    W(f"# Firmware dump report - {Path(report['dump']).name}\n")
    W(f"- **Size:** {report['size']:,} bytes ({report['size']/1024/1024:.2f} MB)")
    W(f"- **MD5:** `{report.get('md5','?')}`")
    W(f"- **Partition table @** `0x{report['pt_offset']:x}`")
    W("")

    # ---- TL;DR summary box (what the user cares about) ----
    tldr: list[str] = []
    for app in report.get("apps", []):
        bits = [f"`{app['partition']}`"]
        if app.get("elrs_version"):
            bits.append(f"ELRS **{app['elrs_version']}** (git `{app.get('elrs_git_hash','?')}`)")
        if app.get("milelrs_version"):
            bits.append(f"MILELRS **v{app['milelrs_version']}** (git `{app.get('milelrs_git_hash','?')}`)")
        if app.get("target_tokens"):
            bits.append(f"target `{app['target_tokens'][0]}`")
        tldr.append("- " + " — ".join(bits))
    ota = report.get("otadata") or {}
    if ota.get("active_slot") is not None:
        tldr.append(f"- **Booting from:** `app{ota['active_slot']}` (by OTADATA seq)")
    # Walk all embedded JSONs and surface operator-facing config (brand / rate /
    # encryption_key / custom_freq / binding_phrase are the bytes a human
    # would actually act on).
    for app in report.get("apps", []):
        for jb in app.get("embedded_json", []):
            val = jb["value"]
            if isinstance(val, dict):
                ops = {k: v for k, v in val.items()
                       if k in ("brand", "binding_rate", "encryption_key",
                                "custom_freq", "custom_freq2", "domain",
                                "swarm_id", "bind_phrase")}
                if ops:
                    tldr.append(f"- **Operator config in `{app['partition']}`** "
                                f"@ `{jb['file_offset']}`:")
                    for k, v in ops.items():
                        tldr.append(f"  - `{k}`: `{v}`")
    creds = report.get("wifi_credentials", [])
    if creds:
        tldr.append(f"- **WiFi credentials:** {len(creds)} pair(s) (see section below)")
    if tldr:
        W("## TL;DR\n")
        W("\n".join(tldr))
        W("")

    W("## Partition table\n")
    W("| Name | Type / Subtype | Offset | Size | Range |")
    W("|------|----------------|--------|------|-------|")
    for part in p:
        W(f"| `{part['name']}` | {part['type_name']}/{part['subtype_name']} "
          f"| `0x{part['offset']:07x}` | {part['size']:,} B "
          f"| `{part['range_hex']}` |")
    W("")

    ota = report.get("otadata")
    if ota:
        W("## OTADATA - which app boots?\n")
        for s in ota["sectors"]:
            blank = " **(blank)**" if s["blank"] else ""
            W(f"- Sector {s['index']} @ `0x{s['offset']:x}`: "
              f"seq=`{s['seq']}`{'' if s['seq']!=0xFFFFFFFF else ' (0xFFFFFFFF = erased)'}, "
              f"state=`0x{s['state']:08x}`, crc=`0x{s['crc']:08x}`{blank}")
        if ota["active_slot"] is not None:
            W(f"- **Active slot:** {ota['active_slot']} -> `app{ota['active_slot']}`")
        if ota["note"]:
            W(f"- _Note:_ {ota['note']}")
        W("")

    for app in report.get("apps", []):
        W(f"## App slot - `{app['partition']}` @ `{app['offset_hex']}`\n")
        if "elrs_version" in app:
            W(f"- **ExpressLRS version:** `{app['elrs_version']}`"
              + (f"  (git `{app.get('elrs_git_hash','?')}`)"))
        if "milelrs_version" in app:
            W(f"- **MILELRS version:** `v{app['milelrs_version']}`"
              + (f"  (git `{app.get('milelrs_git_hash','?')}`)"))
        if app.get("target_tokens"):
            W(f"- **Targets found:** {', '.join(f'`{t}`' for t in app['target_tokens'])}")
        if app.get("product_strings"):
            W(f"- **Product strings:** {', '.join(f'`{s}`' for s in app['product_strings'])}")
        if app.get("framework"):
            W(f"- **Framework:** `{app['framework']}`")
        if app.get("build_user_leak"):
            W(f"- **Build-path leak:** `{app['build_user_leak']}`")
        if app.get("embedded_json"):
            W(f"\n### Embedded JSON blobs in `{app['partition']}`\n")
            for j, jb in enumerate(app["embedded_json"]):
                W(f"#### JSON #{j+1} @ `{jb['file_offset']}` ({jb['length']} bytes)\n")
                W("```json")
                W(json.dumps(jb["value"], indent=2, ensure_ascii=False))
                W("```\n")

    # NVS
    for nvs_report in report.get("nvs_reports", []):
        W(f"## NVS - partition `{nvs_report['partition']}`\n")
        if nvs_report["namespaces"]:
            W(f"Namespaces seen: {', '.join(f'`{v}`(#{k})' for k, v in sorted(nvs_report['namespaces'].items()))}")
            W("")
        for ns, kv in sorted(nvs_report["entries"].items()):
            if not kv:
                continue
            W(f"### `{ns}`\n")
            W("| Key | Type | Value |")
            W("|-----|------|-------|")
            for key, meta in sorted(kv.items()):
                val = meta["value"]
                if isinstance(val, dict):
                    vs = json.dumps(val, ensure_ascii=False)
                elif isinstance(val, str):
                    vs = repr(val)
                else:
                    vs = str(val)
                # Table-cell safety
                vs = vs.replace("|", r"\|").replace("\n", " ")
                if len(vs) > 120:
                    vs = vs[:117] + "..."
                W(f"| `{key}` | {meta['type']} | {vs} |")
            W("")

    # WiFi creds
    creds = report.get("wifi_credentials", [])
    W("## WiFi credentials found\n")
    if creds:
        W("| Role | Namespace | SSID | Password |")
        W("|------|-----------|------|----------|")
        for c in creds:
            W(f"| {c['role']} | `{c['namespace']}` | `{c['ssid']}` | `{c['password']}` |")
    else:
        W("_(none found - receiver likely never connected to a STA network)_")
    W("")

    macs = report.get("mac_candidates", [])
    if macs:
        W("## MAC-like byte sequences\n")
        W("Sequences starting with a known ESP/Espressif OUI - could be"
          " runtime config, MAC mirror or incidental bytes.")
        W("")
        W("| MAC | Found at |")
        W("|-----|----------|")
        for m in macs[:30]:
            W(f"| `{m['mac']}` | `{m['at']}` |")
        W("")

    inter = report.get("interesting_strings", [])
    if inter:
        W(f"## Interesting strings ({len(inter)} - filtered)\n")
        W("_Full string dump kept in the JSON report. Here are the matches"
          " for keywords like ExpressLRS / MILELRS / RUBIKON / encryption /"
          " UID / ssid / framework path._\n")
        W("| Offset | String |")
        W("|--------|--------|")
        for off, s in inter[:120]:
            ss = s.replace("|", r"\|").replace("\n", " ")
            if len(ss) > 140:
                ss = ss[:137] + "..."
            W(f"| `0x{off:08x}` | {ss} |")
        W("")

    return "\n".join(lines)


# ---------------------------------------------------------------------------

def find_wifi_credentials(nvs: dict[str, Any]) -> list[dict[str, str]]:
    creds: list[dict[str, str]] = []
    entries = nvs.get("entries", {})

    def _try(ns_key: str, ssid_k: str, pwd_k: str, role: str) -> None:
        ns = entries.get(ns_key, {})
        if ssid_k not in ns:
            return
        ssid = ns[ssid_k]["value"] if isinstance(ns[ssid_k], dict) else ns[ssid_k]
        pwd_meta = ns.get(pwd_k, {})
        pwd = pwd_meta["value"] if isinstance(pwd_meta, dict) else pwd_meta
        if isinstance(ssid, dict):  # blob
            ssid = str(ssid.get("ascii_preview", ""))
        if isinstance(pwd, dict):
            pwd = str(pwd.get("ascii_preview", ""))
        creds.append({"role": role, "namespace": ns_key,
                      "ssid": str(ssid), "password": str(pwd)})

    _try("nvs.net80211", "sta.ssid", "sta.pswd", "STA")
    _try("nvs.net80211", "ap.ssid", "ap.pswd", "AP")

    for ns_name, kv in entries.items():
        if "wifi" in ns_name.lower() or "wlan" in ns_name.lower():
            for k, meta in kv.items():
                if "ssid" in k.lower():
                    val = meta["value"] if isinstance(meta, dict) else meta
                    creds.append({"role": f"Other ({ns_name}.{k})",
                                  "namespace": ns_name,
                                  "ssid": str(val), "password": ""})
    return creds


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("dump", help="Path to raw flash dump (.bin)")
    ap.add_argument("--pt-offset", type=lambda x: int(x, 0),
                    default=PARTITION_TABLE_OFFSET,
                    help="Partition table offset (default 0x8000)")
    ap.add_argument("--md", help="Write Markdown report here (default: <dump>.parsed.md)")
    ap.add_argument("--json", dest="json_out",
                    help="Write full machine-readable report (default: <dump>.parsed.json)")
    args = ap.parse_args()

    dump_path = Path(args.dump)
    data = dump_path.read_bytes()
    print(f"[+] Loaded {len(data):,} bytes from {dump_path}")

    import hashlib
    md5 = hashlib.md5(data).hexdigest()

    report: dict[str, Any] = {
        "dump": str(dump_path),
        "size": len(data),
        "md5": md5,
        "pt_offset": args.pt_offset,
    }

    parts = parse_partition_table(data, args.pt_offset)
    if not parts:
        print(f"[!] No partition table at 0x{args.pt_offset:x}")
        return
    report["partitions"] = parts
    print(f"[+] Partition table - {len(parts)} entries")

    # OTADATA
    for p in parts:
        if p["type"] == 1 and p["subtype"] == 0:
            report["otadata"] = parse_otadata(data, p["offset"])
            ota = report["otadata"]
            if ota["active_slot"] is not None:
                print(f"[+] OTADATA -> active slot app{ota['active_slot']}")

    # App slots
    apps: list[dict[str, Any]] = []
    for p in parts:
        if p["type"] == 0:
            info = analyse_app_slot(data, p["name"], p["offset"], p["size"])
            apps.append(info)
            banner = []
            if "elrs_version" in info: banner.append(f"ELRS {info['elrs_version']}")
            if "milelrs_version" in info: banner.append(f"MILELRS v{info['milelrs_version']}")
            if info.get("target_tokens"): banner.append(info["target_tokens"][0])
            print(f"[+] {p['name']}: {' / '.join(banner) if banner else '(no signatures)'}")
            if info.get("embedded_json"):
                print(f"    embedded JSON blobs: {len(info['embedded_json'])}")
    report["apps"] = apps

    # NVS partitions
    nvs_reports: list[dict[str, Any]] = []
    for p in parts:
        if p["type"] == 1 and p["subtype"] == 2:
            print(f"[+] Parsing NVS {p['range_hex']}")
            r = parse_nvs(data, p["offset"], p["size"])
            nvs_reports.append({"partition": p["name"], **r})
    report["nvs_reports"] = nvs_reports

    # WiFi creds across all NVS
    all_creds: list[dict[str, str]] = []
    for nr in nvs_reports:
        all_creds.extend(find_wifi_credentials(nr))
    report["wifi_credentials"] = all_creds
    if all_creds:
        print(f"[+] WiFi credentials extracted: {len(all_creds)}")
        for c in all_creds:
            print(f"    [{c['role']}] ssid={c['ssid']!r} pass={c['password']!r}")
    else:
        print("[!] No WiFi credentials in NVS")

    # MACs
    macs = find_mac_addresses(data)
    report["mac_candidates"] = macs

    # Interesting strings
    ss = strings(data)
    inter = find_interesting_strings(ss)
    # Store as 2-tuples so the renderer can iterate them; JSON output
    # converts tuples to lists automatically.
    report["interesting_strings"] = [[o, s] for o, s in inter]
    print(f"[+] {len(ss):,} strings total, {len(inter)} interesting")

    # Write outputs
    md_path = Path(args.md) if args.md else dump_path.with_suffix(".parsed.md")
    md_path.write_text(render_md(report), encoding="utf-8")
    print(f"[+] Wrote {md_path}")

    json_path = Path(args.json_out) if args.json_out else dump_path.with_suffix(".parsed.json")
    # Convert tuples, bytes to serialisable
    json_path.write_text(
        json.dumps(report, indent=2, ensure_ascii=False, default=str),
        encoding="utf-8")
    print(f"[+] Wrote {json_path}")


if __name__ == "__main__":
    main()
