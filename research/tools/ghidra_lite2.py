"""Ghidra-lite v2 — call graph + argument register tracking.

Goal: trace where the root public key arg to a1006_auth_verify comes from.

Steps:
1. Disassemble full .text section of libdji_secure.so
2. Find all BL instructions targeting auth-related functions
3. For each caller, walk backwards to find how x0/x1/x2 args were set
   - MOV xN, xM   → source register
   - LDR xN, [xM, #off] → memory load at xM+off
   - ADRP+ADD xN, #page, #off → absolute data address
4. For LDR with struct-pointer-in-register, recursively trace where xM came from
5. If we reach a fixed .rodata/.data address, dump the content

Limitations:
- Can't follow stores into memory (compiler might stash key on stack)
- Can't always decide which path in a branching function is taken
- Register tracking is heuristic, not symbolic
"""
from pathlib import Path
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import *
from collections import defaultdict

LIBSEC = Path(r"c:/Users/inlar/PycharmProjects/esp32/research/firmware_mavic3/V01.00.0600_wm260_dji_system/decrypted/m0802_unpacked/system_extracted/lib64/libdji_secure.so")
OUT    = Path(r"c:/Users/inlar/PycharmProjects/esp32/research/artifacts/xref_deep_trace.txt")

# Known-offset functions (file offsets in the .so)
KEY_FUNCS = {
    0x3E298: 'a100x_verifyDERSig',         # ECDSA DER verify — takes pubkey arg
    0x40348: 'a1006_auth_verify',          # main accessory verify — takes cert + pubkey
    0x3F9F8: 'a1006_oneway_authentication',
    0x3FE40: 'a1006_auth_challenge',
    0x43840: 'auth_chip_init',             # initialization — might LOAD the key from rodata
    0x44588: 'init_auth_chip_from_json',   # loads from json config
    0x3F940: 'a1006_verify_device',
    0x3F440: 'bECC_verifyResponse',
}


def load():
    with open(LIBSEC, 'rb') as f:
        data = f.read()
    elf = ELFFile(open(LIBSEC, 'rb'))
    sections = {}
    for sec in elf.iter_sections():
        sections[sec.name] = {
            'addr': sec['sh_addr'],
            'offset': sec['sh_offset'],
            'size': sec['sh_size'],
        }
    return data, sections


def va2fo(va, sections):
    for name, s in sections.items():
        if s['addr'] <= va < s['addr'] + s['size']:
            return s['offset'] + (va - s['addr'])
    return None


def fo2va(fo, sections):
    for name, s in sections.items():
        if s['offset'] <= fo < s['offset'] + s['size']:
            return s['addr'] + (fo - s['offset'])
    return None


def find_bl_callers(data, sections, cs, target_va):
    """Scan .text for BL/B instructions that call target_va. Return list of caller VAs."""
    text = sections['.text']
    text_data = data[text['offset']:text['offset'] + text['size']]
    callers = []
    for insn in cs.disasm(text_data, text['addr']):
        if insn.mnemonic in ('bl', 'b'):
            try:
                tgt = int(insn.op_str, 0)
                if tgt == target_va:
                    callers.append(insn.address)
            except ValueError:
                pass
    return callers


def detect_function_start(data, sections, cs, va):
    """Backtrack from va to find function start. Heuristic: look for
    STP x29, x30, [sp, #-N]! or similar prologue pattern within 1024 bytes."""
    text = sections['.text']
    # Read up to 1024 bytes before va
    start_va = max(text['addr'], va - 1024)
    start_fo = va2fo(start_va, sections)
    code = data[start_fo:va2fo(va, sections) + 4]
    # Scan for common prologue instructions
    candidates = []
    for insn in cs.disasm(code, start_va):
        if insn.mnemonic == 'stp':
            # Check if it's stp x29, x30 or stp x19, x20 variants
            if 'x29' in insn.op_str or 'x30' in insn.op_str:
                candidates.append(insn.address)
        elif insn.mnemonic == 'sub' and 'sp, sp' in insn.op_str:
            candidates.append(insn.address)
    # Return most recent prologue candidate before va
    return candidates[-1] if candidates else start_va


def disasm_range(data, sections, cs, start_va, end_va):
    """Disassemble a range of VAs."""
    start_fo = va2fo(start_va, sections)
    end_fo = va2fo(end_va, sections)
    if start_fo is None or end_fo is None:
        return []
    code = data[start_fo:end_fo]
    return list(cs.disasm(code, start_va))


def track_register(instructions, target_reg, end_va):
    """Walk backward from end_va through instructions; find most recent write
    to target_reg and interpret what it sets the reg to.

    Returns dict: {'source': 'mov'|'ldr'|'adrp+add'|'arg', 'value': ...}
    """
    # Find instruction at end_va
    insn_idx = None
    for i, insn in enumerate(instructions):
        if insn.address == end_va:
            insn_idx = i
            break
    if insn_idx is None or insn_idx == 0:
        return {'source': 'unknown'}

    # Walk backward from insn_idx-1
    for i in range(insn_idx - 1, -1, -1):
        insn = instructions[i]
        mn = insn.mnemonic
        op = insn.op_str
        # Crude regex-like parsing
        ops = [o.strip() for o in op.split(',', 2)]

        # MOV dst, imm
        if mn == 'mov' and len(ops) >= 2:
            dst = ops[0]
            if dst == target_reg:
                src = ops[1]
                if src.startswith('#'):
                    try:
                        return {'source': 'mov_imm', 'value': int(src[1:], 0), 'at': insn.address}
                    except ValueError: pass
                elif src.startswith('x') or src.startswith('w'):
                    return {'source': 'mov_reg', 'value': src, 'at': insn.address}

        # ADD dst, src, #imm — might be used with ADRP
        if mn == 'add' and len(ops) == 3 and ops[0] == target_reg:
            # Check if previous instruction is ADRP to same reg
            if i > 0 and instructions[i-1].mnemonic == 'adrp':
                adrp_ops = [o.strip() for o in instructions[i-1].op_str.split(',')]
                if adrp_ops[0] == target_reg:
                    try:
                        page = int(adrp_ops[1].replace('#', ''), 0)
                        add_imm = int(ops[2].replace('#', ''), 0) if ops[2].startswith('#') else 0
                        return {'source': 'adrp+add', 'value': page + add_imm, 'at': insn.address}
                    except ValueError: pass
            # ADD with reg + reg + imm — track base register
            if ops[1].startswith('x') and ops[2].startswith('#'):
                try:
                    off = int(ops[2].replace('#', ''), 0)
                    return {'source': 'add_reg_imm', 'base': ops[1], 'offset': off, 'at': insn.address}
                except ValueError: pass

        # LDR dst, [base, #off] or LDR dst, [base]
        if mn in ('ldr', 'ldrb', 'ldrh') and len(ops) >= 2 and ops[0] == target_reg:
            # Parse memory operand
            rest = op[op.index(','):] if ',' in op else ''
            if '[' in rest:
                bracket = rest[rest.index('['):rest.rindex(']')+1]
                inner = bracket.strip('[]')
                parts = [p.strip() for p in inner.split(',')]
                base = parts[0]
                offset = 0
                if len(parts) > 1 and parts[1].startswith('#'):
                    try: offset = int(parts[1].replace('#', ''), 0)
                    except ValueError: pass
                return {'source': 'ldr', 'base': base, 'offset': offset, 'at': insn.address}
            # LDR xN, =imm (literal)
            if '=' in op:
                try:
                    # Form: "ldr x0, =0x12345"
                    imm_str = op.split('=')[1].strip()
                    return {'source': 'ldr_literal', 'value': int(imm_str, 0), 'at': insn.address}
                except (ValueError, IndexError): pass

        # LDP dst1, dst2, [base, #off]
        if mn == 'ldp' and len(ops) >= 3:
            dst1, dst2 = ops[0], ops[1]
            if dst1 == target_reg or dst2 == target_reg:
                # reg[target] loaded from memory
                bracket_part = ops[2] if '[' in ops[2] else op
                if '[' in bracket_part:
                    inner = bracket_part[bracket_part.index('[')+1 : bracket_part.rindex(']')]
                    parts = [p.strip() for p in inner.split(',')]
                    base = parts[0]
                    offset = 0
                    if len(parts) > 1 and parts[1].startswith('#'):
                        try: offset = int(parts[1].replace('#', ''), 0)
                        except ValueError: pass
                    if dst2 == target_reg:
                        offset += 8  # second element is 8 bytes later
                    return {'source': 'ldp', 'base': base, 'offset': offset, 'at': insn.address}

    # Register never written — came from function caller (argument)
    return {'source': 'arg', 'reg': target_reg}


def analyze_caller(data, sections, cs, caller_va, target_func_name, out):
    """At caller_va is a BL. Find function containing it, disasm, track x0/x1/x2."""
    func_start = detect_function_start(data, sections, cs, caller_va)
    # Disassemble from func_start to caller_va + 4
    insns = disasm_range(data, sections, cs, func_start, caller_va + 4)

    out.append(f"  Caller @ 0x{caller_va:08X} (calls {target_func_name})")
    out.append(f"    Assumed function start: 0x{func_start:08X} ({len(insns)} insns)")

    # Track where x0, x1, x2 were set right before BL
    for reg_idx, reg in enumerate(['x0', 'x1', 'x2', 'x3']):
        result = track_register(insns, reg, caller_va)
        src = result.get('source', '?')
        if src == 'arg':
            out.append(f"      {reg}: from caller's own arg (passthrough)")
        elif src == 'mov_reg':
            out.append(f"      {reg}: <- mov from {result['value']}")
        elif src == 'mov_imm':
            out.append(f"      {reg}: <- imm 0x{result['value']:x}")
        elif src == 'adrp+add':
            target_va = result['value']
            # Dump what's there
            fo = va2fo(target_va, sections)
            dump = ''
            if fo is not None and fo + 32 <= len(data):
                d = data[fo:fo+32]
                dump = d.hex()
                if all(0x20 <= b < 0x7F or b == 0 for b in d[:16]):
                    ascii_s = d[:16].replace(b'\x00', b'.').decode('ascii', errors='replace')
                    dump += f"  ({ascii_s!r})"
            out.append(f"      {reg}: <- ADRP+ADD to 0x{target_va:08X}: {dump}")
        elif src == 'add_reg_imm':
            out.append(f"      {reg}: <- {result['base']} + 0x{result['offset']:x}")
        elif src == 'ldr':
            out.append(f"      {reg}: <- LDR [{result['base']}, #{result['offset']:#x}]")
        elif src == 'ldp':
            out.append(f"      {reg}: <- LDP [{result['base']}, #{result['offset']:#x}]")
        else:
            out.append(f"      {reg}: {result}")


def main():
    data, sections = load()
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cs.detail = False

    out = []
    def W(s=""):
        out.append(s)
        print(s)

    W("=" * 78)
    W("Ghidra-lite v2: call graph + arg tracking in libdji_secure.so")
    W("=" * 78)
    W()

    # Build call graph for key functions
    # For each key function, find ALL callers
    for foff, fname in KEY_FUNCS.items():
        target_va = fo2va(foff, sections)
        if target_va is None: continue
        callers = find_bl_callers(data, sections, cs, target_va)
        W(f"=== {fname} @ 0x{target_va:08X} ({len(callers)} callers) ===")
        for c in callers[:10]:
            analyze_caller(data, sections, cs, c, fname, out)
        if len(callers) > 10:
            W(f"  ... + {len(callers)-10} more callers")
        W()

    with open(OUT, 'w', encoding='utf-8') as f:
        f.write('\n'.join(out))
    print(f"\nSaved to {OUT}")


if __name__ == '__main__':
    main()
