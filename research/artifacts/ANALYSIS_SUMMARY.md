# Deep RE analysis findings — libdji_secure.so + TA1 + TA2

*Generated 2026-04-18 by `research/tools/deep_re_extract.py`*

## libdji_secure.so (398 KB, ARM64)

### Confirmed present
- **Curve parameters for sect163r2** (a, b, Gx, Gy) at three distinct offsets
  — confirms DJI uses B-163 binary field ECC as reported in memory
- **OIDs embedded**: id-ecPublicKey, ecdsa-with-SHA224, sect163r2, id-sha1,
  rsaEncryption (confirms dual ECC+RSA use)
- **X.509 cert template** (260 bytes) at 0x4BA48 — parsed:
  - Outer SEQUENCE: 256B
  - Version 3, serial placeholder (8B `cc cc...`)
  - Signature algo: ecdsa-with-SHA224
  - Validity: 1970-01-01 → 9999-12-31 (never expires)
  - Subject: 3 RDNs (uniqueID 8B, O= 10B, CN= 12B — all placeholder)
  - SubjectPublicKeyInfo: `id-ecPublicKey` + `sect163r2` OID +
    BIT STRING(44B) = `00 04 cc × 42` = uncompressed EC point placeholder (21X + 21Y)
  - Signature: BIT STRING(20B) = `00 cc × 19` placeholder
- **658 crypto-related strings** → `libdji_crypto_strings.txt`

### NOT found (disappointingly)
- No hardcoded DJI root public key visible as simple 43-byte `04 + 42B`
  EC point pattern (false positives are ARM64 code)
- No hardcoded AES/HMAC keys beyond debug/test patterns
- Cert template fields are ALL placeholder — real keys programmed per-device

### Implication
The DJI root public key (used to verify A1006 device certs) is either:
1. In TA2 trusted app, accessed only from TrustZone runtime
2. Derived at boot from hardware SE ROM/fuse (most likely)
3. Present but requires ARM64 xref analysis (Ghidra) to locate via LDR instructions
   pointing into the .rodata section

## TA2 e91c9402-... (238 KB, HSTO format)

### Confirmed present
- **All expected key-name string constants** (derivation labels):
  - `SEC ROOT KEYV1` @ 0x031360
  - `MODULE KEY` @ 0x031480
  - `PKCS12 KEY` @ 0x031500
  - `RTK ENC KEY` @ 0x031540
  - `DJICARE KEY` @ 0x031618
  - `SECURE BIND` @ 0x0316C8
  - `ACCESSORY AUTH` @ 0x031828 ← **battery-related**
  - `ACCESSORY ACTIVATE` @ 0x031818
  - `ACCESSORYENCRYPT` @ 0x031838
  - `SSD AUTH KEY` @ 0x031948 / `SSD KEY` @ 0x031960
- **230 crypto-related strings** incl. `dji_derive_accessory_auth_activate_key`,
  `dji_verify_accessory_auth_msg`, `dji_check_accessory_key_inject_correctness`
- **16 bytes between SSD KEY and decrypt_ssd_root_key labels** at 0x031968:
  `d5 21 69 77 60 63 ea 80 cd a9 ba 2e e5 83 dd db`
  — Could be hardcoded SSD decryption key or padding. Not accessory-related.
  Worth checking against known DJI SSD implementations.

### NOT found
- No hardcoded 32B/48B/64B crypto keys adjacent to derivation labels
- All key NAMES present, key VALUES derived at runtime via hardware SE

### Implication
Key derivation chain is exactly as described in memory file:
```
HW SE ROM/fuse → "SEC ROOT KEYV1" derivation → "ACCESSORY AUTH" derivation → verify NXP A1006 cert
```

Root key physically unextractable from firmware alone.

## TA1 09db16c0-... (203 KB, HSTO format)

### Confirmed
- **112 crypto-related strings** — this TA is the Key Manager / Firmware Verifier
- Functions: `keymgr_get_prak`, `TA_KeyDerivation`, `dji_fw_verify_*`,
  `se_key_derivate_{prokey,romkey}_group` (confirms hardware SE key derivation)
- `inject_key_from_keyrepo` — suggests a key repository concept, all derived from ROM

## Actionable takeaways

1. **Software unseal of PTL/DJI Mavic 3 remains infeasible.**
   No hardcoded keys extracted. Runtime derivation requires TrustZone access.

2. **Our tool's current scope is complete for software research.**
   Moving forward on the cryptographic path requires either:
   - Hardware attack on drone SoC (glitch SE key extraction)
   - Hardware attack on battery (glitch A1006 or chip swap)
   - Access to DJI manufacturing keys (unrealistic)

3. **Interesting secondary findings** (for followup if anyone cares):
   - Mysterious 16 bytes at TA2+0x031968 — may be hardcoded SSD key
   - Cert template structure confirms sect163r2 is FIXED for all Mavic 3 batteries
   - TA2 contains full "ACCESSORY" flow code but values come from hardware

## Artifacts in this directory
- `libdji_cert_template.bin` (260B) — full X.509 template
- `libdji_rodata.bin` (42 KB) — extracted .rodata
- `libdji_crypto_strings.txt` — filtered crypto strings (658)
- `libdji_curve_hits.txt` — sect163r2 parameter locations
- `libdji_function_context.txt` — 64B dumps at known offsets
- `libdji_keyblob_candidates.txt` — high-entropy 20-44B blobs
- `ta1_strings.txt`, `ta2_strings.txt` — full string dumps
- `ta2_keyblob_candidates.txt` — TA2 high-entropy blobs

## For anyone with Ghidra/IDA
Priority xref targets (from `libdji_secure.so`):
- `a100x_verifyDERSig` @ 0x3e298 — traces to LDR → data section → find root pubkey
- `a1006_auth_verify` @ 0x40348 — entry to accessory cert verify, likely loads pubkey
- `auth_chip_init` @ 0x43840 — initialization, may reference stored cert/key

In TA2:
- `dji_verify_accessory_auth_msg` — xref back to derivation function
- `dji_derive_accessory_auth_activate_key` — shows key material flow
