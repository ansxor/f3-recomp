"""Decode configured ROM payloads at their executable RAM addresses."""

import itertools
from dataclasses import dataclass

from capstone import CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_020, Cs, CsInsn

from .emitter import lower, static_flow


@dataclass
class RamRoutine:
    destination: int
    payload: bytes
    instructions: dict[int, CsInsn]


def decode_ram_routines(rom: bytes, config: dict) -> list[RamRoutine]:
    specs = config.get("ram_code", [])
    if not isinstance(specs, list):
        raise ValueError("RAM code must be an array of tables")
    if not specs:
        return []
    decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
    decoder.detail = True
    routines = []
    for spec in specs:
        fields = {"destination", "source", "length", "transform", "entry"}
        if not isinstance(spec, dict) or spec.keys() != fields:
            raise ValueError("RAM code requires destination, source, length, transform and entry")
        destination, source, length, entry = (spec[key] for key in ("destination", "source", "length", "entry"))
        if any(type(value) is not int for value in (destination, source, length, entry)):
            raise ValueError("RAM code addresses and length must be integers")
        end = destination + length
        if (
            length <= 0
            or length % 4
            or source < 0
            or source % 2
            or source + length > len(rom)
            or destination % 2
            or entry % 2
            or not destination <= entry < end
            or not (0x400000 <= destination < end <= 0x440000 or 0x600000 <= destination < end <= 0x610000)
        ):
            raise ValueError(f"Invalid RAM code range at {destination:#x} or ROM source at {source:#x}")
        if spec["transform"] != "negate_be32":
            raise ValueError(f"Unknown RAM code transform: {spec['transform']!r}")
        payload = b"".join(
            (-int.from_bytes(rom[offset : offset + 4], "big") & 0xffffffff).to_bytes(4, "big")
            for offset in range(source, source + length, 4)
        )
        instructions = {}
        pending = [entry]
        while pending:
            pc = pending.pop()
            if pc in instructions:
                continue
            if pc % 2 or not destination <= pc < end:
                raise ValueError(f"RAM code path leaves payload at {pc:#x}")
            offset = pc - destination
            insn = next(decoder.disasm(payload[offset : offset + 24].ljust(24, b"\0"), pc, count=1), None)
            if insn is None or not insn.id or insn.size % 2 or pc + insn.size > end or lower(insn) is None:
                raise ValueError(f"Cannot translate RAM code at {pc:#x}")
            instructions[pc] = insn
            flow = static_flow(insn)
            pending.extend(target for target in flow.targets if destination <= target < end)
            if flow.falls_through or insn.mnemonic.split(".")[0] in ("bsr", "jsr"):
                pending.append(pc + insn.size)
        routines.append(RamRoutine(destination, payload, instructions))
    routines.sort(key=lambda routine: routine.destination)
    for first, second in itertools.pairwise(routines):
        if first.destination + len(first.payload) > second.destination:
            raise ValueError(f"Overlapping RAM code at {second.destination:#x}")
    return routines
