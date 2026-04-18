# Deep xref analysis of libdji_secure.so — Python "Ghidra-lite"

*Generated 2026-04-18 via `research/tools/ghidra_lite.py` (capstone + pyelftools)*

Approach: instead of manual Ghidra clicking, scripted ARM64 ADRP+ADD
pair detection in known auth functions, then follow addresses into .rodata
to find referenced constants.

## What was found

### Found data references
- `a100x_generateChallenge` @ 0x3D730 references 3 addresses in .rodata:
  `0x4DAE0`, `0x4EB00`, `0x4EB18`
- `a100x_verifyResponse` @ 0x3D950 references 1: `0x4EB30`
- `bECC_verifyResponse` @ 0x3F440 references `0x4817A` (error string)
- `auth_chip_init` @ 0x43840 references `0x48647` (error string)
- `a1006_oneway_authentication` @ 0x3F9F8 references `0x6F018` (in .data)

### NOT found
- Zero direct data refs found in:
  - `a1006_auth_verify` @ 0x40348 ← the critical battery auth entry
  - `a100x_verifyDERSig` @ 0x3E298 ← ECDSA DER signature verify
  - `a1006_auth_challenge` @ 0x3FE40
  - Several others

These functions access data via **indirect pointers** (struct members
passed in argument registers x0, x1, etc.). The actual root public key
is passed IN via function argument — coming from the caller, most likely
from `auth_chip_init` or `init_auth_chip_from_json`.

So the key material isn't directly referenced in `a1006_auth_verify` —
it's an argument. Tracing further requires:
1. Finding callers of `a1006_auth_verify`
2. Tracking where they get the pubkey pointer from
3. Following back to either load from storage, hardcoded table, or TA boundary

## Large lookup table at 0x4CAC0-0x4EB3F (~8.3 KB)

Python analysis showed a table of 24-byte records containing:
- 4-byte values (either small 0-7 indexes or full 32-bit data)
- 20-byte high-entropy blobs

Referenced by `a100x_generateChallenge` and `a100x_verifyResponse`.

### Size-based candidates for what it is

Size 20 bytes aligns with:
- **SHA-1 hash output** (20 bytes) — precomputed test vectors?
- **HMAC-SHA1 key** (20 bytes) — key material?
- **ECDSA signature half** (for sect163r2: r or s = 21 bytes, doesn't fit)

Size 24 bytes per record doesn't cleanly match:
- Standard crypto primitive sizes
- sect163r2 coordinates (21 bytes)
- P-256 coordinates (32 bytes)

### What this rules out
- Not precomputed `kG` multiples for sect163r2 (wrong size)
- Not a P-256 or stronger curve (wrong size)
- Not pure SHA-256 tables (32-byte entries expected)

### Most likely explanation
**Precomputed constants / test vectors for internal self-test**, OR
**HMAC-SHA1 key schedule precomputations** used by the challenge
generator for randomness seeding.

If it were hardcoded public keys or shared secrets, Ghidra xref would
show auth functions reading entries by index. They don't — only the
challenge generator reads from here.

## Conclusion for battery research

The scripted analysis confirms what manual Ghidra work would also show:

1. **The DJI root public key is NOT easily findable as a 43-byte blob** in libdji_secure.so's .rodata
2. **Auth functions receive keys by pointer argument**, which means the
   key is passed in — likely allocated at boot from the TA/SE root chain
3. **The large 8.3KB table is probably SHA-1 related**, not directly the key

**Cracking the key would require one of:**
- Full Ghidra with xref from `auth_chip_init`'s callers to trace where
  the key pointer origin is
- Dynamic analysis (intercept the call at runtime with a debugger,
  requires rooting the drone)
- Hardware side-channel attacks on the drone SoC Secure Element

## Tools produced

- `research/tools/deep_re_extract.py` — string/pattern/blob extraction
- `research/tools/ghidra_lite.py` — ARM64 ADRP+ADD+LDR xref analyzer
  using capstone disassembler + pyelftools ELF parser
- `research/artifacts/xref_analysis.txt` — raw output from ghidra_lite
- `research/artifacts/libdji_*.{bin,txt}` — extracted materials

## Next steps for the actually determined

1. Install Ghidra, auto-analyze the .so, manually trace arg flows
2. Dynamic analysis requires rooted Mavic 3 drone (firmware signed)
3. Side-channel on a sacrificial Mavic 3 SoC requires lab gear

At our level of analysis, firmware-derived key extraction has hit a wall.
The hardware attack path (voltage glitch BQ40Z307, cell transplant with
original BMS) remains the only realistic route for end users.
