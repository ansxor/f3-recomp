//! `es5510.cpp` - Ensoniq ES5510 (ESP) Digital Signal Processor.

use crate::state_io::{StateReader, StateWriter, require_state};

const FLAG_N: u8 = 1 << 7;
const FLAG_C: u8 = 1 << 6;
const FLAG_V: u8 = 1 << 5;
const FLAG_LT: u8 = 1 << 4;
const FLAG_Z: u8 = 1 << 3;
const FLAG_NOT: u8 = 1 << 2;

const FLAG_MASK: u8 = FLAG_N | FLAG_C | FLAG_V | FLAG_LT | FLAG_Z;

fn set_flag(ccr: u8, flag: u8) -> u8 {
    ccr | flag
}

fn clear_flag(ccr: u8, flag: u8) -> u8 {
    ccr & !flag
}

fn set_flag_to(ccr: u8, flag: u8, set: bool) -> u8 {
    if set { set_flag(ccr, flag) } else { clear_flag(ccr, flag) }
}

fn is_flag_set(ccr: u8, flag: u8) -> bool {
    (ccr & flag) != 0
}

fn sext24(val: i32) -> i32 {
    ((val & 0x00ffffff) ^ 0x00800000).wrapping_sub(0x00800000)
}

fn sext48(val: i64) -> i64 {
    if val & 0x0000800000000000 != 0 {
        val | !0x0000ffffffffffff
    } else {
        val & 0x0000ffffffffffff
    }
}

fn add(a: i32, b: i32, flags: &mut u8) -> i32 {
    let a_sign = a & 0x00800000;
    let b_sign = b & 0x00800000;
    let result = a.wrapping_add(b);
    let result_sign = result & 0x00800000;
    let overflow = (a_sign == b_sign) && (a_sign != result_sign);
    let carry = (result & 0x01000000) != 0;
    let negative = result_sign != 0;
    let less_than = overflow != negative;
    *flags = (*flags & !FLAG_MASK)
        | ((carry as u8) * FLAG_C)
        | ((negative as u8) * FLAG_N)
        | (((result & 0x00ffffff) == 0) as u8 * FLAG_Z)
        | ((overflow as u8) * FLAG_V)
        | ((less_than as u8) * FLAG_LT);
    result & 0x00ffffff
}

fn round_24_to_16(dol24: i32) -> i16 {
    (dol24 >> 8) as i16
}

fn saturate(value: i32, flags: &mut u8, negative: bool) -> i32 {
    if is_flag_set(*flags, FLAG_V) {
        *flags = set_flag_to(*flags, FLAG_N, negative);
        *flags = clear_flag(*flags, FLAG_Z);
        if negative { 0x00800000 } else { 0x007fffff }
    } else {
        value
    }
}

fn negate(value: i32) -> i32 {
    ((value ^ 0x00ffffff).wrapping_add(1)) & 0x00ffffff
}

fn asl(value: i32, shift: i32, flags: &mut u8) -> i32 {
    let src24 = value & 0x00ffffff;
    let carry = (src24 & (1 << (24 - shift))) != 0;
    let shifted = (sext24(src24) as i64) << shift;
    let overflow = (shifted > 0x007fffff) || (shifted < -0x00800000);
    let result = if overflow {
        if shifted < 0 { 0x00800000 } else { 0x007fffff }
    } else {
        (shifted as i32) & 0x00ffffff
    };

    *flags = set_flag_to(*flags, FLAG_C, carry);
    *flags = set_flag_to(*flags, FLAG_V, overflow);
    *flags = set_flag_to(*flags, FLAG_N, (result & 0x00800000) != 0);
    *flags = set_flag_to(*flags, FLAG_Z, (result & 0x00ffffff) == 0);

    result
}

fn count_low_ones(mut x: i32) -> i8 {
    let mut n: i8 = 0;
    while (x & 1) == 1 {
        n = n.wrapping_add(1);
        x >>= 1;
    }
    n
}

const OP_CMP: u8 = 4;

pub type OpSrcDst = u8;
pub const SRC_DST_REG: OpSrcDst = 1 << 0;
pub const SRC_DST_DELAY: OpSrcDst = 1 << 1;
pub const SRC_DST_BOTH: OpSrcDst = (1 << 0) | (1 << 1);

pub type RamControlAccess = u8;
pub const RAM_CONTROL_DELAY: RamControlAccess = 0;
pub const RAM_CONTROL_TABLE_A: RamControlAccess = 1;
pub const RAM_CONTROL_TABLE_B: RamControlAccess = 2;
pub const RAM_CONTROL_IO: RamControlAccess = 3;

pub type RamCycle = u8;
pub const RAM_CYCLE_READ: RamCycle = 0;
pub const RAM_CYCLE_WRITE: RamCycle = 1;
pub const RAM_CYCLE_DUMP_FIFO: RamCycle = 2;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum State {
    Running = 0,
    Halted = 1,
}

pub const STATE_RUNNING: State = State::Running;
pub const STATE_HALTED: State = State::Halted;

#[derive(Clone, Copy)]
pub struct OpSelect {
    pub alu_src: OpSrcDst,
    pub alu_dst: OpSrcDst,
    pub mac_src: OpSrcDst,
    pub mac_dst: OpSrcDst,
}

#[derive(Clone, Copy)]
pub struct RamControl {
    pub cycle: RamCycle,
    pub access: RamControlAccess,
    pub description: &'static str,
}

const fn op_select(alu_src: OpSrcDst, alu_dst: OpSrcDst, mac_src: OpSrcDst, mac_dst: OpSrcDst) -> OpSelect {
    OpSelect { alu_src, alu_dst, mac_src, mac_dst }
}

const OPERAND_SELECT: [OpSelect; 16] = [
    op_select(SRC_DST_REG, SRC_DST_REG, SRC_DST_REG, SRC_DST_REG),
    op_select(SRC_DST_REG, SRC_DST_REG, SRC_DST_REG, SRC_DST_DELAY),
    op_select(SRC_DST_REG, SRC_DST_REG, SRC_DST_REG, SRC_DST_BOTH),
    op_select(SRC_DST_REG, SRC_DST_REG, SRC_DST_DELAY, SRC_DST_REG),
    op_select(SRC_DST_REG, SRC_DST_REG, SRC_DST_DELAY, SRC_DST_BOTH),
    op_select(SRC_DST_REG, SRC_DST_DELAY, SRC_DST_REG, SRC_DST_REG),
    op_select(SRC_DST_REG, SRC_DST_DELAY, SRC_DST_DELAY, SRC_DST_REG),
    op_select(SRC_DST_REG, SRC_DST_BOTH, SRC_DST_REG, SRC_DST_REG),
    op_select(SRC_DST_REG, SRC_DST_BOTH, SRC_DST_DELAY, SRC_DST_REG),
    op_select(SRC_DST_DELAY, SRC_DST_REG, SRC_DST_REG, SRC_DST_REG),
    op_select(SRC_DST_DELAY, SRC_DST_REG, SRC_DST_REG, SRC_DST_DELAY),
    op_select(SRC_DST_DELAY, SRC_DST_REG, SRC_DST_REG, SRC_DST_BOTH),
    op_select(SRC_DST_DELAY, SRC_DST_REG, SRC_DST_DELAY, SRC_DST_REG),
    op_select(SRC_DST_DELAY, SRC_DST_REG, SRC_DST_DELAY, SRC_DST_BOTH),
    op_select(SRC_DST_DELAY, SRC_DST_BOTH, SRC_DST_REG, SRC_DST_REG),
    op_select(SRC_DST_DELAY, SRC_DST_BOTH, SRC_DST_DELAY, SRC_DST_REG),
];

const fn ram_control(cycle: RamCycle, access: RamControlAccess, description: &'static str) -> RamControl {
    RamControl { cycle, access, description }
}

const RAM_CONTROL: [RamControl; 8] = [
    ram_control(RAM_CYCLE_READ, RAM_CONTROL_DELAY, "Read Delay+%06x"),
    ram_control(RAM_CYCLE_WRITE, RAM_CONTROL_DELAY, "Write Delay+%06x"),
    ram_control(RAM_CYCLE_READ, RAM_CONTROL_TABLE_A, "Read Table A+%06x"),
    ram_control(RAM_CYCLE_WRITE, RAM_CONTROL_TABLE_A, "Write Table A+%06x"),
    ram_control(RAM_CYCLE_READ, RAM_CONTROL_TABLE_B, "Read Table B+%06x"),
    ram_control(RAM_CYCLE_DUMP_FIFO, RAM_CONTROL_DELAY, "Read Delay+%06x and Dump FIFO"),
    ram_control(RAM_CYCLE_READ, RAM_CONTROL_IO, "Read from I/O at %06x"),
    ram_control(RAM_CYCLE_WRITE, RAM_CONTROL_IO, "Write to I/O at %06x"),
];

#[derive(Clone, Copy, Default)]
pub struct AluState {
    pub a_reg: u8,
    pub b_reg: u8,
    pub src: OpSrcDst,
    pub dst: OpSrcDst,
    pub op: u8,
    pub a_value: i32,
    pub b_value: i32,
    pub result: i32,
    pub update_ccr: bool,
    pub write_result: bool,
}

#[derive(Clone, Copy, Default)]
pub struct MulAccState {
    pub c_reg: u8,
    pub d_reg: u8,
    pub src: OpSrcDst,
    pub dst: OpSrcDst,
    pub accumulate: bool,
    pub c_value: i32,
    pub d_value: i32,
    pub product: i64,
    pub result: i64,
    pub write_result: bool,
}

#[derive(Clone, Copy, Default)]
pub struct RamState {
    pub address: i32,
    pub io: bool,
    pub cycle: RamCycle,
}

pub struct ES5510 {
    halt_asserted: bool,
    pc: u8,
    state: State,

    m_gpr: Box<[i32]>,
    m_instr: Box<[u64]>,
    m_dram: Box<[i16]>,

    ser0r: i16,
    ser0l: i16,
    ser1r: i16,
    ser1l: i16,
    ser2r: i16,
    ser2l: i16,
    ser3r: i16,
    ser3l: i16,

    machl: i64,
    mac_overflow: bool,
    dil: i16,
    memsiz: i32,
    memmask: i32,
    memincrement: i32,
    memshift: i8,
    dlength: i32,
    abase: i32,
    bbase: i32,
    dbase: i32,
    sigreg: i32,
    mulshift: i32,
    ccr: i8,
    cmr: i8,
    dol: [i16; 2],
    dol_count: i32,

    dol_latch: i32,
    dil_latch: i32,
    dadr_latch: u32,
    gpr_latch: i32,
    instr_latch: u64,
    ram_sel: u8,
    host_control: u8,
    host_serial: u8,

    alu: AluState,
    mulacc: MulAccState,
    ram: RamState,
    ram_p: RamState,
    ram_pp: RamState,
}

impl Default for ES5510 {
    fn default() -> Self {
        Self::new()
    }
}

impl ES5510 {
    pub const DRAM_SIZE: u32 = 1 << 20;
    pub const DRAM_MASK: u32 = ES5510::DRAM_SIZE - 1;

    const REGISTERS_STATE_SIZE: usize = 171;

    pub fn new() -> Self {
        ES5510 {
            halt_asserted: false,
            pc: 0,
            state: STATE_RUNNING,
            m_gpr: vec![0i32; 0xc0].into_boxed_slice(),
            m_instr: vec![0u64; 160].into_boxed_slice(),
            m_dram: vec![0i16; ES5510::DRAM_SIZE as usize].into_boxed_slice(),
            ser0r: 0,
            ser0l: 0,
            ser1r: 0,
            ser1l: 0,
            ser2r: 0,
            ser2l: 0,
            ser3r: 0,
            ser3l: 0,
            machl: 0,
            mac_overflow: false,
            dil: 0,
            memsiz: 0x00ffffff,
            memmask: 0x00000000,
            memincrement: 0x01000000,
            memshift: 24,
            dlength: 0,
            abase: 0,
            bbase: 0,
            dbase: 0,
            sigreg: 0,
            mulshift: 2,
            ccr: 0,
            cmr: 0,
            dol: [0; 2],
            dol_count: 0,
            dol_latch: 0,
            dil_latch: 0,
            dadr_latch: 0,
            gpr_latch: 0,
            instr_latch: 0,
            ram_sel: 0,
            host_control: 0x04,
            host_serial: 0,
            alu: AluState::default(),
            mulacc: MulAccState::default(),
            ram: RamState::default(),
            ram_p: RamState::default(),
            ram_pp: RamState::default(),
        }
    }

    pub fn reset(&mut self) {
        self.pc = 0;
        self.state = STATE_RUNNING;
        self.sigreg = 0;
        self.mulshift = 2;
        self.dol_latch = 0;
        self.dil_latch = 0;
        self.dadr_latch = 0;
        self.gpr_latch = 0;
        self.instr_latch = 0;
        self.ram_sel = 0;
        self.host_control = 0x04;
        self.host_serial = 0;

        self.m_gpr.fill(0);
        self.m_instr.fill(0);
        self.m_dram.fill(0);
        self.ram = RamState::default();
        self.ram_p = RamState::default();
        self.ram_pp = RamState::default();
    }

    pub fn host_r(&mut self, offset: u32) -> u8 {
        match offset {
            0x00 => ((self.gpr_latch >> 16) & 0xff) as u8,
            0x01 => ((self.gpr_latch >> 8) & 0xff) as u8,
            0x02 => ((self.gpr_latch >> 0) & 0xff) as u8,

            0x03 => ((self.instr_latch >> 40) & 0xff) as u8,
            0x04 => ((self.instr_latch >> 32) & 0xff) as u8,
            0x05 => ((self.instr_latch >> 24) & 0xff) as u8,
            0x06 => ((self.instr_latch >> 16) & 0xff) as u8,
            0x07 => ((self.instr_latch >> 8) & 0xff) as u8,
            0x08 => ((self.instr_latch >> 0) & 0xff) as u8,

            0x09 => ((self.dil_latch >> 16) & 0xff) as u8,
            0x0a => ((self.dil_latch >> 8) & 0xff) as u8,
            0x0b => 0,

            0x0c => ((self.dol_latch >> 16) & 0xff) as u8,
            0x0d => ((self.dol_latch >> 8) & 0xff) as u8,
            0x0e => 0xff,

            0x0f => ((self.dadr_latch >> 16) & 0xff) as u8,
            0x10 => ((self.dadr_latch >> 8) & 0xff) as u8,
            0x11 => ((self.dadr_latch >> 0) & 0xff) as u8,

            0x12 => 0,

            0x16 => 0x27,
            _ => 0x00,
        }
    }

    pub fn host_w(&mut self, offset: u32, data: u8) {
        let d = data as u32;
        match offset {
            0x00 => self.gpr_latch = ((self.gpr_latch as u32 & 0x00ffff) | ((d & 0xff) << 16)) as i32,
            0x01 => self.gpr_latch = ((self.gpr_latch as u32 & 0xff00ff) | ((d & 0xff) << 8)) as i32,
            0x02 => self.gpr_latch = ((self.gpr_latch as u32 & 0xffff00) | ((d & 0xff) << 0)) as i32,

            0x03 => self.instr_latch = (self.instr_latch & 0x00ffffffffff) | (data as u64 & 0xff) << 40,
            0x04 => self.instr_latch = (self.instr_latch & 0xff00ffffffff) | (data as u64 & 0xff) << 32,
            0x05 => self.instr_latch = (self.instr_latch & 0xffff00ffffff) | (data as u64 & 0xff) << 24,
            0x06 => self.instr_latch = (self.instr_latch & 0xffffff00ffff) | (data as u64 & 0xff) << 16,
            0x07 => self.instr_latch = (self.instr_latch & 0xffffffff00ff) | (data as u64 & 0xff) << 8,
            0x08 => self.instr_latch = (self.instr_latch & 0xffffffffff00) | (data as u64 & 0xff) << 0,

            0x0c => self.dol_latch = ((self.dol_latch as u32 & 0x00ffff) | ((d & 0xff) << 16)) as i32,
            0x0d => self.dol_latch = ((self.dol_latch as u32 & 0xff00ff) | ((d & 0xff) << 8)) as i32,
            0x0e => self.dol_latch = ((self.dol_latch as u32 & 0xffff00) | ((d & 0xff) << 0)) as i32,

            0x0f => {
                self.dadr_latch = (self.dadr_latch & 0x00ffff) | ((d & 0xff) << 16);
                if self.ram_sel != 0 {
                    self.dil_latch = (self.dram_r(self.dadr_latch as i32) as i32) << 8;
                } else {
                    self.dram_w(self.dadr_latch as i32, (self.dol_latch >> 8) as i16);
                }
            }
            0x10 => self.dadr_latch = (self.dadr_latch & 0xff00ff) | ((d & 0xff) << 8),
            0x11 => self.dadr_latch = (self.dadr_latch & 0xffff00) | ((d & 0xff) << 0),

            0x12 => {
                self.host_control = (self.host_control & 0x4) | (data & 0x3);
                if self.host_control & 0x02 != 0 {
                    if self.state == STATE_HALTED {
                        self.m_dram.fill(0);
                    }
                    self.host_control &= !0x02;
                }
            }

            0x14 => self.ram_sel = data & 0x80,
            0x18 => self.host_serial = data,

            0x1f => {
                if self.halt_asserted {
                    self.state = STATE_HALTED;
                }
            }

            0x80 => {
                if data < 0xa0 {
                    self.instr_latch = self.m_instr[data as usize];
                }
                if data < 0xc0 {
                    self.gpr_latch = self.m_gpr[data as usize] & 0xffffff;
                } else if data >= 0xea {
                    self.gpr_latch = self.read_reg(data);
                }
            }

            0xa0 => self.write_reg(data, self.gpr_latch),

            0xc0 => {
                if data < 0xa0 {
                    self.m_instr[data as usize] = self.instr_latch & 0xffffffffffff;
                }
            }

            0xe0 => {
                if data < 0xa0 {
                    self.m_instr[data as usize] = self.instr_latch & 0xffffffffffff;
                }
                self.write_reg(data, self.gpr_latch);
            }
            _ => {}
        }
    }

    pub fn ser_r(&self, offset: i32) -> i16 {
        match offset {
            0 => self.ser0l,
            1 => self.ser0r,
            2 => self.ser1l,
            3 => self.ser1r,
            4 => self.ser2l,
            5 => self.ser2r,
            6 => self.ser3l,
            7 => self.ser3r,
            _ => 0,
        }
    }

    pub fn ser_w(&mut self, offset: i32, data: i16) {
        match offset {
            0 => self.ser0l = data,
            1 => self.ser0r = data,
            2 => self.ser1l = data,
            3 => self.ser1r = data,
            4 => self.ser2l = data,
            5 => self.ser2r = data,
            6 => self.ser3l = data,
            7 => self.ser3r = data,
            _ => {}
        }
    }

    #[allow(non_snake_case)]
    pub fn set_HALT(&mut self, halt: bool) {
        self.halt_asserted = halt;
    }

    #[allow(non_snake_case)]
    pub fn get_HALT(&self) -> bool {
        self.halt_asserted
    }

    pub fn get_state(&self) -> State {
        self.state
    }

    pub fn read_reg(&mut self, reg: u8) -> i32 {
        if reg < 0xc0 {
            return self.m_gpr[reg as usize];
        }
        match reg {
            234 => (self.ser0r as i32) << 8,
            235 => (self.ser0l as i32) << 8,
            236 => (self.ser1r as i32) << 8,
            237 => (self.ser1l as i32) << 8,
            238 => (self.ser2r as i32) << 8,
            239 => (self.ser2l as i32) << 8,
            240 => (self.ser3r as i32) << 8,
            241 => (self.ser3l as i32) << 8,
            242 => {
                (if self.mac_overflow {
                    if self.machl < 0 { 0x00000000 } else { 0x00ffffff }
                } else {
                    (self.machl >> 0) & 0x00ffffff
                }) as i32
            }
            243 => {
                (if self.mac_overflow {
                    if self.machl < 0 { 0x00800000 } else { 0x007fffff }
                } else {
                    (self.machl >> 24) & 0x00ffffff
                }) as i32
            }
            244 => (self.dil as i32) << 8,
            245 => self.dlength,
            246 => self.abase,
            247 => self.bbase,
            248 => self.dbase,
            249 => self.sigreg,
            250 => (self.ccr as i32) << 16,
            251 => (self.cmr as i32) << 16,
            252 => 0x00ffffff,
            253 => 0x00800000,
            254 => 0x007fffff,
            255 => 0,
            _ => 0,
        }
    }

    pub fn write_to_dol(&mut self, value: i32) {
        let dol16 = round_24_to_16(value);
        if self.dol_count >= 2 {
            self.dol[0] = self.dol[1];
            self.dol[1] = dol16;
        } else {
            self.dol[self.dol_count as usize] = dol16;
            self.dol_count += 1;
        }
    }

    pub fn write_reg(&mut self, reg: u8, mut value: i32) {
        value &= 0x00ffffff;
        if reg < 0xc0 {
            self.m_gpr[reg as usize] = value;
        } else {
            match reg {
                234 => self.ser0r = ((value >> 8) & 0xffff) as i16,
                235 => self.ser0l = ((value >> 8) & 0xffff) as i16,
                236 => self.ser1r = ((value >> 8) & 0xffff) as i16,
                237 => self.ser1l = ((value >> 8) & 0xffff) as i16,
                238 => self.ser2r = ((value >> 8) & 0xffff) as i16,
                239 => self.ser2l = ((value >> 8) & 0xffff) as i16,
                240 => self.ser3r = ((value >> 8) & 0xffff) as i16,
                241 => self.ser3l = ((value >> 8) & 0xffff) as i16,
                242 => {
                    let masked = self.machl & ((0x00ffffffu32 as i64) << 24);
                    let shifted = (value & 0x00ffffff) as i64;
                    self.machl = sext48(masked | shifted);
                }
                243 => {
                    let masked = self.machl & ((0x00ffffffu32 as i64) << 0);
                    let shifted = ((value & 0x00ffffff) as i64) << 24;
                    self.machl = sext48(masked | shifted);
                    self.mac_overflow = false;
                }
                244 => {
                    self.memshift = count_low_ones(value);
                    self.memsiz = 0x00ffffff >> (24 - self.memshift as i32);
                    self.memmask = 0x00ffffff & !self.memsiz;
                    self.memincrement = 1 << self.memshift;
                    self.dbase &= self.memmask;
                }
                245 => self.dlength = value,
                246 => self.abase = value,
                247 => self.bbase = value,
                248 => self.dbase = value,
                249 => {
                    self.sigreg = value;
                    self.mulshift = if self.sigreg & (1 << 22) != 0 { 1 } else { 2 };
                }
                250 => self.ccr = ((value >> 16) & FLAG_MASK as i32) as i8,
                251 => self.cmr = ((value >> 16) & (FLAG_MASK | FLAG_NOT) as i32) as i8,
                _ => {}
            }
        }
    }

    pub fn instr_at(&mut self, pc: i32) -> &mut u64 {
        &mut self.m_instr[(pc % 160) as usize]
    }

    pub fn dram_at(&mut self, addr: i32) -> &mut i16 {
        &mut self.m_dram[(addr as u32 & ES5510::DRAM_MASK) as usize]
    }

    fn dram_r(&self, addr: i32) -> i16 {
        self.m_dram[(addr as u32 & ES5510::DRAM_MASK) as usize]
    }

    fn dram_w(&mut self, addr: i32, data: i16) {
        self.m_dram[(addr as u32 & ES5510::DRAM_MASK) as usize] = data;
    }

    fn alu_operation_end(&mut self) {
        if self.halt_asserted {
            self.state = STATE_HALTED;
            self.host_control &= !0x04;
        }
        self.dbase = self.dbase.wrapping_sub(self.memincrement);
        if self.dbase < 0 {
            self.dbase = self.dlength;
        }
        if self.state == STATE_RUNNING {
            self.pc = 0;
        }
    }

    fn alu_operation(&self, op: u8, mut a: i32, b: i32, flags: &mut u8) -> i32 {
        let tmp;
        match op {
            0x0 => {
                tmp = add(a, b, flags);
                saturate(tmp, flags, (a & 0x00800000) != 0)
            }
            0x1 => {
                tmp = add(a, negate(b), flags);
                saturate(tmp, flags, (a & 0x00800000) != 0)
            }
            0x2 => add(a, b, flags),
            0x3 => add(a, negate(b), flags),
            0x4 => {
                add(a, negate(b), flags);
                a
            }
            0x5 => {
                a &= b;
                *flags = set_flag_to(*flags, FLAG_N, (a & 0x00800000) != 0);
                *flags = set_flag_to(*flags, FLAG_Z, a == 0);
                a
            }
            0x6 => {
                a |= b;
                *flags = set_flag_to(*flags, FLAG_N, (a & 0x00800000) != 0);
                *flags = set_flag_to(*flags, FLAG_Z, a == 0);
                a
            }
            0x7 => {
                a ^= b;
                *flags = set_flag_to(*flags, FLAG_N, (a & 0x00800000) != 0);
                *flags = set_flag_to(*flags, FLAG_Z, a == 0);
                a
            }
            0x8 => {
                *flags = clear_flag(*flags, FLAG_N);
                let is_negative = (b & 0x00800000) != 0;
                *flags = set_flag_to(*flags, FLAG_C, is_negative);
                let result = if is_negative { 0x00ffffff ^ b } else { b };
                *flags = set_flag_to(*flags, FLAG_Z, (result & 0x00ffffff) == 0);
                result
            }
            0x9 => b,
            0xa => asl(b, 2, flags),
            0xb => asl(b, 8, flags),
            0xc => {
                *flags = clear_flag(*flags, FLAG_N);
                *flags = set_flag_to(*flags, FLAG_C, (b & 0x00800000) != 0);
                (b << 15) & 0x007fffff
            }
            0xd => add(0x007fffff, negate(b), flags),
            0xe => {
                *flags = set_flag_to(*flags, FLAG_N, (b & 0x00800000) != 0);
                *flags = set_flag_to(*flags, FLAG_C, (b & 1) != 0);
                (b >> 1) | (b & 0x00800000)
            }
            _ => 0,
        }
    }

    fn execute_run(&mut self, mut cycles: i32) {
        while cycles > 0 {
            if self.state == STATE_HALTED {
                if self.halt_asserted {
                    self.host_control &= !0x04;
                    break;
                } else {
                    self.state = STATE_RUNNING;
                    self.host_control |= 0x04;
                    self.pc = 0;
                }
            } else {
                self.ram_pp = self.ram_p;
                self.ram_p = self.ram;

                let instr_index = if self.pc < 160 { self.pc as usize } else { self.pc as usize - 160 };
                let instr = self.m_instr[instr_index];

                if self.ram_pp.cycle != RAM_CYCLE_WRITE {
                    if self.ram_pp.io {
                        self.dil = 0;
                    } else {
                        self.dil = self.dram_r(self.ram_pp.address);
                    }
                }

                let ram_control = RAM_CONTROL[((instr >> 3) & 0x07) as usize];
                self.ram.cycle = ram_control.cycle;
                self.ram.io = ram_control.access == RAM_CONTROL_IO;

                let offset = self.m_gpr[instr_index];
                match ram_control.access {
                    RAM_CONTROL_DELAY => {
                        let modulo = self.dlength.wrapping_add(self.memincrement);
                        let mut mod_result: i32 = 0;
                        if modulo > 0 {
                            mod_result = self.dbase.wrapping_add(offset);
                            if mod_result >= modulo {
                                mod_result = mod_result.wrapping_sub(modulo);
                            }
                            if (mod_result as u32) >= (modulo as u32) {
                                mod_result %= modulo;
                            }
                        }
                        self.ram.address = (mod_result & self.memmask) >> (self.memshift as u32);
                    }
                    RAM_CONTROL_TABLE_A => {
                        self.ram.address = (self.abase.wrapping_add(offset) & self.memmask) >> (self.memshift as u32);
                    }
                    RAM_CONTROL_TABLE_B => {
                        self.ram.address = (self.bbase.wrapping_add(offset) & self.memmask) >> (self.memshift as u32);
                    }
                    _ => {
                        self.ram.address = offset & 0x00fffff0;
                    }
                }

                let operand_select = ((instr >> 8) & 0x0f) as usize;
                let op_select = OPERAND_SELECT[operand_select];
                let mut skip = false;
                let skippable = (instr & (1 << 7)) != 0;
                if skippable {
                    skip = (((self.ccr as i32) & (self.cmr as i32) & FLAG_MASK as i32) != 0)
                        != is_flag_set(self.cmr as u8, FLAG_NOT);
                }

                if self.mulacc.write_result {
                    let c_sext = sext24(self.mulacc.c_value) as i64;
                    let d_sext = sext24(self.mulacc.d_value) as i64;
                    self.mulacc.product = c_sext.wrapping_mul(d_sext).wrapping_shl(self.mulshift as u32);
                    if self.mulacc.accumulate {
                        self.mulacc.result = self.mulacc.product.wrapping_add(self.machl);
                    } else {
                        self.mulacc.result = self.mulacc.product;
                    }

                    if self.mulacc.result < -(1i64 << 47) || self.mulacc.result >= (1i64 << 47) {
                        self.mac_overflow = true;
                    } else {
                        self.mac_overflow = false;
                    }

                    self.machl = self.mulacc.result;
                    let tmp: i32 = if self.mac_overflow {
                        if self.machl < 0 { 0x00800000 } else { 0x007fffff }
                    } else {
                        ((self.mulacc.result as u64 & 0x0000ffffff000000) >> 24) as i32
                    };

                    if self.mulacc.dst & SRC_DST_REG != 0 {
                        self.write_reg(self.mulacc.c_reg, tmp);
                    }
                    if self.mulacc.dst & SRC_DST_DELAY != 0 {
                        self.write_to_dol(tmp);
                    }
                }

                self.mulacc.c_reg = ((instr >> 32) & 0xff) as u8;
                self.mulacc.d_reg = ((instr >> 40) & 0xff) as u8;
                self.mulacc.src = op_select.mac_src;
                self.mulacc.dst = op_select.mac_dst;
                self.mulacc.accumulate = ((instr >> 6) & 0x01) != 0;
                self.mulacc.write_result = !skip;

                if self.mulacc.src == SRC_DST_REG {
                    self.mulacc.c_value = self.read_reg(self.mulacc.c_reg);
                } else {
                    self.mulacc.c_value = (self.dil as i32) << 8;
                }
                self.mulacc.d_value = self.read_reg(self.mulacc.d_reg);

                if self.alu.write_result {
                    let mut flags = self.ccr as u8;
                    self.alu.result = self.alu_operation(self.alu.op, self.alu.a_value, self.alu.b_value, &mut flags);
                    if self.alu.op != OP_CMP {
                        if self.alu.dst & SRC_DST_REG != 0 {
                            self.write_reg(self.alu.a_reg, self.alu.result);
                        }
                        if self.alu.dst & SRC_DST_DELAY != 0 {
                            self.write_to_dol(self.alu.result);
                        }
                    }
                    if self.alu.update_ccr {
                        self.ccr = flags as i8;
                    }
                }

                self.alu.a_reg = ((instr >> 16) & 0xff) as u8;
                self.alu.b_reg = ((instr >> 24) & 0xff) as u8;
                self.alu.op = ((instr >> 12) & 0x0f) as u8;
                self.alu.src = op_select.alu_src;
                self.alu.dst = op_select.alu_dst;
                self.alu.write_result = !skip || (self.alu.op == OP_CMP);
                self.alu.update_ccr = !skippable || (self.alu.op == OP_CMP);

                if self.alu.op == 0x0f {
                    self.alu_operation_end();
                } else if self.alu.op >= 8 {
                    if self.alu.src == SRC_DST_REG {
                        self.alu.b_value = self.read_reg(self.alu.b_reg);
                    } else {
                        self.alu.b_value = (self.dil as i32) << 8;
                    }
                } else {
                    if self.alu.src == SRC_DST_REG {
                        self.alu.a_value = self.read_reg(self.alu.a_reg);
                    } else {
                        self.alu.a_value = (self.dil as i32) << 8;
                    }
                    self.alu.b_value = self.read_reg(self.alu.b_reg);
                }

                if self.ram_p.cycle != RAM_CYCLE_READ {
                    if self.ram_p.cycle == RAM_CYCLE_WRITE {
                        if !self.ram_p.io {
                            self.dram_w(self.ram_p.address, self.dol[0]);
                        }
                    }
                    self.dol[0] = self.dol[1];
                    if self.dol_count > 0 {
                        self.dol_count -= 1;
                    }
                }

                self.pc = self.pc.wrapping_add(1);
            }
            cycles -= 1;
        }
    }

    pub fn run_once(&mut self) {
        self.set_HALT(false);
        self.execute_run(1);
        self.set_HALT(true);
        if self.state != STATE_HALTED {
            self.execute_run(200);
        }
    }

    pub fn state_size(&self) -> usize {
        ES5510::REGISTERS_STATE_SIZE + 4 * 0xc0 + 8 * 160 + 2 * ES5510::DRAM_SIZE as usize
    }

    fn save_ram(writer: &mut StateWriter, r: &RamState) {
        writer.write_i32(r.address);
        writer.write_bool(r.io);
        writer.write_u8(r.cycle);
    }

    fn load_ram(reader: &mut StateReader) -> RamState {
        RamState { address: reader.read_i32(), io: reader.read_u8() != 0, cycle: reader.read_u8() }
    }

    pub fn save_state(&self, writer: &mut StateWriter) {
        writer.write_bool(self.halt_asserted);
        writer.write_u8(self.pc);
        writer.write_u8(self.state as u8);
        for v in [
            self.ser0r, self.ser0l, self.ser1r, self.ser1l, self.ser2r, self.ser2l, self.ser3r, self.ser3l,
        ] {
            writer.write_i16(v);
        }
        writer.write_i64(self.machl);
        writer.write_bool(self.mac_overflow);
        writer.write_i16(self.dil);
        writer.write_i32(self.memsiz);
        writer.write_i32(self.memmask);
        writer.write_i32(self.memincrement);
        writer.write_i8(self.memshift);
        writer.write_i32(self.dlength);
        writer.write_i32(self.abase);
        writer.write_i32(self.bbase);
        writer.write_i32(self.dbase);
        writer.write_i32(self.sigreg);
        writer.write_i32(self.mulshift);
        writer.write_i8(self.ccr);
        writer.write_i8(self.cmr);
        writer.write_i16(self.dol[0]);
        writer.write_i16(self.dol[1]);
        writer.write_i32(self.dol_count);
        writer.write_i32(self.dol_latch);
        writer.write_i32(self.dil_latch);
        writer.write_u32(self.dadr_latch);
        writer.write_i32(self.gpr_latch);
        writer.write_u64(self.instr_latch);
        writer.write_u8(self.ram_sel);
        writer.write_u8(self.host_control);
        writer.write_u8(self.host_serial);

        writer.write_u8(self.alu.a_reg);
        writer.write_u8(self.alu.b_reg);
        writer.write_u8(self.alu.src);
        writer.write_u8(self.alu.dst);
        writer.write_u8(self.alu.op);
        writer.write_i32(self.alu.a_value);
        writer.write_i32(self.alu.b_value);
        writer.write_i32(self.alu.result);
        writer.write_bool(self.alu.update_ccr);
        writer.write_bool(self.alu.write_result);

        writer.write_u8(self.mulacc.c_reg);
        writer.write_u8(self.mulacc.d_reg);
        writer.write_u8(self.mulacc.src);
        writer.write_u8(self.mulacc.dst);
        writer.write_bool(self.mulacc.accumulate);
        writer.write_i32(self.mulacc.c_value);
        writer.write_i32(self.mulacc.d_value);
        writer.write_i64(self.mulacc.product);
        writer.write_i64(self.mulacc.result);
        writer.write_bool(self.mulacc.write_result);

        Self::save_ram(writer, &self.ram);
        Self::save_ram(writer, &self.ram_p);
        Self::save_ram(writer, &self.ram_pp);

        for v in self.m_gpr.iter() {
            writer.write_i32(*v);
        }
        for v in self.m_instr.iter() {
            writer.write_u64(*v);
        }
        let mut dram = Vec::with_capacity(self.m_dram.len() * 2);
        for v in self.m_dram.iter() {
            dram.extend_from_slice(&v.to_le_bytes());
        }
        writer.write_bytes(&dram);
    }

    pub fn load_state(&mut self, reader: &mut StateReader) {
        let mut record = [0u8; ES5510::REGISTERS_STATE_SIZE];
        reader.read_bytes(&mut record);
        let r = &mut StateReader::new(&record);

        let halt_asserted = r.read_u8();
        let pc = r.read_u8();
        let state = r.read_u8();
        let mut ser = [0i16; 8];
        for s in &mut ser {
            *s = r.read_i16();
        }
        let machl = r.read_i64();
        let mac_overflow = r.read_u8();
        let dil = r.read_i16();
        let memsiz = r.read_i32();
        let memmask = r.read_i32();
        let memincrement = r.read_i32();
        let memshift = r.read_i8();
        let dlength = r.read_i32();
        let abase = r.read_i32();
        let bbase = r.read_i32();
        let dbase = r.read_i32();
        let sigreg = r.read_i32();
        let mulshift = r.read_i32();
        let ccr = r.read_i8();
        let cmr = r.read_i8();
        let dol = [r.read_i16(), r.read_i16()];
        let dol_count = r.read_i32();
        let dol_latch = r.read_i32();
        let dil_latch = r.read_i32();
        let dadr_latch = r.read_u32();
        let gpr_latch = r.read_i32();
        let instr_latch = r.read_u64();
        let ram_sel = r.read_u8();
        let host_control = r.read_u8();
        let host_serial = r.read_u8();

        let alu = AluState {
            a_reg: r.read_u8(),
            b_reg: r.read_u8(),
            src: r.read_u8(),
            dst: r.read_u8(),
            op: r.read_u8(),
            a_value: r.read_i32(),
            b_value: r.read_i32(),
            result: r.read_i32(),
            update_ccr: r.read_u8() != 0,
            write_result: r.read_u8() != 0,
        };
        let mulacc = MulAccState {
            c_reg: r.read_u8(),
            d_reg: r.read_u8(),
            src: r.read_u8(),
            dst: r.read_u8(),
            accumulate: r.read_u8() != 0,
            c_value: r.read_i32(),
            d_value: r.read_i32(),
            product: r.read_i64(),
            result: r.read_i64(),
            write_result: r.read_u8() != 0,
        };
        let ram = Self::load_ram(r);
        let ram_p = Self::load_ram(r);
        let ram_pp = Self::load_ram(r);

        require_state(
            state <= 1
                && halt_asserted <= 1
                && memshift >= 0
                && memshift <= 24
                && dol_count >= 0
                && dol_count <= 2
                && alu.op < 16
                && alu.src <= 3
                && alu.dst <= 3
                && mulacc.src <= 3
                && mulacc.dst <= 3,
        );
        require_state(
            ram.io as u8 <= 1
                && ram.cycle <= 2
                && ram_p.io as u8 <= 1
                && ram_p.cycle <= 2
                && ram_pp.io as u8 <= 1
                && ram_pp.cycle <= 2,
        );

        self.halt_asserted = halt_asserted != 0;
        self.pc = pc;
        self.state = if state == 0 { STATE_RUNNING } else { STATE_HALTED };
        self.ser0r = ser[0];
        self.ser0l = ser[1];
        self.ser1r = ser[2];
        self.ser1l = ser[3];
        self.ser2r = ser[4];
        self.ser2l = ser[5];
        self.ser3r = ser[6];
        self.ser3l = ser[7];
        self.machl = machl;
        self.mac_overflow = mac_overflow != 0;
        self.dil = dil;
        self.memsiz = memsiz;
        self.memmask = memmask;
        self.memincrement = memincrement;
        self.memshift = memshift;
        self.dlength = dlength;
        self.abase = abase;
        self.bbase = bbase;
        self.dbase = dbase;
        self.sigreg = sigreg;
        self.mulshift = mulshift;
        self.ccr = ccr;
        self.cmr = cmr;
        self.dol = dol;
        self.dol_count = dol_count;
        self.dol_latch = dol_latch;
        self.dil_latch = dil_latch;
        self.dadr_latch = dadr_latch;
        self.gpr_latch = gpr_latch;
        self.instr_latch = instr_latch;
        self.ram_sel = ram_sel;
        self.host_control = host_control;
        self.host_serial = host_serial;
        self.alu = alu;
        self.mulacc = mulacc;
        self.ram = ram;
        self.ram_p = ram_p;
        self.ram_pp = ram_pp;

        for v in self.m_gpr.iter_mut() {
            *v = reader.read_i32();
        }
        for v in self.m_instr.iter_mut() {
            *v = reader.read_u64();
        }
        let mut dram = vec![0u8; self.m_dram.len() * 2];
        reader.read_bytes(&mut dram);
        for (v, b) in self.m_dram.iter_mut().zip(dram.chunks_exact(2)) {
            *v = i16::from_le_bytes([b[0], b[1]]);
        }
    }
}
