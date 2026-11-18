"""Per-opcode legality from the pinned Musashi descriptors.

The committed CSVs hold CPU opcode metadata, never game ROM data. Each row is
mask,match,legal; rows apply in order, so a later, narrower row overrides an
earlier, broader one. RESET (0x4e70) is legal although Musashi gives it no
descriptor cost; callers keep that special case.
"""

from pathlib import Path


def _load(name: str) -> bytes:
    table = bytearray(65536)
    for line in Path(__file__).with_name(name).read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        mask_text, match_text, legal_text = line.split(",")
        mask, match, legal = int(mask_text, 16), int(match_text, 16), int(legal_text)
        free = (~mask) & 0xFFFF
        subset = free
        while True:
            table[match | subset] = legal
            if subset == 0:
                break
            subset = (subset - 1) & free
    return bytes(table)


LEGAL_68020 = _load("68020_opcodes.csv")
