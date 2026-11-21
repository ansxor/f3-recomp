"""Check CCR effect differences against the encoded instruction and replay both CFGs."""

from collections import deque

from tools.recomp_compare.decoder import matches, word_at
from tools.recomp_py.liveness import flag_effects
from tools.recomp_py.sprite_units import parse_sprite_units

ALL = 31
BITS = dict(zip("XNZVC", (16, 8, 4, 2, 1), strict=True))
MASKS = {
    "".join(flag for flag, bit in BITS.items() if mask & bit) or "-": mask
    for mask in range(32)
}


def privileged(word):
    return (
        word in (0x007C, 0x027C, 0x0A7C, 0x4E70, 0x4E72, 0x4E73, 0x4E7A, 0x4E7B)
        or word & 0xFFC0 in (0x40C0, 0x46C0)
        or matches(word, 0xFFF0, 0x4E60)
        or (matches(word, 0xFF00, 0x0E00) and word & 0xC0 != 0xC0)
    )


def effect_category(discovery, pc, after, old, new):
    data = discovery.window(pc)
    word, ext = word_at(data, 0), word_at(data, 2)
    if new[1].startswith(("exception:", "ram-exception:")):
        if after == "-" and discovery.category(pc, "disposition", old[1], new[1], old, new):
            return "flag-exception-row"
        return None
    if old[1].startswith("exception:"):
        return None
    old_effect = tuple(MASKS[flags] for flags in old[4:6])
    new_effect = tuple(MASKS[flags] for flags in new[4:6])
    x, n, z, v, c = BITS.values()
    nzvc = ALL ^ x
    pair = old_effect, new_effect
    if word & 0xF1C0 in (0x4100, 0x4180) and pair == ((ALL, 0), (x, z | v | c)):
        return "flag-chk"
    if (word & 0xF0C0 == 0x80C0 or word & 0xFFC0 == 0x4C40) and pair == ((ALL, 0), (ALL ^ c, c)):
        return "flag-divide"
    if word & 0xF9C0 == 0x00C0 and (word >> 9 & 7) <= 2:
        if pair == ((ALL, 0), ((x | n | v) if ext & 0x800 else 0, z | c)):
            return "flag-bounds"
    if word >> 12 == 0xE and word & 0xC0 != 0xC0 and word & 0x20:
        if word & 0x18 in (0, 8) and pair == ((x, nzvc), (0, nzvc)):
            return "flag-shift"
        if word & 0x18 == 0x10 and pair == ((x, nzvc), (x, ALL)):
            return "flag-shift"
    if matches(word, 0xFFC0, 0x4800) and new_effect == (x | z, ALL ^ z):
        if old_effect in ((x | z, ALL), (ALL, 0)):
            return "flag-nbcd"
    immediate = ext & ALL
    status_effects = {
        0x003C: (ALL ^ immediate, immediate),
        0x023C: (immediate, ALL ^ immediate),
        0x0A3C: (immediate, 0),
    }
    if word in status_effects and pair == (status_effects[word], (ALL, ALL)):
        return "flag-ccr-immediate"
    if word == 0x4E77 and pair == ((ALL, 0), (0, ALL)):
        return "flag-rtr"
    if word in (0x51FA, 0x51FB, 0x51FC) and pair == ((ALL, 0), (0, 0)):
        return "flag-trapf"
    return None


class Replay:
    """Independent least solution by backward propagation of each new live-in bit."""

    def __init__(self, discovery, config, records, *, frozen):
        instructions = {}
        starts, ends = parse_sprite_units(config).hook_pcs()
        pins = starts.keys() | ends.keys()
        for (pc, kind), row in records:
            if row[1] == "sprite-reset" and frozen:
                pins.add(pc)
            if kind or row[2] == "-":
                continue
            instructions[pc] = row
            if row[1] in ("ram", "ram-fallback", "fallback"):
                pins.add(pc)
            if not frozen and matches(word_at(discovery.window(pc, 2), 0), 0xFFC0, 0x06C0):
                pins.add(pc)

        observers = 0
        if not frozen:
            for flag in config.get("cpu", {}).get("interrupt_observes", []):
                observers |= BITS[flag.upper()]

        self.out = bytearray((max(instructions, default=0) >> 1) + 1)
        incoming = bytearray(len(self.out))
        writes = bytearray(len(self.out))
        predecessors = {}
        queue = deque()
        for pc, row in instructions.items():
            index = pc >> 1
            flow = row[3].removeprefix("branch:").split(",")
            targets = [int(edge, 16) for edge in flow if edge.startswith("0x")]
            if "next" in flow:
                next_pc = pc + int(row[2])
                targets.append(next_pc if frozen else next_pc & 0xFFFFFF)
            reads, writes[index] = MASKS[row[4]], MASKS[row[5]]
            boundary = "escape" in flow
            if frozen:
                if pc in pins:
                    reads = ALL
                if not boundary and row[4:6] == ["XNZVC", "-"]:
                    data = discovery.window(pc, 24)
                    insn = next(discovery.capstone.disasm(data, pc, count=1), None)
                    boundary = insn is not None and flag_effects(insn).opaque
            else:
                boundary |= pc in pins or privileged(word_at(discovery.window(pc), 0))
            for target in targets:
                if target not in instructions or (not frozen and target in pins):
                    boundary = True
                else:
                    predecessors.setdefault(target >> 1, []).append(index)
            out = ALL if boundary else observers
            self.out[index] = out
            incoming[index] = reads | (out & ~writes[index])
            if incoming[index]:
                queue.append(index)

        while queue:
            target = queue.popleft()
            for index in predecessors.get(target, ()):
                added = incoming[target] & ~self.out[index]
                if added:
                    self.out[index] |= added
                    new_input = added & ~writes[index] & ~incoming[index]
                    if new_input:
                        incoming[index] |= new_input
                        queue.append(index)

    def matches(self, pc, live_out):
        return pc >> 1 < len(self.out) and self.out[pc >> 1] == MASKS[live_out]
