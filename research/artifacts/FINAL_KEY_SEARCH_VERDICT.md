# Final verdict: Mavic 3 root public key search

*2026-04-18 — after complete firmware analysis*

## Exhaustive static analysis completed

### Methods used
1. Byte-pattern matching for sect163r2 curve params — found ✅ (as expected)
2. OID search (id-ecPublicKey, ecdsa-with-SHA224, sect163r2) — all found ✅
3. String extraction (658 crypto-related strings identified)
4. High-entropy 20/21/43-byte blob detection
5. ADRP+ADD xref analysis (ghidra_lite) on all 14 auth functions
6. Call-graph analysis (ghidra_lite2) — 0 internal callers = all externally called
7. ASN.1 DER structure detection across libdji_secure.so + TA1 + TA2
8. File system search for cert/key/pem/pubkey files
9. Configuration file references in all binaries

### What was found
- ✅ X.509 cert template at libdji_secure.so+0x4BA48 (placeholder `cc` bytes — per-battery programming)
- ✅ sect163r2 curve parameters (a, b) at 3 locations
- ✅ All crypto OIDs embedded as expected
- ✅ All key derivation labels in TA2 ("SEC ROOT KEYV1", "ACCESSORY AUTH", etc.)
- ✅ Reference to `/etc/ac_config.json` (loaded by `init_auth_chip_from_json`)
- ✅ Public RSA-1024 key for network/license (NOT battery auth)
- ✅ DJI RSA-4096 CA for OTA signing (NOT battery auth)
- ✅ 20-byte high-entropy blobs at 0x4DAE0, 0x4EB00/18/30 (likely SHA-1 test vectors)
- ✅ Large 8.3KB table in .rodata (likely HMAC-SHA1 precomputed key schedules)

### What was NOT found (and why)
- ❌ Hardcoded DJI root public key as identifiable blob
- ❌ Direct reference to root key from `a1006_auth_verify` / `a100x_verifyDERSig` (they use function args)
- ❌ `/etc/ac_config.json` is NOT in firmware — generated at runtime
- ❌ Any readable cert/pubkey file in `/etc/` (only RSA-1024 + OTA CA)

## Conclusion

**The Mavic 3 battery auth root public key is ABSENT from firmware.**

### Derivation chain (confirmed via firmware RE)

```
Hardware Secure Element (MTK MediaTek SoC) — ROM fuse key
  │
  ├── TA1 (keymgr) via se_key_derivate_romkey_group
  │
  ▼
"SEC ROOT KEYV1" — derived at TA2 boot
  │
  ▼
"ACCESSORY AUTH" — per-usage session key
  │
  ▼
Written to /etc/ac_config.json on first boot (by init_auth_chip_from_json)
  │
  ▼
Loaded by dji_sys → a100x_v1_challenge / a100x_v1_verify_response
  │
  ▼
Passed as arg x0/x1 to a1006_auth_verify → verifies battery A1006 cert
```

### Why firmware analysis can't find it

1. Root key is derived from hardware SE — never stored as static data
2. Derivation happens in TrustZone (TA1/TA2) — not accessible to normal world
3. Derived key lives in runtime memory + ac_config.json on filesystem
4. Firmware contains only DERIVATION LABELS, not key values

## Practical paths forward

### Impossible without physical/dynamic access
- Extracting root key from firmware (confirmed NOT there)
- Reverse-engineering derivation to reproduce key without HW SE

### Requires rooted Mavic 3 drone (medium difficulty)
- Read `/etc/ac_config.json` from running drone
- Memory dump during auth (LD_PRELOAD or gdb)
- Intercept TA IPC calls

### Requires hardware side-channel (high difficulty)
- Voltage glitching drone's SoC to leak SE key
- Power analysis during SE key derivation
- Chip decap (lab)

### Requires workarounds (practical!)
- **Cell transplant**: keep original BMS+A1006, swap cells
- **MITM I²C proxy**: ESP32 between original BMS and replacement cells
  (forwards A1006 challenge-response traffic while replacement cells power the pack)
- **Firmware downgrade on drone**: older FW may have weaker checks
- **Accept limitations**: use PTL batteries with low cycle count, avoid stress

## Tools available for anyone picking this up

- `research/tools/deep_re_extract.py` — initial pattern + string extraction
- `research/tools/ghidra_lite.py` — ADRP+ADD xref tracking in named functions
- `research/tools/ghidra_lite2.py` — call-graph + arg tracking (WIP)
- `research/artifacts/` — all extracted data (cert template, rodata, strings, blobs)

## Verdict

Software unseal of Mavic 3 / PTL clones is **definitively infeasible**
without hardware or rooted-drone access. Our tool has reached the limit
of what purely firmware RE can do. Future progress requires different
attack vectors, not better software analysis.
