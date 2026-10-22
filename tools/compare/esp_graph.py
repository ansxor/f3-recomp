"""Decode the ES5510 effects program a reference-backend run loaded and check the HLE graph against it.

Input is a lossless driver bus trace (F3SND2) of the reference backend::

    f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 8 \\
        --audio-backend reference --sound-driver native --sound-trace OUT.trace --wav OUT.wav

The trace's ES5510 host writes are replayed into a pure-Python integer port of
``runtime/third_party/audio/es5510.cpp`` (frame level, the same pipeline order), which then gives

``disasm``   the loaded program: delay address, MAC (c = c * d, accumulate, destination) and ALU per instruction;
``graph``    the exact signal graph. The port is run symbolically: every register carries, next to its integer
             value, a linear expression over the inputs, the delay RAM writes and the registers carried from the
             previous frame. Each delay RAM write becomes a node ``W<instr>``, each read becomes ``W<n>[z-d]``
             with d resolved from the actual RAM addresses (ES5510 delay addresses are ``(dbase + offset) >> 8``
             with ``dbase`` falling by one address per frame, so a read sees the write of the instruction whose
             offset is d * 256 lower, d frames ago); coefficients are printed as real numbers;
``verify``   the HLE graph of the loaded program (``Model`` for the ROM programs 0, 5 and 6, ``Model1`` for programs 1/2;
             the equations in runtime/audio/hle/faust/es5510.lib, reverb.dsp, chorus.dsp, echo.dsp and hall.dsp)
             against the port, on an impulse, with the port's LFO
             registers fed to the model; prints the error against the port's output;
``programs`` every program the driver loaded during the trace and whether it is one of the decoded ones.

Only numpy is needed. ``graph`` runs about 70000 warm-up frames (a minute) so that every delay has been written
once before a read is attributed to its writer.
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
from pathlib import Path
from typing import Any

import numpy as np

Lin = dict[Any, float]
M24 = 0xFFFFFF
RECORD = struct.Struct("<QQIIIBB2x")
ES5510_BASE = 0x260000


def s24(v: int) -> int:
    v &= M24
    return v - (1 << 24) if v & 0x800000 else v


def s48(v: int) -> int:
    v &= (1 << 48) - 1
    return v - (1 << 48) if v & (1 << 47) else v


def s16(v: int) -> int:
    v &= 0xFFFF
    return v - 65536 if v & 0x8000 else v


def real(v: int) -> float:
    return s24(v) / 2**23


# (alu src, alu dst, mac src, mac dst) per operand-select field; R = register, D = delay (DIL/DOL), B = both.
OPERAND_SELECT = [
    "RRRR", "RRRD", "RRRB", "RRDR", "RRDB", "RDRR", "RDDR", "RBRR",
    "RBDR", "DRRR", "DRRD", "DRRB", "DRDR", "DRDB", "DBRR", "DBDR",
]
RAM_CONTROL = [("R", "D"), ("W", "D"), ("R", "A"), ("W", "A"), ("R", "B"), ("F", "D"), ("R", "I"), ("W", "I")]
ALU_NAMES = ["ADD", "SUB", "ADDU", "SUBU", "CMP", "AND", "OR", "XOR", "ABS", "MOV", "ASL2", "ASL8", "LS15", "DIFF", "ASR", "END"]
SER_INDEX = {234: 1, 235: 0, 236: 3, 237: 2, 238: 5, 239: 4, 240: 7, 241: 6}  # register -> ser_w index
SER_NAMES = ["ser0l", "ser0r", "ser1l", "ser1r", "ser2l", "ser2r", "ser3l", "ser3r"]
REG_NAMES = {234: "ser0r", 235: "ser0l", 236: "ser1r", 237: "ser1l", 238: "ser2r", 239: "ser2l", 240: "ser3r",
             241: "ser3l", 242: "MACL", 243: "MACH", 244: "DIL", 255: "0", 252: "MAX", 253: "MIN", 254: "MAXP"}


def lin_add(a: Lin, b: Lin, k: float = 1.0) -> Lin:
    r = dict(a)
    for v, c in b.items():
        r[v] = r.get(v, 0.0) + c * k
        if abs(r[v]) < 1e-12:
            del r[v]
    return r


def lin_scale(a: Lin, k: float) -> Lin:
    return {v: c * k for v, c in a.items()} if k else {}


class Esp:
    """Frame-level integer port of the ES5510 emulation, with optional linear-expression shadow values."""

    def __init__(self) -> None:
        self.g = [0] * 0xC0
        self.instr = [0] * 160
        self.dram = np.zeros(1 << 20, dtype=np.int32)
        self.ser = [0] * 8
        self.memsiz, self.memmask, self.memincrement, self.memshift = 0xFFFFFF, 0, 1 << 24, 24
        self.dlength = self.abase = self.bbase = self.dbase = self.sigreg = 0
        self.mulshift = 2
        self.machl, self.ov, self.dil, self.ccr, self.cmr = 0, False, 0, 0, 0
        self.dol, self.dolc = [0, 0], 0
        self.alu: dict[str, Any] = dict(aReg=255, bReg=255, src="R", dst="R", op=15, aValue=0, bValue=0, result=0, upd=1, wr=0)
        self.mac: dict[str, Any] = dict(cReg=255, dReg=255, src="R", dst="R", acc=0, cValue=0, dValue=0, wr=0)
        self.ram = dict(addr=0, io=0, cyc=0, ix=-1)
        self.ramp, self.rampp = dict(self.ram), dict(self.ram)
        self.gpr_latch = self.dol_latch = self.dadr_latch = self.instr_latch = self.ram_sel = 0
        self.frame = 0
        self.pc = 0
        self.new_flags = 0
        # symbolic state
        self.sym = False
        self.gl = [{} for _ in range(0xC0)]
        self.lmac, self.ldil, self.ldol = {}, {}, [{}, {}]
        self.mac.update(ix=-1, frm=0, lc={}, ld={})
        self.alu.update(ix=-1, frm=0, la={}, lb={})
        self.dsrc = [None, None]
        self.meta, self.metanz, self.late = {}, {}, {}
        self.outlin, self.statedef = {}, {}
        self.nonlinear: dict[int, tuple[str, Lin, Lin]] = {}

    # -- host interface (sound CPU bus), as in ES5510::host_w --------------------------------------------------
    def host_w(self, offset: int, data: int) -> None:
        if offset in (0, 1, 2):
            sh = 16 - 8 * offset
            self.gpr_latch = (self.gpr_latch & ~(0xFF << sh)) | (data << sh)
        elif 3 <= offset <= 8:
            sh = 40 - 8 * (offset - 3)
            self.instr_latch = (self.instr_latch & ~(0xFF << sh)) | (data << sh)
        elif offset in (0x0C, 0x0D, 0x0E):
            sh = 16 - 8 * (offset - 0x0C)
            self.dol_latch = (self.dol_latch & ~(0xFF << sh)) | (data << sh)
        elif offset in (0x0F, 0x10, 0x11):
            sh = 16 - 8 * (offset - 0x0F)
            self.dadr_latch = (self.dadr_latch & ~(0xFF << sh)) | (data << sh)
            if offset == 0x0F and not self.ram_sel:
                self.dram[self.dadr_latch & 0xFFFFF] = s16(self.dol_latch >> 8)
        elif offset == 0x12 and data & 2:
            self.dram[:] = 0
        elif offset == 0x14:
            self.ram_sel = data & 0x80
        elif offset == 0xA0:
            self.write_reg(data, self.gpr_latch)
        elif offset == 0xC0:
            if data < 0xA0:
                self.instr[data] = self.instr_latch & 0xFFFFFFFFFFFF
        elif offset == 0xE0:
            if data < 0xA0:
                self.instr[data] = self.instr_latch & 0xFFFFFFFFFFFF
            self.write_reg(data, self.gpr_latch)

    def program_key(self) -> tuple[int, ...]:
        """The loaded program: instruction words up to and including END."""
        words = []
        for x in self.instr:
            words.append(x)
            if (x >> 12) & 15 == 15:
                break
        return tuple(words)

    def replay_trace(self, path: str, until_sample: int | None = None, on_write=None) -> None:
        with open(path, "rb") as f:
            if f.read(8) != b"F3SND2\0\0":
                raise SystemExit("not an F3SND2 trace")
            while block := f.read(RECORD.size * 8192):
                for _, sample, _, addr, value, kind, width in RECORD.iter_unpack(block):
                    if kind == 5 or (until_sample is not None and sample > until_sample):
                        return
                    if kind != 3 or not ES5510_BASE <= addr < ES5510_BASE + 0x200:
                        continue
                    if width == 4:
                        self.host_w((addr >> 1) & 0xFF, (value >> 16) & 0xFF)
                        self.host_w(((addr + 2) >> 1) & 0xFF, value & 0xFF)
                    elif width == 2:
                        self.host_w((addr >> 1) & 0xFF, value & 0xFF)
                    elif width == 1 and addr & 1:
                        self.host_w((addr >> 1) & 0xFF, value & 0xFF)
                    else:
                        continue
                    if on_write is not None:
                        on_write(sample, (addr >> 1) & 0xFF)

    # -- registers ---------------------------------------------------------------------------------------------
    def lin_of(self, r: int) -> Lin:
        if r < 0xC0:
            return self.gl[r]
        if r in SER_INDEX:
            return {("in", SER_INDEX[r]): 1.0} if self.sym else {}
        if r in (242, 243):
            return self.lmac
        if r == 244:
            return self.ldil
        return {}

    def read_reg(self, r: int) -> int:
        if r < 0xC0:
            return self.g[r]
        if r in SER_INDEX:
            return (self.ser[SER_INDEX[r]] << 8) & M24
        if r == 242:
            return (0 if self.machl < 0 else M24) if self.ov else (self.machl & M24)
        if r == 243:
            return (0x800000 if self.machl < 0 else 0x7FFFFF) if self.ov else ((self.machl >> 24) & M24)
        return {244: (self.dil << 8) & M24, 245: self.dlength, 246: self.abase, 247: self.bbase, 248: self.dbase,
                249: self.sigreg, 250: (self.ccr << 16) & M24, 251: (self.cmr << 16) & M24, 252: M24,
                253: 0x800000, 254: 0x7FFFFF}.get(r, 0)

    def write_reg(self, r: int, v: int, lin: Lin | None = None) -> None:
        v &= M24
        if r < 0xC0:
            self.g[r] = v
            if lin is not None:
                self.gl[r] = lin
        elif r in SER_INDEX:
            self.ser[SER_INDEX[r]] = s16(v >> 8)
            if lin is not None:
                self.outlin[SER_INDEX[r]] = lin
        elif r == 242:
            self.machl = s48((self.machl & (M24 << 24)) | v)
        elif r == 243:
            self.machl = s48((self.machl & M24) | (v << 24))
            self.ov = False
            if lin is not None:
                self.lmac = dict(lin)
        elif r == 244:
            n = 0
            x = v
            while x & 1:
                n += 1
                x >>= 1
            self.memshift = n
            self.memsiz = M24 >> (24 - n)
            self.memmask = M24 & ~self.memsiz
            self.memincrement = 1 << n
            self.dbase &= self.memmask
        elif r == 245:
            self.dlength = v
        elif r == 246:
            self.abase = v
        elif r == 247:
            self.bbase = v
        elif r == 248:
            self.dbase = v
        elif r == 249:
            self.sigreg = v
            self.mulshift = 1 if v & (1 << 22) else 2
        elif r == 250:
            self.ccr = (v >> 16) & 0xF8
        elif r == 251:
            self.cmr = (v >> 16) & 0xFC

    def push_dol(self, val: int, src, lin: Lin | None) -> None:
        d16 = s16(s24(val) >> 8)
        lin = lin if lin is not None else {}
        if self.dolc >= 2:
            self.dol[0], self.dol[1] = self.dol[1], d16
            self.dsrc[0], self.dsrc[1] = self.dsrc[1], src
            self.ldol[0], self.ldol[1] = self.ldol[1], lin
        else:
            self.dol[self.dolc], self.dsrc[self.dolc], self.ldol[self.dolc] = d16, src, lin
            self.dolc += 1

    def alu_op(self, op: int, a: int, b: int) -> int:
        f = self.ccr

        def add(x: int, y: int) -> int:
            nonlocal f
            x &= M24
            y &= M24
            r = x + y
            ov = (x & 0x800000) == (y & 0x800000) and (x & 0x800000) != (r & 0x800000)
            n = (r & 0x800000) != 0
            f = (f & ~0xF8) | (0x40 if r & 0x1000000 else 0) | (0x80 if n else 0) | (0x08 if not r & M24 else 0) \
                | (0x20 if ov else 0) | (0x10 if ov != n else 0)
            return r & M24

        def neg(v: int) -> int:
            return ((v ^ M24) + 1) & M24

        def sat(t: int, negative: bool) -> int:
            nonlocal f
            if f & 0x20:
                f = (f | 0x80) if negative else (f & ~0x80)
                f &= ~0x08
                return 0x800000 if negative else 0x7FFFFF
            return t

        r = 0
        if op == 0:
            r = sat(add(a, b), bool(a & 0x800000))
        elif op == 1:
            r = sat(add(a, neg(b)), bool(a & 0x800000))
        elif op == 2:
            r = add(a, b)
        elif op == 3:
            r = add(a, neg(b))
        elif op == 4:
            add(a, neg(b))
            r = a
        elif op in (5, 6, 7):
            r = a & b if op == 5 else a | b if op == 6 else a ^ b
            f = (f | 0x80) if r & 0x800000 else (f & ~0x80)
            f = (f | 0x08) if r == 0 else (f & ~0x08)
        elif op == 9:
            r = b
        elif op == 12:
            f = (f & ~0x80)
            f = (f | 0x40) if b & 0x800000 else (f & ~0x40)
            r = (b << 15) & 0x7FFFFF
        elif op == 13:
            r = add(0x7FFFFF, neg(b))
        elif op == 14:
            f = (f | 0x80) if b & 0x800000 else (f & ~0x80)
            f = (f | 0x40) if b & 1 else (f & ~0x40)
            r = (b >> 1) | (b & 0x800000)
        elif op in (10, 11):
            sh = 2 if op == 10 else 8
            carry = bool((b & M24) & (1 << (24 - sh)))
            shifted = s24(b) << sh
            ov = shifted > 0x7FFFFF or shifted < -0x800000
            r = (0x800000 if shifted < 0 else 0x7FFFFF) if ov else (shifted & M24)
            f = (f | 0x40) if carry else (f & ~0x40)
            f = (f | 0x20) if ov else (f & ~0x20)
            f = (f | 0x80) if r & 0x800000 else (f & ~0x80)
            f = (f | 0x08) if r == 0 else (f & ~0x08)
        self.new_flags = f
        return r

    def mac_result(self) -> None:
        m = self.mac
        if not m["wr"]:
            return
        product = (s24(m["cValue"]) * s24(m["dValue"])) << self.mulshift
        result = product + self.machl if m["acc"] else product
        self.ov = not -(1 << 47) <= result < (1 << 47)
        self.machl = result
        tmp = (0x800000 if result < 0 else 0x7FFFFF) if self.ov else (result & 0x0000FFFFFF000000) >> 24
        if self.sym:
            lc, ld = m["lc"], m["ld"]
            k = 2 ** (self.mulshift - 1)
            if lc and ld:  # signal x signal: an opaque node N<instr>, defined in self.nonlinear
                lp = {("N", m["ix"]): 1.0}
                self.nonlinear[m["ix"]] = ("mac product", lc, ld)
            else:
                lp = lin_scale(lc, real(m["dValue"]) * k) if lc else lin_scale(ld, real(m["cValue"]) * k) if ld else {}
            lres = lin_add(lp, self.lmac) if m["acc"] else lp
            self.lmac = lres
        else:
            lres = None
        if m["dst"] in "RB":
            self.write_reg(m["cReg"], tmp, lres)
        if m["dst"] in "DB":
            self.push_dol(tmp, ("mac", m["ix"], m["frm"]), lres)

    def run_frame(self, ins=None) -> tuple[int, int]:
        """One ES5510::run_once(): wake, then execute up to and including END. ins = six ser_w values."""
        if ins is not None:
            self.ser[:6] = list(ins)
        self.pc = 0
        self.frame += 1
        if self.sym:
            self.statedef = {}
            for r in range(0xC0):
                if self.gl[r]:
                    self.statedef[r] = self.gl[r]
                    self.gl[r] = {("S", r): 1.0}
            if self.lmac:
                self.statedef["machl"] = self.lmac
                self.lmac = {("S", "machl"): 1.0}
            self.outlin = {}
        for _ in range(300):
            self.rampp, self.ramp = dict(self.ramp), dict(self.ram)
            ix = self.pc
            instr = self.instr[ix]
            if self.rampp["cyc"] != 1:
                if self.rampp["io"]:
                    self.dil = 0
                else:
                    a = self.rampp["addr"] & 0xFFFFF
                    self.dil = int(self.dram[a])
                    mt = self.meta.get(a)
                    self.ldil = {("R", mt[1], self.frame - mt[2]): 1.0} if self.sym and mt and self.metanz.get(a) else {}
            cyc, acc = RAM_CONTROL[(instr >> 3) & 7]
            self.ram = dict(cyc=1 if cyc == "W" else 2 if cyc == "F" else 0, io=1 if acc == "I" else 0, addr=0, ix=ix)
            offset = self.g[ix]
            if acc == "D":
                modulo = self.dlength + self.memincrement
                mr = 0
                if modulo > 0:
                    mr = self.dbase + offset
                    if mr >= modulo:
                        mr -= modulo
                    if mr >= modulo or mr < 0:
                        mr %= modulo
                self.ram["addr"] = (mr & self.memmask) >> self.memshift
            elif acc == "A":
                self.ram["addr"] = ((self.abase + offset) & self.memmask) >> self.memshift
            elif acc == "B":
                self.ram["addr"] = ((self.bbase + offset) & self.memmask) >> self.memshift
            else:
                self.ram["addr"] = offset & 0xFFFFF0
            osel = OPERAND_SELECT[(instr >> 8) & 15]
            skippable = bool(instr & 0x80)
            skip = skippable and (((self.ccr & self.cmr & 0xF8) != 0) != bool(self.cmr & 4))
            self.mac_result()
            m = self.mac
            m.update(ix=ix, frm=self.frame, cReg=(instr >> 32) & 0xFF, dReg=(instr >> 40) & 0xFF, src=osel[2],
                     dst=osel[3], acc=(instr >> 6) & 1, wr=not skip)
            m["cValue"] = self.read_reg(m["cReg"]) if m["src"] == "R" else (self.dil << 8)
            m["dValue"] = self.read_reg(m["dReg"])
            m["lc"] = self.lin_of(m["cReg"]) if m["src"] == "R" else self.ldil
            m["ld"] = self.lin_of(m["dReg"])
            a = self.alu
            if a["wr"]:
                a["result"] = self.alu_op(a["op"], a["aValue"], a["bValue"])
                if self.sym:
                    la, lb, op = a["la"], a["lb"], a["op"]
                    if op in (0, 2):
                        lr = lin_add(la, lb)
                    elif op in (1, 3):
                        lr = lin_add(la, lb, -1.0)
                    elif op == 9:
                        lr = lb
                    elif op == 14:
                        lr = lin_scale(lb, .5)
                    else:
                        lr = {}
                        if (la or lb) and op not in (4, 15):  # shift/logic on signal data: opaque node N<instr>
                            lr = {("N", a["ix"]): 1.0}
                            self.nonlinear[a["ix"]] = (ALU_NAMES[op], la, lb)
                else:
                    lr = None
                if a["op"] != 4:
                    if a["dst"] in "RB":
                        self.write_reg(a["aReg"], a["result"], lr)
                    if a["dst"] in "DB":
                        self.push_dol(a["result"], ("alu", a["ix"], a["frm"]), lr)
                if a["upd"]:
                    self.ccr = self.new_flags
            a.update(ix=ix, frm=self.frame, aReg=(instr >> 16) & 0xFF, bReg=(instr >> 24) & 0xFF,
                     op=(instr >> 12) & 15, src=osel[0], dst=osel[1], wr=(not skip) or ((instr >> 12) & 15) == 4,
                     upd=(not skippable) or ((instr >> 12) & 15) == 4, la={}, lb={})
            done = a["op"] == 15
            if done:
                self.dbase -= self.memincrement
                if self.dbase < 0:
                    self.dbase = self.dlength
                if self.sym and self.mac["wr"]:  # settle the last MAC now so its expression is in this frame's terms
                    self.mac_result()
                    self.mac["wr"] = False
            elif a["op"] >= 8:
                a["bValue"] = self.read_reg(a["bReg"]) if a["src"] == "R" else (self.dil << 8) & M24
                a["lb"] = self.lin_of(a["bReg"]) if a["src"] == "R" else self.ldil
            else:
                a["aValue"] = self.read_reg(a["aReg"]) if a["src"] == "R" else (self.dil << 8) & M24
                a["bValue"] = self.read_reg(a["bReg"])
                a["la"] = self.lin_of(a["aReg"]) if a["src"] == "R" else self.ldil
                a["lb"] = self.lin_of(a["bReg"])
            if self.ramp["cyc"] != 0:
                if self.ramp["cyc"] == 1 and not self.ramp["io"]:
                    addr = self.ramp["addr"] & 0xFFFFF
                    self.dram[addr] = self.dol[0]
                    cf = self.dsrc[0][2] if self.dsrc[0] else self.frame
                    self.meta[addr] = (self.dsrc[0], self.ramp["ix"], cf)
                    self.metanz[addr] = bool(self.ldol[0])
                    self.late.setdefault(cf, {})[self.ramp["ix"]] = self.ldol[0]
                self.dol[0] = self.dol[1]
                self.dsrc[0] = self.dsrc[1]
                self.ldol[0] = self.ldol[1]
                if self.dolc > 0:
                    self.dolc -= 1
            self.pc += 1
            if done:
                break
        return self.ser[6], self.ser[7]


# --------------------------------------------------------------------------------------------------------------
# The HLE graphs. Mirror the FAUST programs in runtime/audio/hle/faust/ (es5510.lib: all-pass networks and the shared
# tank; reverb.dsp, chorus.dsp, echo.dsp, hall.dsp: the per-program front ends). Three programs are decoded: the
# ROM pointer-table programs 0 (reverb only), 5 (chorus + reverb) and 6 (ping-pong echo + enhancer + reverb).
# --------------------------------------------------------------------------------------------------------------
# Reverb tank constants per program (GPR-derived, see the note): input scale k, input leak, loop-filter terms
# c1/c2, all-pass gain g, wet tap weights.
TANK = {
    0: dict(k=.5, leak=.0703, c1=.0087905, c2=.0615333, g=.40625, tl=(.125, .625, .4375), tr=(.125, .625, .625)),
    5: dict(k=.7071, leak=.0075, c1=.0070313, c2=.0492188, g=.4, tl=(.13333, .66667, .46667), tr=(.13333, .66667, .66667)),
    6: dict(k=.7071, leak=.0075, c1=.0070313, c2=.0492188, g=.4, tl=(.13333, .66667, .46667), tr=(.13333, .66667, .66667)),
}


def detect_program(esp: Esp) -> int:
    """Identify the decoded program from its instruction words (instruction 1 differs per program)."""
    return {0xED7DEF399128: 0, 0x737DEF4E9128: 5, 0x617DEF3F9128: 6, 0xED7DEF679128: 1}.get(esp.instr[1], -1)


def sat(x: float) -> float:
    return max(-1.0, min(1.0, x))


class Model:
    def __init__(self, program: int, n: int, g110, g1, t122, t123, t124, t125, fb=0.0, g118=0.0, g119=1.0,
                 echo=8176.0, mod_center=921.0):
        self.prog = program
        self.c = TANK[program]
        self.g110, self.g1, self.g2 = g110, g1, 1 - g1
        self.t122, self.t123, self.t124, self.t125, self.fb = t122, t123, t124, t125, fb
        self.sin_th, self.cos_th = g118, g119
        self.echo, self.mod_center = echo, mod_center
        keys = [48, 50, 53, 54, 64, 68, 69, 85, 87, 88] + ([29, 34] if program in (5, 6) else [])
        self.w = {k: np.zeros(n + 4) for k in keys}
        self.g128 = self.g129 = self.g130 = self.g131 = self.g115 = self.g116 = 0.0
        self.g67 = self.g70 = self.g71 = self.g89 = self.g90 = self.g91 = 0.0
        self.g105 = self.g111 = self.g112 = self.g113 = 0.0

    def rd(self, k: int, t: int, d: int) -> float:
        return self.w[k][t - d] if t - d >= 0 else 0.0

    def tap(self, k: int, t: int, d: float) -> float:
        if d < 0:
            return 0.0
        i = int(math.floor(d))
        f = d - i
        return (1 - f) * self.rd(k, t, i) + f * self.rd(k, t, i + 1)

    def step(self, t, a0, a1, a2, xa, ya, xb, yb, xc):
        w, rd, c = self.w, self.rd, self.c
        p = self.prog
        if p == 0:  # reverb only: ser1 is dry-ish and enters the reverb on the same frame
            d_out, d_rev = (a1[0], a1[1]), (a1[0], a1[1])
        else:
            d_out = d_rev = (self.g115, self.g116)
        out = (a0[0] + self.t125 * d_out[0] + self.t123 * a2[0] + self.g89,
               a0[1] + self.t125 * d_out[1] + self.t123 * a2[1] + self.g90)
        if p == 5:
            w[29][t] = a1[0] + self.fb * self.g130
            w[34][t] = a1[1] + self.fb * self.g129
        elif p == 6:
            self.g113 = .75 * self.g113 + .00683594 * self.g112
            xe = a1[0] + self.g113
            y = sat(4 * sat(4 * a1[0]))
            w[29][t] = xe + self.fb * self.g111
            w[34][t] = a1[1] + self.fb * self.g105
        u = c["k"] * (1 - c["leak"]) * (self.t124 * (d_rev[0] + d_rev[1]) + self.t122 * (a2[0] + a2[1])) + c["leak"] * self.g67
        ret = rd(48, t, 1105)
        v = u + self.g110 * (self.g2 * ret + self.g1 * self.g70) + c["c1"] * self.g70 + c["c2"] * self.g71
        g = c["g"]
        d1, d2 = rd(64, t, 168), rd(69, t, 276)
        w[64][t] = v + g * d1
        w[69][t] = -g * v + (1 - g * g) * d1 + g * d2
        w[68][t] = g * g * v - g * (1 - g * g) * d1 + (1 - g * g) * d2
        pp, q, r = rd(68, t, 755), rd(87, t, 549), rd(88, t, 1369)
        w[85][t] = -.5 * pp + .75 * q
        w[87][t] = -.25 * pp - .125 * q + .9375 * r
        w[88][t] = .25 * r + pp + .5 * q
        a, b, cc = rd(50, t, 1012), rd(85, t, 242), rd(53, t, 696)
        w[48][t] = .75 * a - .5 * b
        w[50][t] = -.125 * a + .9375 * cc - .25 * b
        w[53][t] = -.125 * a - .0625 * cc - .25 * b + .9375 * self.g91
        w[54][t] = .5 * a + .25 * cc + b + .25 * self.g91
        if p == 5:
            n128, n129 = self.tap(29, t, 70 + 40 * xa), self.tap(29, t, 303 + 40 * xb)
            n130, n131 = self.tap(34, t, 261 + 40 * ya), self.tap(34, t, 428 + 40 * yb)
            n115 = self.sin_th * .7071 * (self.g128 + self.g129) + self.cos_th * a1[0]
            n116 = self.sin_th * .7071 * (self.g130 + self.g131) + self.cos_th * a1[1]
            self.g128, self.g129, self.g130, self.g131 = n128, n129, n130, n131
            self.g115, self.g116 = n115, n116
        elif p == 6:
            n115 = self.sin_th * .7071 * self.g105 + self.cos_th * xe
            n116 = self.sin_th * .7071 * self.g111 + self.cos_th * a1[1]
            self.g105 = self.tap(29, t, self.echo - .00390625)
            self.g111 = rd(34, t, int(self.echo))
            self.g112 = sat(2 * y - .5 * y ** 3)
            self.g115, self.g116 = n115, n116
        if p == 6:
            self.g91 = self.tap(29, t, self.mod_center + 8 * xc)  # the program's modulated tap reads the L echo line
        else:
            self.g91 = self.tap(54, t, 921 + 8 * xc)
        self.g89 = c["tl"][0] * rd(48, t, 384) + c["tl"][1] * rd(68, t, 4) + c["tl"][2] * rd(85, t, 26)
        self.g90 = c["tr"][0] * rd(48, t, 1105) + c["tr"][1] * rd(68, t, 242) + c["tr"][2] * rd(85, t, 180)
        self.g70, self.g71 = .625 * ret + .375 * self.g70, .125 * self.g70 + .875 * self.g71
        self.g67 = u
        return out


class Model1:
    """ROM programs 1/2 (Kirameki presets 2 and 0): two cross-coupled reverb chains (figure of eight)."""

    # (name, ring) delays: chain X is the right channel input chain, chain Y the left one.
    def __init__(self, n, a, g, g2, decay, g108, t122, t123, t124, t125, leak=.0078125, depth=8.0, center=611.0):
        self.a, self.g, self.g2, self.decay, self.g108 = a, g, g2, decay, g108
        self.t122, self.t123, self.t124, self.t125, self.leak = t122, t123, t124, t125, leak
        self.depth, self.center = depth, center
        names = [13, 15, 18, 21, 23, 25, 26, 30, 34, 35, 44, 46, 49, 50, 69, 71, 72, 76, 80, 81]
        self.w = {k: np.zeros(n + 4) for k in names}
        self.uL = self.uR = self.lpY1 = self.lpY2 = self.lpX1 = self.lpX2 = 0.0
        self.mX = self.mY = self.wetL = self.wetR = 0.0

    def rd(self, k, t, d):
        return self.w[k][t - d] if t - d >= 0 else 0.0

    def tap(self, k, t, d):
        i = int(math.floor(d))
        f = d - i
        return (1 - f) * self.rd(k, t, i) + f * self.rd(k, t, i + 1)

    def chain(self, t, v, dd1, dd2, wd, wo, w1, wd1, wd2, blk, mod):
        """Diffuser pair (nodes wd, wo, w1; delays dd1/dd2), nested all-pass block B, nested all-pass block A."""
        a, g, g2, rd, w = self.a, self.g, self.g2, self.rd, self.w
        d1, d2 = rd(wd, t, dd1), rd(w1, t, dd2)
        w[wd][t] = v + g * d1
        o1 = -g * v + (1 - g * g) * d1
        w[w1][t] = o1 + g2 * d2
        w[wo][t] = (1 - g2 * g2) * d2 - g2 * o1
        # block B: P = wo delayed, Q/R its own nodes
        (pn, pd, qn, qd, rn, rdl, outn) = blk[0]
        P, Q, R = rd(pn, t, pd), rd(qn, t, qd), rd(rn, t, rdl)
        w[outn][t] = -a * P + (1 - a * a) * Q
        w[qn][t] = -.25 * P - a / 4 * Q + .9375 * R
        w[rn][t] = .25 * R + P + a * Q
        # block A with modulated input
        (an, ad, bn, bd, cn, cd, o2, o3, o4) = blk[1]
        A, B, C = rd(an, t, ad), rd(bn, t, bd), rd(cn, t, cd)
        w[o2][t] = (1 - a * a) * A - a * B
        w[an][t] = -a / 4 * A + .9375 * C - .25 * B
        w[cn][t] = -a / 4 * A - .0625 * C - .25 * B + .9375 * mod
        w[o4][t] = a * A + .25 * C + B + .25 * mod

    def step(self, t, a0, a1, a2, xc_prev):
        rd, w = self.rd, self.w
        out = (a0[0] + self.t125 * a1[0] + self.t123 * a2[0] + self.wetL,
               a0[1] + self.t125 * a1[1] + self.t123 * a2[1] + self.wetR)
        vX = .5 * self.uR + self.decay * self.lpX1 + self.g108 * self.lpX2
        vY = .5 * self.uL + self.decay * self.lpY1 + self.g108 * self.lpY2
        # chain X: diffuser 88/141 (W30, W34 out, W35), block B (W34 in 628, W25 232, W26 874 -> W23), block A (W15 650, W23 214, W18 369)
        self.chain(t, vX, 88, 141, 30, 34, 35, 88, 141, [(34, 628, 25, 232, 26, 874, 23), (15, 650, 23, 214, 18, 369, 13, 15, 21)], self.mX)
        self.chain(t, vY, 102, 168, 76, 80, 81, 102, 168, [(80, 449, 71, 335, 72, 836, 69), (46, 617, 69, 145, 49, 424, 44, 46, 50)], self.mY)
        uL = self.leak * self.uL + (1 - self.leak) * (self.t124 * a1[0] + self.t122 * a2[0])
        uR = self.leak * self.uR + (1 - self.leak) * (self.t124 * a1[1] + self.t122 * a2[1])
        w13, w44 = rd(13, t, 7), rd(44, t, 675)
        self.lpY1, self.lpY2 = .5 * w13 + .5 * self.lpY1, .125 * w13 + .875 * self.lpY2
        self.lpX1, self.lpX2 = .5 * w44 + .5 * self.lpX1, .125 * w44 + .875 * self.lpX2
        d90 = self.center + self.depth * xc_prev
        self.mX, self.mY = self.tap(21, t, d90), self.tap(50, t, d90 - 136)
        self.wetL = .125 * rd(13, t, 3) + .4375 * rd(23, t, 163) + .9375 * rd(34, t, 101) - .4375 * rd(69, t, 131)
        self.wetR = -.4375 * rd(23, t, 74) + .125 * rd(44, t, 225) + .4375 * rd(69, t, 10) + .5 * rd(80, t, 1)
        self.uL, self.uR = uL, uR
        return out


# --------------------------------------------------------------------------------------------------------------
def reg_name(r: int) -> str:
    return REG_NAMES.get(r, f"G{r}")


def disasm(esp: Esp) -> None:
    print(f"memshift {esp.memshift} dlength {esp.dlength:#x} sigreg {esp.sigreg:#x} (mulshift {esp.mulshift})")
    for i in range(160):
        x = esp.instr[i]
        op = (x >> 12) & 15
        cyc, acc = RAM_CONTROL[(x >> 3) & 7]
        sel = OPERAND_SELECT[(x >> 8) & 15]
        c, d, a, b = (x >> 32) & 255, (x >> 40) & 255, (x >> 16) & 255, (x >> 24) & 255
        cs = "DIL" if sel[2] == "D" else reg_name(c)
        mdst = {"R": reg_name(c), "D": "DOL", "B": reg_name(c) + "+DOL"}[sel[3]]
        asrc = "DIL" if sel[0] == "D" else reg_name(a)
        adst = {"R": reg_name(a), "D": "DOL", "B": reg_name(a) + "+DOL"}[sel[1]]
        ram = {"R": "read", "W": "WRITE", "F": "read+dump"}[cyc] if acc == "D" else f"{cyc}/{acc}"
        print(f"{i:2d} {ram:9s} off={s24(esp.g[i]) / 256:9.2f} | {mdst} = {'acc+' if (x >> 6) & 1 else ''}{cs}*{reg_name(d)}"
              f" | {adst} <- {ALU_NAMES[op]} {asrc},{reg_name(b)}")
        if op == 15:
            break


def lin_str(d: Lin) -> str:
    def nm(v):
        return SER_NAMES[v[1]] if v[0] == "in" else f"W{v[1]}[z-{v[2]}]" if v[0] == "R" else f"N{v[1]}" if v[0] == "N" \
            else f"G{v[1]}'"
    return " + ".join(f"{c:.6g}*{nm(v)}" for v, c in sorted(d.items(), key=lambda kv: str(kv[0])))


def cmd_graph(esp: Esp, warm: int) -> None:
    esp.sym = True
    for _ in range(warm):
        esp.run_frame([0] * 6)
    for _ in range(3):
        esp.run_frame([0] * 6)
    f = esp.frame - 3
    print("# registers G' are the value latched at the end of the previous frame; W<n>[z-d] is the delay RAM write of "
          "instruction n, d frames ago;\n# delays that move with an LFO are printed at their value in this frame.")
    for k, v in sorted(esp.outlin.items()):
        print(f"OUT {SER_NAMES[k]} = {lin_str(v)}")
    for k, v in sorted(esp.statedef.items(), key=lambda kv: str(kv[0])):
        print(f"G{k}' = {lin_str(v)}")
    for k, v in sorted(esp.late.get(f, {}).items()):
        if v:
            print(f"W{k} = {lin_str(v)}")
    for k, (what, a, b) in sorted(esp.nonlinear.items()):
        print(f"N{k} = {what}(a = {lin_str(a)} ; b = {lin_str(b)})")


def cmd_verify(esp: Esp, inp: int, frames: int, warm: int, echo: float) -> None:
    prog = detect_program(esp)
    if prog < 0:
        raise SystemExit("loaded program is not one of the decoded programs (0, 1, 5, 6)")
    esp.dram[:] = 0
    for _ in range(warm):
        esp.run_frame([0] * 6)
    rg = lambda r: real(esp.g[r])
    g1 = rg(1)
    if prog == 0:
        model = Model(0, frames, rg(110), g1, rg(122), rg(123), rg(124), rg(125))
        lfo = lambda: (0, 0, 0, 0, rg(34))
        sample_xc = 34
    elif prog == 1:
        model = Model1(frames, rg(5), rg(8), rg(17), rg(110), rg(108), rg(122), rg(123), rg(124), rg(125), rg(3),
                       real(esp.g[109]) * 2**23 / 256, s24(esp.g[0]) / 256)
        lfo = lambda: (0, 0, 0, 0, rg(32))
        sample_xc = 32
    elif prog == 5:
        model = Model(5, frames, rg(110), g1, rg(122), rg(123), rg(124), rg(125), rg(134), rg(118), rg(119))
        lfo = lambda: (rg(49), rg(111), rg(51), rg(112), rg(35))
        sample_xc = 35
    else:
        model = Model(6, frames, rg(110), g1, rg(122), rg(123), rg(124), rg(125), rg(134), rg(118), rg(119), echo,
                      s24(esp.g[0]) / 256)
        lfo = lambda: (0, 0, 0, 0, rg(20))
        sample_xc = 20
    xc_prev = rg(sample_xc)
    ref, out = [], []
    for t in range(frames):
        ins = [20000 if (t == 0 and i == inp) else 0 for i in range(6)]
        ref.append(esp.run_frame(ins))
        x = np.array(ins) / 32768.0
        xa, ya, xb, yb, xc = lfo()
        if prog == 1:
            out.append(model.step(t, (x[0], x[1]), (x[2], x[3]), (x[4], x[5]), xc_prev))
        else:
            out.append(model.step(t, (x[0], x[1]), (x[2], x[3]), (x[4], x[5]), xa, ya, xb, yb, xc_prev))
        xc_prev = xc
    ref = np.array(ref) / 32768.0
    out = np.array(out)
    floor = np.median(ref[frames // 2:], axis=0)  # truncation DC of the 16-bit delay RAM
    err = out + floor - ref
    print(f"program {prog}, impulse on {SER_NAMES[inp]}: emulator peak {abs(ref - floor).max():.4f}, max error "
          f"{abs(err).max():.2e} ({20 * math.log10(abs(err).max() / abs(ref - floor).max()):.1f} dB re peak)")
    for a, b in [(1, 100), (100, 1000), (1000, 4000), (4000, frames)]:
        if b > a:
            print(f"  frames {a:5d}..{b:5d}: emulator rms {np.sqrt(((ref[a:b] - floor) ** 2).mean()):.3e}, "
                  f"error rms {np.sqrt((err[a:b] ** 2).mean()):.3e}")


def cmd_programs(trace: str) -> None:
    """Every program the driver loaded, with the sample of its last instruction write and its instruction count."""
    esp = Esp()
    state: dict[str, Any] = {"last": -10**9, "burst": None}
    loads: list[tuple[int, tuple[int, ...]]] = []

    def flush() -> None:
        if state["burst"] is not None:
            loads.append((state["burst"], esp.program_key()))
            state["burst"] = None

    def on_write(sample: int, offset: int) -> None:
        if offset in (0xC0, 0xE0):
            if sample - state["last"] > 20000:
                flush()
            state["last"] = sample
            state["burst"] = sample

    esp.replay_trace(trace, on_write=on_write)
    flush()
    seen: dict[tuple[int, ...], int] = {}
    for sample, key in loads:
        seen.setdefault(key, len(seen))
        known = {0xED7DEF399128: "program 0", 0x737DEF4E9128: "program 5", 0x617DEF3F9128: "program 6"}.get(
            key[1] if len(key) > 1 else 0, "undecoded")
        print(f"sample {sample:9d}: loaded #{seen[key]} ({len(key)} instructions, {known})")
    print(f"{len(loads)} loads, {len(seen)} distinct")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace", type=Path, help="F3SND2 bus trace of a reference-backend run")
    ap.add_argument("command", choices=["disasm", "graph", "verify", "programs"])
    ap.add_argument("--until-sample", type=int, default=None, help="replay host writes up to this sample (default: all)")
    ap.add_argument("--warm", type=int, default=None, help="warm-up frames (graph 70000, verify 3000)")
    ap.add_argument("--input", type=int, default=4, choices=range(6),
                    help="verify: impulse on ser_w index (0 ser0l .. 4 ser2l .. 5 ser2r), default 4")
    ap.add_argument("--frames", type=int, default=8000, help="verify: frames to compare")
    ap.add_argument("--echo", type=float, default=8176.0, help="verify, program 6: echo delay in frames (p2 * 112)")
    args = ap.parse_args()
    if args.command == "programs":
        cmd_programs(str(args.trace))
        return
    esp = Esp()
    esp.replay_trace(str(args.trace), args.until_sample)
    if args.command == "disasm":
        disasm(esp)
    elif args.command == "graph":
        cmd_graph(esp, 70000 if args.warm is None else args.warm)
    else:
        cmd_verify(esp, args.input, args.frames, 3000 if args.warm is None else args.warm, args.echo)


if __name__ == "__main__":
    sys.exit(main())
