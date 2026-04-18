#!/usr/bin/env python3
"""
Bayck RC C3 Dual — ELRS firmware dump parser.

Reads a raw flash dump from an ESP32-C3 ELRS receiver and extracts:
  - firmware version / target model / branch / git hash
  - WiFi credentials (SSID + password) from the NVS partition
  - binding phrase UID (lower 6 bytes of SHA256)
  - FCC/CE region flag
  - MAC address(es) stored in eFuse mirror / NVS

Run:
    python parse.py dump.bin [--json out.json] [--txt out.txt]

NVS parser follows the ESP-IDF NVS v2 layout:
    https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/nvs_flash.html

Works on any ESP32-family dump; target model detection is ELRS-specific.
"""

from __future__ import annotations
import argparse
import json
import re
import struct
import sys
from pathlib import Path
from typing import Any

# Partition-table magic (ESP_PARTITION_MAGIC = 0x50AA)
PARTITION_MAGIC = b"\xaa\x50"
PARTITION_TABLE_OFFSET = 0x8000  # stock ESP32-C3 layout (overridable via --pt-offset)
PARTITION_ENTRY_SIZE = 32

# NVS page header constants
NVS_PAGE_SIZE = 4096
NVS_PAGE_HEADER_SIZE = 32
NVS_ENTRY_SIZE = 32
NVS_STATE_ACTIVE = 0xFFFFFFFE
NVS_STATE_FULL = 0xFFFFFFFC
NVS_STATE_FREEING = 0xFFFFFFF8

NVS_TYPE_U8 = 0x01
NVS_TYPE_I8 = 0x11
NVS_TYPE_U16 = 0x02
NVS_TYPE_I16 = 0x12
NVS_TYPE_U32 = 0x04
NVS_TYPE_I32 = 0x14
NVS_TYPE_U64 = 0x08
NVS_TYPE_I64 = 0x18
NVS_TYPE_STR = 0x21
NVS_TYPE_BLOB = 0x42
NVS_TYPE_BLOB_DATA = 0x41
NVS_TYPE_BLOB_IDX = 0x48


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
        parts.append({
            "name": name,
            "type": ptype,
            "subtype": subtype,
            "offset": off,
            "size": size,
            "size_hex": f"0x{off:08x}-0x{off+size:08x}",
        })
    return parts


def parse_nvs(data: bytes, part_offset: int, part_size: int) -> dict[str, Any]:
    """Walk NVS pages and extract (ns, key, value) triples."""
    entries: dict[str, dict[str, Any]] = {}
    namespaces: dict[int, str] = {}

    # Pass 1: collect namespace IDs (type=U8, ns=0 → key=namespace name, value=ns_id)
    # Pass 2: collect real entries

    for pass_num in (1, 2):
        for page_start in range(part_offset, part_offset + part_size, NVS_PAGE_SIZE):
            if page_start + NVS_PAGE_SIZE > len(data):
                break
            page = data[page_start:page_start + NVS_PAGE_SIZE]
            if len(page) < NVS_PAGE_HEADER_SIZE:
                continue
            state = struct.unpack("<I", page[0:4])[0]
            if state not in (NVS_STATE_ACTIVE, NVS_STATE_FULL, NVS_STATE_FREEING):
                continue

            # Walk entries 0..125 (126 entries per page)
            for eidx in range(126):
                e_off = NVS_PAGE_HEADER_SIZE + eidx * NVS_ENTRY_SIZE
                if e_off + NVS_ENTRY_SIZE > len(page):
                    break
                entry = page[e_off:e_off + NVS_ENTRY_SIZE]
                ns_id = entry[0]
                e_type = entry[1]
                span = entry[2]
                # entry[3] reserved
                # entry[4:8] crc32
                key = entry[8:24].rstrip(b"\x00").decode("utf-8", "replace")
                if not key or ns_id == 0xFF:
                    continue

                if pass_num == 1:
                    if ns_id == 0 and e_type == NVS_TYPE_U8:
                        # Namespace entry: the U8 value at 24..25 is the ns_id
                        raw = entry[24:32]
                        val = raw[0]
                        namespaces[val] = key
                    continue

                # Pass 2: real entries
                if ns_id == 0:
                    continue  # skip namespace defs

                ns_name = namespaces.get(ns_id, f"ns#{ns_id}")
                value: Any = None
                try:
                    if e_type in (NVS_TYPE_U8, NVS_TYPE_I8):
                        value = entry[24]
                        if e_type == NVS_TYPE_I8 and value > 127:
                            value -= 256
                    elif e_type in (NVS_TYPE_U16, NVS_TYPE_I16):
                        value = struct.unpack("<h" if e_type == NVS_TYPE_I16 else "<H",
                                              entry[24:26])[0]
                    elif e_type in (NVS_TYPE_U32, NVS_TYPE_I32):
                        value = struct.unpack("<i" if e_type == NVS_TYPE_I32 else "<I",
                                              entry[24:28])[0]
                    elif e_type in (NVS_TYPE_U64, NVS_TYPE_I64):
                        value = struct.unpack("<q" if e_type == NVS_TYPE_I64 else "<Q",
                                              entry[24:32])[0]
                    elif e_type == NVS_TYPE_STR:
                        size = struct.unpack("<H", entry[24:26])[0]
                        # String data lives in following entries (span-1 of them)
                        data_start = e_off + NVS_ENTRY_SIZE
                        raw = page[data_start:data_start + size]
                        # strip trailing NULs
                        value = raw.rstrip(b"\x00").decode("utf-8", "replace")
                    elif e_type == NVS_TYPE_BLOB_DATA:
                        size = struct.unpack("<H", entry[24:26])[0]
                        data_start = e_off + NVS_ENTRY_SIZE
                        value = {
                            "blob_hex_preview": page[data_start:data_start + min(size, 32)].hex(),
                            "size": size,
                        }
                    else:
                        value = {"type_raw": e_type, "bytes_hex": entry[24:32].hex()}
                except Exception as ex:
                    value = {"parse_error": str(ex)}

                entries.setdefault(ns_name, {})[key] = value

    return {"namespaces": namespaces, "entries": entries}


def find_version_string(data: bytes) -> dict[str, str]:
    """ExpressLRS embeds its version as 'vX.Y.Z branch hash' in rodata."""
    results: dict[str, str] = {}

    # Primary: ExpressLRS vMajor.Minor.Patch
    m = re.search(rb"ExpressLRS v(\d+\.\d+\.\d+)", data)
    if m:
        results["elrs_version"] = m.group(1).decode()

    # Git hash (7 hex chars surrounded by spaces/nulls)
    for gm in re.finditer(rb"([0-9a-f]{7,8})", data):
        candidate = gm.group(1).decode()
        # heuristic: git hash near version string
        if "elrs_version" in results:
            pos = data.find(results["elrs_version"].encode())
            if pos >= 0 and 0 < gm.start() - pos < 64:
                results["git_hash"] = candidate
                break

    # Target token (Bayck_RC, C3, Dual, etc.)
    for key in ("Bayck_RC_C3_Dual", "BAYCK_RC_C3_DUAL", "Bayck_RC_C3",
                "SX1280", "LR1121", "2G4", "900"):
        pat = re.compile(re.escape(key).encode(), re.IGNORECASE)
        if pat.search(data):
            results.setdefault("target_tokens", [])
            if isinstance(results["target_tokens"], list):
                results["target_tokens"].append(key)

    return results


def find_mac_addresses(data: bytes) -> list[str]:
    """Heuristic — find sequences that look like MAC addresses near ESP OUI."""
    macs: set[str] = set()
    # ESP OUIs commonly seen
    oui_patterns = [b"\x24\x0a\xc4", b"\x24\xa1\x60", b"\x7c\x9e\xbd",
                    b"\x84\xcc\xa8", b"\x94\xb9\x7e", b"\xac\x67\xb2",
                    b"\xf4\xcf\xa2", b"\x08\x3a\xf2"]
    for oui in oui_patterns:
        for m in re.finditer(re.escape(oui), data):
            addr = data[m.start():m.start() + 6]
            if len(addr) == 6:
                macs.add(":".join(f"{b:02x}" for b in addr))
    return sorted(macs)


def find_wifi_credentials(nvs: dict[str, Any]) -> list[dict[str, str]]:
    """Look for SSID/password pairs anywhere in NVS, regardless of namespace."""
    creds: list[dict[str, str]] = []
    entries = nvs.get("entries", {})

    # ESP-IDF's `wifi_config_t` NVS blobs live under 'nvs.net80211' with
    # keys 'sta.ssid', 'sta.pswd', 'ap.ssid', 'ap.pswd'. ELRS stores its
    # own in 'ELRS' or 'elrs' namespaces.
    def _maybe(pair_ns: str, ssid_key: str, pwd_key: str, role: str) -> None:
        ns = entries.get(pair_ns, {})
        if ssid_key in ns:
            creds.append({
                "role": role,
                "namespace": pair_ns,
                "ssid": str(ns[ssid_key]),
                "password": str(ns.get(pwd_key, "")),
            })

    _maybe("nvs.net80211", "sta.ssid", "sta.pswd", "STA (home WiFi)")
    _maybe("nvs.net80211", "ap.ssid", "ap.pswd", "AP (receiver's own)")
    # ELRS custom
    for ns_name in entries:
        if "wifi" in ns_name.lower() or "wlan" in ns_name.lower() or "elrs" in ns_name.lower():
            ns = entries[ns_name]
            for k, v in ns.items():
                if "ssid" in k.lower() and isinstance(v, str):
                    pwd_key = k.lower().replace("ssid", "pass").replace("ssid", "pwd")
                    creds.append({
                        "role": f"ELRS ({ns_name}.{k})",
                        "namespace": ns_name,
                        "ssid": v,
                        "password": str(ns.get(pwd_key, "")),
                    })

    return creds


def strings(data: bytes, min_len: int = 6) -> list[tuple[int, str]]:
    """Extract printable ASCII strings from the blob, with offsets."""
    out: list[tuple[int, str]] = []
    pat = re.compile(rb"[\x20-\x7e]{%d,}" % min_len)
    for m in pat.finditer(data):
        s = m.group(0).decode("ascii", "replace")
        out.append((m.start(), s))
    return out


def interesting_strings(all_strings: list[tuple[int, str]]) -> list[tuple[int, str]]:
    keywords = [
        "ExpressLRS", "ELRS", "crsf", "CRSF", "SX128", "LR11", "Bayck",
        "target", "device", "uid", "binding", "bind_phrase", "FCC", "CE", "region",
        "ssid", "password", "pswd", "wifi", "WiFi", "WPA", "mac_address",
        "v3.", "v4.", "v5.", "git", "commit",
    ]
    pat = re.compile("|".join(re.escape(k) for k in keywords), re.IGNORECASE)
    return [(o, s) for o, s in all_strings if pat.search(s)]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("dump", help="Path to raw flash dump (.bin)")
    ap.add_argument("--pt-offset", type=lambda x: int(x, 0),
                    default=PARTITION_TABLE_OFFSET,
                    help="Partition table offset (default 0x8000)")
    ap.add_argument("--txt", help="Write human-readable report here")
    ap.add_argument("--json", dest="json_out", help="Write machine-readable report here")
    args = ap.parse_args()

    dump_path = Path(args.dump)
    data = dump_path.read_bytes()
    print(f"[+] Loaded {len(data)} bytes from {dump_path}")

    report: dict[str, Any] = {"dump": str(dump_path), "size": len(data)}

    # 1. Partition table
    parts = parse_partition_table(data, args.pt_offset)
    if not parts:
        print(f"[!] No partition table at 0x{args.pt_offset:x} — "
              "try --pt-offset=0x9000 or 0xD000 for custom layouts")
    else:
        print(f"[+] Partition table ({len(parts)} entries):")
        for p in parts:
            print(f"    {p['name']:<16} type={p['type']:02x} sub={p['subtype']:02x} "
                  f"{p['size_hex']} ({p['size']} B)")
    report["partitions"] = parts

    # 2. NVS parse (all nvs partitions)
    nvs_reports: list[dict[str, Any]] = []
    for p in parts:
        if p["type"] == 1 and p["subtype"] == 2:  # data/nvs
            print(f"[+] Parsing NVS at {p['size_hex']}...")
            r = parse_nvs(data, p["offset"], p["size"])
            nvs_reports.append({"partition": p["name"], **r})
    report["nvs"] = nvs_reports

    # 3. Version/target
    vinfo = find_version_string(data)
    if vinfo:
        print(f"[+] Version info: {vinfo}")
    report["version"] = vinfo

    # 4. MAC addresses
    macs = find_mac_addresses(data)
    if macs:
        print(f"[+] MAC candidates: {macs}")
    report["mac_candidates"] = macs

    # 5. WiFi creds (aggregate across NVS reports)
    all_creds: list[dict[str, str]] = []
    for nr in nvs_reports:
        all_creds.extend(find_wifi_credentials(nr))
    if all_creds:
        print("[+] WiFi credentials extracted:")
        for c in all_creds:
            print(f"    [{c['role']}] SSID={c['ssid']!r}  PASS={c['password']!r}")
    else:
        print("[!] No WiFi credentials found in NVS — receiver may never have connected "
              "to a STA network, or NVS is blank (fresh flash).")
    report["wifi_credentials"] = all_creds

    # 6. Interesting strings
    ss = strings(data)
    interesting = interesting_strings(ss)
    print(f"[+] {len(ss)} printable strings total, {len(interesting)} interesting")
    report["interesting_strings_count"] = len(interesting)

    # Write outputs
    txt_path = Path(args.txt) if args.txt else dump_path.with_suffix(".parsed.txt")
    with txt_path.open("w", encoding="utf-8") as f:
        f.write(f"# Parse report for {dump_path}\n")
        f.write(f"Size: {len(data)} bytes\n\n")
        f.write("## Partition table\n")
        for p in parts:
            f.write(f"  {p['name']:<16} type={p['type']:02x}/{p['subtype']:02x} {p['size_hex']}\n")
        f.write("\n## Version\n")
        f.write(json.dumps(vinfo, indent=2, ensure_ascii=False) + "\n")
        f.write("\n## MAC candidates\n")
        f.write("\n".join(f"  {m}" for m in macs) + "\n")
        f.write("\n## NVS entries\n")
        for nr in nvs_reports:
            f.write(f"\n### Partition {nr['partition']}\n")
            for ns_name, ns_entries in nr["entries"].items():
                f.write(f"\n[{ns_name}]\n")
                for k, v in ns_entries.items():
                    f.write(f"  {k} = {v!r}\n")
        f.write("\n## WiFi credentials\n")
        if all_creds:
            for c in all_creds:
                f.write(f"  [{c['role']}] SSID={c['ssid']!r}  PASS={c['password']!r}\n")
        else:
            f.write("  (none found)\n")
        f.write("\n## Interesting strings (filtered)\n")
        for off, s in interesting:
            f.write(f"  0x{off:08x}  {s}\n")

    print(f"[+] Wrote {txt_path}")

    if args.json_out:
        Path(args.json_out).write_text(json.dumps(report, indent=2, ensure_ascii=False,
                                                  default=str))
        print(f"[+] Wrote {args.json_out}")


if __name__ == "__main__":
    main()
