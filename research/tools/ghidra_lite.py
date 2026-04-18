"""Ghidra-lite: Python static analysis of libdji_secure.so.

Does what Ghidra xref analysis does, but scripted:
1. Parse ELF, locate text/rodata sections
2. Disassemble known functions with capstone (ARM64)
3. Find ADRP+ADD pairs — these compute absolute data addresses
4. Collect all addresses referenced by specific auth functions
5. Dump contents at each address — flag high-entropy blobs as key candidates
6. Cross-reference: which data addresses are read by ALL auth functions
   (likely shared root key) vs only some (likely context-specific)

Output: research/artifacts/xref_analysis.txt
"""
from pathlib import Path
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from collections import defaultdict

LIBSEC = Path(r"c:/Users/inlar/PycharmProjects/esp32/research/firmware_mavic3/V01.00.0600_wm260_dji_system/decrypted/m0802_unpacked/system_extracted/lib64/libdji_secure.so")
OUT    = Path(r"c:/Users/inlar/PycharmProjects/esp32/research/artifacts/xref_analysis.txt")

# Function offsets from memory — these are file offsets in the .so
FUNCS = [
    (0x3d650, 224,  "a100x_decompressCert"),
    (0x3d730, 544,  "a100x_generateChallenge"),
    (0x3d950, 484,  "a100x_verifyResponse"),
    (0x3e068, 384,  "a100x_v1_challenge"),
    (0x3e1e8, 176,  "a100x_v1_verify_response"),
    (0x3e298, 1244, "a100x_verifyDERSig"),        # <-- THE function that verifies accessory cert
    (0x3f410, 48,   "bECC_precomputeResponse"),
    (0x3f440, 132,  "bECC_verifyResponse"),
    (0x3f940, 180,  "a1006_verify_device"),
    (0x3f9f8, 436,  "a1006_oneway_authentication"),
    (0x3fe40, 904,  "a1006_auth_challenge"),
    (0x40348, 256,  "a1006_auth_verify"),          # <-- main battery auth entry
    (0x43840, 400,  "auth_chip_init"),
    (0x44588, 428,  "init_auth_chip_from_json"),
]


def load_elf():
    with open(LIBSEC, 'rb') as f:
        data = f.read()
    elf = ELFFile(open(LIBSEC, 'rb'))
    # Get section info
    sections = {}
    for sec in elf.iter_sections():
        sections[sec.name] = {
            'addr': sec['sh_addr'],
            'offset': sec['sh_offset'],
            'size': sec['sh_size'],
            'type': sec['sh_type'],
        }
    return data, sections


def va_to_file_offset(va, sections):
    """Convert virtual address to file offset using section map."""
    for name, s in sections.items():
        if s['addr'] <= va < s['addr'] + s['size']:
            return s['offset'] + (va - s['addr'])
    return None


def file_offset_to_va(fo, sections):
    """Reverse."""
    for name, s in sections.items():
        if s['offset'] <= fo < s['offset'] + s['size']:
            return s['addr'] + (fo - s['offset'])
    return None


def disasm_function(data, func_file_offset, size, sections, cs):
    """Disassemble function, return list of (va, mnemonic, op_str, bytes)."""
    text = data[func_file_offset:func_file_offset + size]
    base_va = file_offset_to_va(func_file_offset, sections)
    instructions = []
    for insn in cs.disasm(text, base_va):
        instructions.append((insn.address, insn.mnemonic, insn.op_str, bytes(insn.bytes)))
    return instructions


def find_adrp_add_pairs(instructions):
    """Find ADRP+ADD pairs — they form 64-bit absolute address computations.
    Returns list of (va_of_adrp, target_va)."""
    refs = []
    for i in range(len(instructions) - 1):
        va1, mn1, op1, _ = instructions[i]
        va2, mn2, op2, _ = instructions[i + 1]
        if mn1 == 'adrp' and mn2 == 'add':
            # Parse: "adrp x8, #0x4a000" and "add x8, x8, #0xb30"
            try:
                # adrp target
                adrp_parts = op1.split(', ')
                reg_a = adrp_parts[0].strip()
                page = int(adrp_parts[1].replace('#', '').strip(), 0)
                # Ensure ADD uses same register
                add_parts = op2.split(', ')
                if len(add_parts) != 3: continue
                if add_parts[0].strip() != reg_a: continue
                if add_parts[1].strip() != reg_a: continue
                offset = int(add_parts[2].replace('#', '').strip(), 0)
                target = page + offset
                refs.append((va1, target))
            except Exception:
                continue
        # Also ADRP + LDR (load directly from computed page)
        if mn1 == 'adrp' and mn2 in ('ldr', 'ldrb', 'ldrh', 'ldrsw'):
            try:
                adrp_parts = op1.split(', ')
                reg_a = adrp_parts[0].strip()
                page = int(adrp_parts[1].replace('#', '').strip(), 0)
                # LDR xN, [xM, #offset]
                if '[' in op2 and reg_a in op2:
                    bracket = op2[op2.index('['):op2.rindex(']') + 1]
                    parts = bracket.strip('[]').split(',')
                    offset = 0
                    if len(parts) > 1:
                        offset = int(parts[1].strip().replace('#', '').strip(), 0)
                    target = page + offset
                    refs.append((va1, target))
            except Exception:
                continue
    return refs


def dump_bytes_at(data, sections, va, n=64):
    fo = va_to_file_offset(va, sections)
    if fo is None or fo + n > len(data):
        return None
    return data[fo:fo + n]


def looks_like_key(blob):
    """Heuristic: 20+ distinct bytes, not mostly 0 or 0xFF."""
    if blob is None or len(blob) < 20:
        return False
    unique = len(set(blob))
    if unique < 16:
        return False
    if blob.count(0) > len(blob) // 2:
        return False
    if blob.count(0xFF) > len(blob) // 2:
        return False
    return True


def main():
    data, sections = load_elf()
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)

    out = []
    def W(s=""):
        out.append(s)
        print(s)

    W("=" * 70)
    W(f"libdji_secure.so  {len(data)} bytes")
    W(f".text   @ 0x{sections.get('.text', {}).get('addr', 0):08X}  size={sections.get('.text', {}).get('size', 0)}")
    W(f".rodata @ 0x{sections.get('.rodata', {}).get('addr', 0):08X}  size={sections.get('.rodata', {}).get('size', 0)}")
    W(f".data   @ 0x{sections.get('.data', {}).get('addr', 0):08X}  size={sections.get('.data', {}).get('size', 0)}")
    W()

    rodata_start = sections.get('.rodata', {}).get('addr', 0)
    rodata_end   = rodata_start + sections.get('.rodata', {}).get('size', 0)
    data_start   = sections.get('.data', {}).get('addr', 0)
    data_end     = data_start + sections.get('.data', {}).get('size', 0)

    # Per-function analysis
    all_refs = defaultdict(set)  # target_va -> set of function names
    refs_by_func = {}

    for (foff, fsize, fname) in FUNCS:
        W("-" * 70)
        W(f"Function {fname} @ file offset 0x{foff:06X} size={fsize}")

        # file offset → virtual address
        va = file_offset_to_va(foff, sections)
        if va is None:
            W(f"  WARN: can't map file offset to VA")
            continue

        instructions = disasm_function(data, foff, fsize, sections, cs)
        W(f"  disassembled {len(instructions)} instructions, first VA = 0x{va:08X}")

        # Find ADRP+ADD pairs
        refs = find_adrp_add_pairs(instructions)
        func_refs = set()
        for src_va, target_va in refs:
            # Classify target
            where = "?"
            if rodata_start <= target_va < rodata_end:   where = ".rodata"
            elif data_start <= target_va < data_end:     where = ".data"
            elif target_va < sections.get('.text', {}).get('addr', 0) + sections.get('.text', {}).get('size', 0):
                where = ".text"
            func_refs.add((target_va, where))
            all_refs[target_va].add(fname)
        refs_by_func[fname] = func_refs

        # Show top 15 most interesting refs (in .rodata)
        rodata_refs = sorted([t for (t, w) in func_refs if w == '.rodata'])
        W(f"  {len(func_refs)} data refs, {len(rodata_refs)} into .rodata")
        for t in rodata_refs[:15]:
            blob = dump_bytes_at(data, sections, t, 48)
            marker = " !KEY?" if looks_like_key(blob) else ""
            hex_str = blob[:32].hex() if blob else '?'
            # Show ASCII too
            ascii_s = ''.join(chr(b) if 0x20 <= b < 0x7F else '.' for b in (blob[:32] if blob else []))
            W(f"    0x{t:08X}: {hex_str}  [{ascii_s}]{marker}")

    W()
    W("=" * 70)
    W("Cross-reference summary: addresses referenced by MULTIPLE auth functions")
    W("(likely shared root key / curve params / cert structures)")
    W()
    shared = [(t, funcs) for t, funcs in all_refs.items() if len(funcs) >= 2]
    shared.sort(key=lambda x: -len(x[1]))
    for t, funcs in shared[:30]:
        blob = dump_bytes_at(data, sections, t, 48)
        marker = " !KEY?" if looks_like_key(blob) else ""
        if rodata_start <= t < rodata_end:
            where = ".rodata"
        elif data_start <= t < data_end:
            where = ".data"
        else:
            where = "?"
        hex_str = blob[:32].hex() if blob else '?'
        ascii_s = ''.join(chr(b) if 0x20 <= b < 0x7F else '.' for b in (blob[:32] if blob else []))
        W(f"  0x{t:08X} [{where}] ({len(funcs)}x) {sorted(funcs)}")
        W(f"                  {hex_str}")
        W(f"                  [{ascii_s}]{marker}")

    W()
    W("=" * 70)
    W("Addresses referenced ONLY by a1006_auth_verify / a100x_verifyDERSig")
    W("(candidates for hardcoded root public key for DJI battery cert verification)")
    W()
    key_funcs = {'a1006_auth_verify', 'a100x_verifyDERSig', 'a1006_oneway_authentication', 'bECC_verifyResponse'}
    for t, funcs in all_refs.items():
        if funcs & key_funcs:
            blob = dump_bytes_at(data, sections, t, 64)
            if looks_like_key(blob):
                where = ".rodata" if rodata_start <= t < rodata_end else ".data" if data_start <= t < data_end else "?"
                W(f"  0x{t:08X} [{where}] refs={sorted(funcs)}")
                W(f"                  {blob.hex()}")
                W(f"                  ascii: {''.join(chr(b) if 0x20 <= b < 0x7F else '.' for b in blob)}")
                W()

    with open(OUT, 'w', encoding='utf-8') as f:
        f.write('\n'.join(out))
    print(f"\nSaved to {OUT}")


if __name__ == '__main__':
    main()
