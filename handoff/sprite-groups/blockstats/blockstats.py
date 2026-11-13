#!/usr/bin/env python3
"""Per-frame hardware-block statistics from f3rt-run graphics.bin dumps.

Mirrors runtime/renderer/decode.cpp decode_sprite_list exactly (no splices, as the game path
calls it): same state machine, persistent state across frames (bank, flipscreen), same culling
against the scanout crop, same tile==0 handling. Read-only over the dump directory.

usage: blockstats.py DUMP_DIR VISIBLE_Y VISIBLE_HEIGHT OUT_JSONL [--first N] [--last N]
"""
import json, os, re, sys
from collections import Counter

H_START, H_VIS = 46, 320
SPRITE_RAM = 0x10000
BANK_STRIDE = 0x8000


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s32(v):
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v & 0x80000000 else v


class Axis:
    def __init__(self):
        self.block_scale = 1 << 8
        self.pos = 0
        self.block_pos = 0
        self.gl = 0
        self.sub = 0

    def update(self, scroll, posw, multi, block_ctrl, zoom):
        """Returns True when this update sets a new block origin (decode.cpp block_pos write)."""
        new = s16(posw | 0xF000) if posw & 0x800 else s16(posw & 0xFFF)
        if scroll & 0x01:
            self.sub = new
        if scroll & 0x02:
            self.gl = new
        if not (scroll & 0x08):
            new = s16(new + self.gl)
            if not (scroll & 0x04):
                new = s16(new + self.sub)
        origin = False
        if block_ctrl == 0b00:
            if not multi:
                self.block_pos = s32(new << 8)
                self.block_scale = 0x100 - zoom
                origin = True
            self.pos = self.block_pos
        elif block_ctrl == 0b10:
            self.pos = self.block_pos
        elif block_ctrl == 0b11:
            self.pos = s32(self.pos + self.block_scale * 16)
        return origin


class State:
    def __init__(self):
        self.flipscreen = False
        self.bank = False


def walk(gfx, st, visible_y, visible_height):
    """Walk one frame. Returns (visits, end_bank). visits: dicts in walk order."""
    x, y = Axis(), Axis()
    multi = False
    n_drawn = 0
    visits = []
    total = 0
    offs = 0
    while offs < 0x400 and total < 0x400:
        total += 1
        base = BANK_STRIDE if st.bank else 0
        cur_bank = int(st.bank)
        spr = gfx[base + offs * 16: base + offs * 16 + 16]
        w = [int.from_bytes(spr[i:i + 2], "big") for i in range(0, 14, 2)]
        w0, w1, w2, w3, w4, w5, w6 = w
        if w3 & 0x8000:
            st.flipscreen = bool(w5 & 0x2000)
            st.bank = bool(w5 & 0x0001)
        # list jump (allow_jump=true for list entries)
        if w6 & 0x8000:
            new_offs = w6 & 0x3FF
            if new_offs == offs:
                break  # list end (self-jump): entry not processed
            jump_target = new_offs
            offs_next_override = new_offs - 1
        else:
            jump_target = None
        spritecont = (w4 >> 8) & 0xFF
        scroll_mode = (w2 >> 12) & 0xF
        multi_prev = multi
        xc = (spritecont >> 6) & 3
        yc = (spritecont >> 4) & 3
        ox = x.update(scroll_mode, w2 & 0xFFF, multi, xc, w1 & 0xFF)
        oy = y.update(scroll_mode, w3 & 0xFFF, multi, yc, (w1 >> 8) & 0xFF)
        multi = (spritecont & 0x08) != 0
        tile = w0 | ((w5 & 1) << 16)
        v = {
            'offs': offs, 'bank': cur_bank, 'raw': spr.hex(), 'tile': tile,
            'cmd': bool(w3 & 0x8000), 'jump_to': jump_target,
            'multi_prev': multi_prev, 'xctrl': xc, 'yctrl': yc,
            'origin_x': ox, 'origin_y': oy, 'origin': ox or oy,
            'multi_prev_only_origin': not multi_prev,
            'drawn': False, 'cull': None, 'bank_after': int(st.bank),
        }
        visits.append(v)
        if tile and n_drawn < 1024:
            fs = st.flipscreen
            bsx, bsy = x.block_scale, y.block_scale
            tx = ((512 << 8) - bsx * 16 - x.pos) if fs else x.pos
            ty = ((256 << 8) - bsy * 16 - y.pos) if fs else y.pos
            if tx + bsx * 16 <= (H_START << 8):
                v['cull'] = 'x_left'
            elif tx > ((H_START + H_VIS - 1) << 8):
                v['cull'] = 'x_right'
            elif ty + bsy * 16 <= (visible_y << 8):
                v['cull'] = 'y_top'
            elif ty > ((visible_y + visible_height - 1) << 8):
                v['cull'] = 'y_bottom'
            else:
                v['drawn'] = True
                n_drawn += 1
                v['flip_screen'] = fs
        if jump_target is not None:
            offs = offs_next_override
        offs += 1
    return visits, int(st.bank)


def segment(visits):
    """Split walk into hardware blocks at new origins. Returns list of blocks (dicts)."""
    blocks = []
    cur = None
    for v in visits:
        if v['origin'] or cur is None:
            cur = {'origin_offs': v['offs'] if v['origin'] else None,
                   'implicit': not v['origin'], 'entries': [], 'drawn': []}
            blocks.append(cur)
        cur['entries'].append(v['offs'])
        if v['drawn']:
            cur['drawn'].append(v['offs'])
    return blocks


def main():
    dump, vy, vh, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    first = last = None
    if '--first' in sys.argv: first = int(sys.argv[sys.argv.index('--first') + 1])
    if '--last' in sys.argv: last = int(sys.argv[sys.argv.index('--last') + 1])
    frames = sorted(int(m.group(1)) for d in os.listdir(dump)
                    if (m := re.fullmatch(r'frame_(\d+)', d)))
    frames = [f for f in frames if (first is None or f >= first) and (last is None or f <= last)]
    st = State()  # cold: bank=0, flipscreen=0; chained across consecutive dumped frames
    prev = None
    rows = []
    with open(out, 'w') as fo:
        for f in frames:
            gfx = open(os.path.join(dump, f'frame_{f:04d}', 'graphics.bin'), 'rb').read(SPRITE_RAM)
            start_bank = int(st.bank)
            visits, end_bank = walk(gfx, st, vy, vh)
            blocks = segment(visits)
            drawn = [v for v in visits if v['drawn']]
            sizes = [len(b['drawn']) for b in blocks if b['drawn']]
            alt_origins = sum(1 for v in visits if v['multi_prev_only_origin'])
            rec = {
                'frame': f, 'start_bank': start_bank, 'end_bank': end_bank,
                'bank_switches': sum(1 for a, b in zip(visits, visits[1:]) if a['bank_after'] != b['bank_after']),
                'walked': len(visits), 'tile0': sum(1 for v in visits if not v['tile'] and not v['cmd'] and not v['jump_to']),
                'cmd': sum(1 for v in visits if v['cmd']),
                'culled': sum(1 for v in visits if v['tile'] and v['cull']),
                'drawn': len(drawn),
                'drawn_banks': dict(Counter(v['bank'] for v in drawn)),
                'origins_all': sum(1 for b in blocks if not b['implicit']),
                'implicit_blocks_with_drawn': sum(1 for b in blocks if b['implicit'] and b['drawn']),
                'blocks': sum(1 for b in blocks if b['drawn']),
                'blocks_ge2': sum(1 for s in sizes if s >= 2),
                'singletons': sum(1 for s in sizes if s == 1),
                'hist': {'1': sum(1 for s in sizes if s == 1), '2-4': sum(1 for s in sizes if 2 <= s <= 4),
                         '5-16': sum(1 for s in sizes if 5 <= s <= 16), '17+': sum(1 for s in sizes if s >= 17)},
                'members_in_multi': sum(s for s in sizes if s >= 2),
                'alt_origin_count_ignoring_ctrl': alt_origins,
                'drawn_entries': [[v['offs'], v['bank'], v['raw'], v['multi_prev'], v['xctrl'], v['yctrl'], v['origin']] for v in drawn],
                'blocks_detail': [[b['origin_offs'], b['drawn']] for b in blocks if b['drawn']],
            }
            st_flip = st.flipscreen
            rec['flipscreen_end'] = st_flip
            fo.write(json.dumps(rec) + '\n')
            rows.append(rec)
    print(f'frames={len(rows)} range={rows[0]["frame"]}..{rows[-1]["frame"]} out={out}')


if __name__ == '__main__':
    main()
