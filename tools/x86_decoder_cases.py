#!/usr/bin/env python3
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "core" / "relinker" / "codegen" / "tests" / "DecoderLengthCases.hpp"

PREFIXES = {0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3} | set(range(0x40, 0x50))
LEGACY_PREFIXES = [(), (0x66,), (0xF2,), (0xF3,), (0x48,), (0x66, 0x48), (0x67,)]
OPERAND_SIZE_PREFIXES = [(), (0x66,), (0x48,), (0x66, 0x48)]
MODRM_FORMS = [(0x84, 0x65), (0xC1,)]
TAIL = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x08, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x0F])
PREFIX_MNEMONICS = {"lock", "rep", "repne", "data16", "addr32", "cs", "ss", "ds", "es", "fs", "gs", "rex64", "rex",
                    "notrack", "xacquire", "xrelease"}
EXCLUDED_MNEMONICS = {"vmread", "vmreadq", "vmwrite", "vmwriteq"}
ENTRY = re.compile(r"^([0-9a-f]+) <t(\d+)>:$")
LINE = re.compile(r"^\s*[0-9a-f]+:\s([0-9a-f]{2}(?: [0-9a-f]{2})*)\s*\t(.*)$")


def candidates():
    one = [op for op in range(256) if op not in PREFIXES and op not in (0x0F, 0x62, 0x8F, 0xC4, 0xC5, 0xD5)]
    maps = [(prefix, (op,)) for prefix in LEGACY_PREFIXES for op in one]
    maps += [(prefix, (0x0F, op)) for prefix in LEGACY_PREFIXES for op in range(256) if op not in (0x0F, 0x38, 0x3A)]
    maps += [(prefix, (0x0F, escape, op)) for prefix in LEGACY_PREFIXES for escape in (0x38, 0x3A) for op in range(256)]
    for pp in range(4):
        for vex_l in range(2):
            vex2 = 0xF8 | (vex_l << 2) | pp
            maps += [((), (0xC5, vex2, op)) for op in range(256)]
            for mmmmm in (1, 2, 3):
                for w in range(2):
                    maps += [((), (0xC4, 0xE0 | mmmmm, (w << 7) | 0x78 | (vex_l << 2) | pp, op)) for op in range(256)]
    for prefix, opcode in maps:
        for form, modrm in enumerate(MODRM_FORMS):
            head = bytes(prefix) + bytes(opcode) + bytes(modrm)
            yield group_of(prefix, opcode), prefix, form, (head + TAIL)[:16]


def group_of(prefix, opcode):
    if opcode[0] == 0xC5:
        return ("vex", 1, opcode[2])
    if opcode[0] == 0xC4:
        return ("vex", opcode[1] & 0x1F, opcode[3])
    return ("legacy",) + tuple(opcode)


def select(rows):
    forms = {}
    for group, prefix, form, case, length in rows:
        forms.setdefault((group, prefix), {})[form] = (case, length)
    chosen = {}
    for (group, prefix), by_form in forms.items():
        without_modrm = len(by_form) == 2 and by_form[0][1] == by_form[1][1]
        form = 0 if 0 in by_form else 1
        case, length = by_form[form]
        chosen[(group, prefix)] = (without_modrm, form, length - len(prefix), case, length)
    kept = {}
    for (group, prefix), (without_modrm, form, relative, case, length) in sorted(chosen.items(), key=lambda item: len(item[0][1])):
        sizes = {chosen[(group, p)][2] for p in OPERAND_SIZE_PREFIXES if (group, p) in chosen}
        size_prefix = prefix if len(sizes) > 1 and prefix in OPERAND_SIZE_PREFIXES else None
        kept.setdefault(group, {}).setdefault((without_modrm, form, relative, size_prefix), (case, length))
    return sorted(value for variants in kept.values() for value in variants.values())


def disassemble(cases, objdump, clang):
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "cases.s"
        source.write_text("".join(f"t{index}:\n.byte {','.join(f'0x{b:02x}' for b in case)}\n"
                                  for index, case in enumerate(cases)))
        obj = Path(directory) / "cases.o"
        subprocess.run([clang, "-c", "-x", "assembler", "-target", "x86_64-unknown-linux-gnu", str(source), "-o", str(obj)],
                       check=True)
        text = subprocess.run([objdump, "-d", "--triple=x86_64-unknown-linux-gnu", str(obj)],
                              capture_output=True, text=True, check=True).stdout
    entries = {}
    current = None
    for raw in text.splitlines():
        entry = ENTRY.match(raw)
        if entry:
            current = int(entry.group(2))
            entries[current] = []
            continue
        line = LINE.match(raw)
        if line and current is not None and len(entries[current]) < 4:
            entries[current].append((len(line.group(1).split()), line.group(2).replace("\t", " ").split("#")[0].strip()))
    return entries


def length_of(instructions):
    total = 0
    for size, text in instructions:
        if not text or text.startswith("<unknown>") or "(bad)" in text or text == "lock":
            return None
        mnemonic = text.split()[0]
        if mnemonic in PREFIX_MNEMONICS and text == mnemonic:
            total += size
            continue
        if mnemonic in PREFIX_MNEMONICS or mnemonic in EXCLUDED_MNEMONICS:
            return None
        return total + size, mnemonic
    return None


def operand_size_branch(case, mnemonic):
    opcode = next(i for i, b in enumerate(case) if b not in PREFIXES)
    return 0x66 in case[:opcode] and (mnemonic.startswith("j") or mnemonic.startswith("call") or mnemonic.startswith("loop"))


def main():
    parser = argparse.ArgumentParser(description="Regenerate the LLVM-checked x86-64 decoder length cases")
    parser.add_argument("--objdump", default="llvm-objdump", help="LLVM objdump with the X86 target")
    parser.add_argument("--clang", default="clang", help="clang with the X86 target, used as the assembler")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()

    generated = list(candidates())
    entries = disassemble([case for _, _, _, case in generated], args.objdump, args.clang)
    rows = []
    for index, (group, prefix, form, case) in enumerate(generated):
        decoded = length_of(entries.get(index, []))
        if decoded is None:
            continue
        length, mnemonic = decoded
        if operand_size_branch(case, mnemonic):
            continue
        rows.append((group, prefix, form, case[:length].hex(), length))
    selected = select(rows)
    lines = ["#ifndef CODEGEN_TESTS_DECODERLENGTHCASES_HPP", "#define CODEGEN_TESTS_DECODERLENGTHCASES_HPP", "",
             "#include <cstddef>", "", "struct DecoderLengthCase {", "    const char* Bytes;", "    std::size_t Length;",
             "};", "", "inline constexpr DecoderLengthCase kDecoderLengthCases[] = {"]
    lines += [f'    {{"{hexbytes}", {length}}},' for hexbytes, length in selected]
    lines += ["};", "", "#endif", ""]
    args.output.write_text("\n".join(lines))
    print(f"{len(selected)} of {len(rows)} decoded cases written to {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
