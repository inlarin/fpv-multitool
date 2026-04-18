"""Deep RE extraction on libdji_secure.so + TA2 (e91c9402-...).

Goals:
1. Extract .rodata from libdji_secure.so, look for:
   - Hardcoded ECC curve parameters (sect163r2 domain: a, b, Gx, Gy, order n)
   - Hardcoded public keys (21-byte X + 21-byte Y on B-163)
   - Cert templates with 'cc cc' placeholders
   - DER ASN.1 sequences
   - PEM markers / strings

2. Extract all strings (plain ASCII + UTF-16 for Windows-style binaries)
   from TA2 — look for key names, crypto constants, error messages.

3. Disassemble specific functions at known offsets (a1006_auth_verify,
   a1006_oneway_authentication) — look for magic constants immediately
   before conditional branches (likely compared-against values).

4. Look for ROM key derivation constants in TA2 — `SEC ROOT KEYV1`,
   `ACCESSORY AUTH`, `MODULE KEY` etc. Show byte patterns around them.

Output: extracted artifacts saved to research/artifacts/
"""
import os
import re
import sys
import struct
from pathlib import Path

FIRMWARE = Path(r"c:/Users/inlar/PycharmProjects/esp32/research/firmware_mavic3/V01.00.0600_wm260_dji_system/decrypted")
LIBSEC   = FIRMWARE / "m0802_unpacked/system_extracted/lib64/libdji_secure.so"
TA2      = FIRMWARE / "m0802_unpacked/ta_decrypted/e91c9402-64a0-470f-88e7bf5d3c606b6a_TZTA.bin"
TA1      = FIRMWARE / "m0802_unpacked/ta_decrypted/09db16c0-873b-4fed-b87ea5d2b86293a2_TZTA.bin"

OUT = Path(r"c:/Users/inlar/PycharmProjects/esp32/research/artifacts")
OUT.mkdir(parents=True, exist_ok=True)


# ===== Known sect163r2 parameters (from SEC 2: Recommended Elliptic Curve Domain Parameters) =====
# f(z) = z^163 + z^7 + z^6 + z^3 + 1  =  0x800000000000000000000000000000000000000C9
# a = 1
# b = 020a601907b8c953ca1481eb10512f78744a3205fd  (21 bytes)
# Gx = 03f0eba16286a2d57ea0991168d4994637e8343e36
# Gy = 00d51fbc6c71a0094fa2cdd545b11c5c0c797324f1
# n = order
SECT163R2 = {
    'field_poly': bytes.fromhex('0800000000000000000000000000000000000000C9'.rjust(22,'0').lower())[-22:],
    'a': bytes([0x00] * 20 + [0x01]),
    'b': bytes.fromhex('020a601907b8c953ca1481eb10512f78744a3205fd'),
    'Gx': bytes.fromhex('03f0eba16286a2d57ea0991168d4994637e8343e36'),
    'Gy': bytes.fromhex('00d51fbc6c71a0094fa2cdd545b11c5c0c797324f1'),
}


def read_bin(path):
    with open(path, 'rb') as f:
        return f.read()


def find_patterns(data, label, patterns):
    """Find byte patterns in data. Patterns = dict {name: bytes}. Returns list of (name, offset)."""
    hits = []
    for name, pat in patterns.items():
        off = 0
        while True:
            i = data.find(pat, off)
            if i < 0:
                break
            hits.append((name, i))
            off = i + 1
    print(f"  [{label}] {len(hits)} pattern hits: {[h[0] for h in hits[:10]]}{'...' if len(hits)>10 else ''}")
    return hits


def find_strings(data, min_len=6):
    """Find ASCII strings. Return list of (offset, string)."""
    pat = re.compile(rb'[\x20-\x7E]{%d,}' % min_len)
    return [(m.start(), m.group(0).decode('ascii', errors='replace')) for m in pat.finditer(data)]


def find_key_like_blobs(data):
    """Heuristic: 21-byte aligned blobs with high entropy (likely EC points or HMAC keys).
    For B-163 curve, public key = 1 + 21 + 21 = 43 bytes uncompressed, or 1 + 21 = 22 compressed.
    Private scalar = 21 bytes.
    Report any 20-21 byte block with byte diversity > 15 unique values."""
    hits = []
    for blen in (21, 20, 22, 32, 43, 44):
        for off in range(0, len(data) - blen, 4):
            blob = data[off:off+blen]
            unique = len(set(blob))
            if unique >= 15 and blob.count(0) < blen // 3 and blob.count(0xFF) < blen // 3:
                # High entropy, not padding
                hits.append((off, blen, blob.hex()))
                if len(hits) >= 200:
                    return hits
    return hits


def analyze_libdji_secure():
    print("=== libdji_secure.so analysis ===")
    data = read_bin(LIBSEC)
    print(f"  size: {len(data)} bytes")

    # Look for sect163r2 curve parameters
    print("\n-- Searching for sect163r2 curve parameters --")
    hits = find_patterns(data, "sect163r2", SECT163R2)
    with open(OUT / "libdji_curve_hits.txt", 'w') as f:
        for name, off in hits:
            f.write(f"{name} at offset 0x{off:06X}\n")
            # Dump 64 bytes around for context
            ctx = data[max(0, off-16):off+len(SECT163R2[name])+16]
            f.write(f"  context: {ctx.hex()}\n")

    # Look for OIDs
    print("\n-- Searching for crypto OIDs --")
    oids = {
        'id-ecPublicKey':    bytes.fromhex('2A8648CE3D0201'),  # 1.2.840.10045.2.1
        'ecdsa-with-SHA224': bytes.fromhex('2A8648CE3D040301'),  # 1.2.840.10045.4.3.1
        'sect163r2':          bytes.fromhex('2B8104000F'),       # 1.3.132.0.15
        'id-sha1':           bytes.fromhex('2B0E03021A'),       # 1.3.14.3.2.26
        'rsaEncryption':     bytes.fromhex('2A864886F70D010101'),
    }
    find_patterns(data, "OIDs", oids)

    # Extract PEM markers + crypto strings
    print("\n-- Extracting strings --")
    strs = find_strings(data, 8)
    crypto_strs = [s for off, s in strs if any(k in s.lower() for k in
        ['key', 'cert', 'ecc', 'ecdsa', 'sha', 'rsa', 'a1006', 'a100x', 'auth', 'signature',
         'curve', 'secret', 'root', 'cipher', 'aes', 'hmac', 'bind', 'accessory', 'sec'])]
    with open(OUT / "libdji_crypto_strings.txt", 'w', encoding='utf-8') as f:
        for s in sorted(set(crypto_strs))[:500]:
            f.write(s + '\n')
    print(f"  {len(crypto_strs)} crypto-related strings -> libdji_crypto_strings.txt")

    # Look for key-like blobs (high-entropy 20-22-43 byte blocks)
    print("\n-- Searching for EC point / key blobs --")
    blobs = find_key_like_blobs(data)
    print(f"  {len(blobs)} candidates (len=20-44, high entropy)")
    with open(OUT / "libdji_keyblob_candidates.txt", 'w') as f:
        f.write(f"# Top entropy blobs in libdji_secure.so\n")
        f.write(f"# Columns: offset, length, hex\n")
        # Sort by offset, write first 150
        for off, blen, hexstr in blobs[:150]:
            f.write(f"0x{off:06X}\t{blen}\t{hexstr}\n")

    # Try to find function at 0x3fe40 (a1006_auth_challenge) — look for what's nearby
    print("\n-- Context around known function offsets --")
    known = [
        (0x3e068, 'a100x_v1_challenge'),
        (0x3e298, 'a100x_verifyDERSig'),
        (0x3fe40, 'a1006_auth_challenge'),
        (0x40348, 'a1006_auth_verify'),
        (0x3f9f8, 'a1006_oneway_authentication'),
        (0x43840, 'auth_chip_init'),
        (0x44bb0, '.rodata start (from memory)'),
        (0x4ba48, 'X.509 cert template (from memory)'),
    ]
    with open(OUT / "libdji_function_context.txt", 'w') as f:
        for off, name in known:
            if off < len(data):
                ctx = data[off:off+64]
                f.write(f"=== {name} @ 0x{off:06X} ===\n")
                f.write(f"hex: {ctx.hex()}\n")
                f.write(f"ascii: {''.join(chr(b) if 0x20<=b<0x7F else '.' for b in ctx)}\n\n")

    # Extract .rodata — typically between text and data. Use simple heuristic:
    # ELF section headers would be ideal but let's do the reported offset 0x44bb0
    # for 42KB (from memory file)
    if 0x44bb0 + 0xA800 <= len(data):
        rodata = data[0x44bb0:0x44bb0 + 0xA800]
        with open(OUT / "libdji_rodata.bin", 'wb') as f:
            f.write(rodata)
        print(f"\n  Extracted .rodata (42 KB) -> libdji_rodata.bin")

        # Extract cert template at +0x26e98 relative = 0x4ba48 absolute
        cert_off = 0x4ba48 - 0x44bb0  # 0x6E98
        if cert_off + 260 <= len(rodata):
            cert_template = rodata[cert_off:cert_off+260]
            with open(OUT / "libdji_cert_template.bin", 'wb') as f:
                f.write(cert_template)
            print(f"  Extracted X.509 cert template (260B) -> libdji_cert_template.bin")
            print(f"  Template first 64 bytes: {cert_template[:64].hex()}")


def analyze_ta2():
    print("\n=== TA2 (e91c9402 — DJI Accessory/SEC Auth) analysis ===")
    if not TA2.exists():
        print(f"  MISSING: {TA2}")
        return
    data = read_bin(TA2)
    print(f"  size: {len(data)} bytes")

    # Key constant names (from memory notes)
    key_names = [
        b'SEC ROOT KEYV1', b'MODULE KEY', b'PKCS12 KEY', b'RTK ENC KEY',
        b'DJICARE KEY', b'SECURE BIND', b'ACCESSORY AUTH', b'ACCESSORYENCRYPT',
        b'SSD AUTH KEY', b'SSD KEY',
    ]
    print("\n-- Locations of key-name constants --")
    for kn in key_names:
        off = data.find(kn)
        if off >= 0:
            ctx_before = data[max(0,off-32):off]
            ctx_after  = data[off+len(kn):off+len(kn)+32]
            print(f"  '{kn.decode()}' at 0x{off:06X}")
            print(f"    32B before: {ctx_before.hex()}")
            print(f"    32B after:  {ctx_after.hex()}")

    # Extract all strings
    strs = find_strings(data, 6)
    with open(OUT / "ta2_strings.txt", 'w', encoding='utf-8') as f:
        for off, s in strs:
            f.write(f"0x{off:06X}\t{s}\n")
    print(f"\n  All {len(strs)} strings -> ta2_strings.txt")

    # Crypto-related function names
    crypto_strs = [s for off, s in strs if any(k in s.lower() for k in
        ['key', 'derive', 'verify', 'accessory', 'root', 'auth', 'cipher', 'sign',
         'rsa', 'ecc', 'aes', 'hmac', 'sha', 'cert', 'bind', 'inject'])]
    print(f"  {len(crypto_strs)} crypto-related -> first 30:")
    for s in crypto_strs[:30]:
        print(f"    {s}")

    # Look for TA2 crypto constants/roots. TA2 is HSTO format — body offset 0x2E0+
    # (HSTO header is ~700 bytes). Look for high-entropy blobs in body.
    print("\n-- High-entropy blobs in TA2 body (potential keys/roots) --")
    blobs = find_key_like_blobs(data[0x2E0:])
    with open(OUT / "ta2_keyblob_candidates.txt", 'w') as f:
        f.write(f"# High-entropy blobs in TA2 body, adjusted to body offset (-0x2E0)\n")
        for off, blen, hexstr in blobs[:100]:
            f.write(f"0x{off+0x2E0:06X}\t{blen}\t{hexstr}\n")
    print(f"  {len(blobs)} candidates -> ta2_keyblob_candidates.txt")


def analyze_ta1():
    print("\n=== TA1 (09db16c0 — Key Manager / Firmware Verifier) analysis ===")
    if not TA1.exists():
        print(f"  MISSING: {TA1}")
        return
    data = read_bin(TA1)
    print(f"  size: {len(data)} bytes")
    strs = find_strings(data, 6)
    with open(OUT / "ta1_strings.txt", 'w', encoding='utf-8') as f:
        for off, s in strs:
            f.write(f"0x{off:06X}\t{s}\n")
    crypto_strs = [s for off, s in strs if any(k in s.lower() for k in
        ['key', 'derive', 'verify', 'inject', 'sign', 'rsa', 'ecc', 'aes', 'sha', 'cert'])]
    print(f"  {len(crypto_strs)} crypto-related -> first 25:")
    for s in crypto_strs[:25]:
        print(f"    {s}")


if __name__ == '__main__':
    print(f"Output dir: {OUT}")
    analyze_libdji_secure()
    analyze_ta2()
    analyze_ta1()
    print(f"\nDone. See files in {OUT}")
