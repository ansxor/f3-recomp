//! `runtime/state_io.hpp`: `StateWriter`/`StateReader` over a caller-provided span. The canonical
//! records are packed and native (little-endian), so each field is written in declaration order.

pub struct StateWriter<'a> {
    dst: &'a mut [u8],
    pos: usize,
}

impl<'a> StateWriter<'a> {
    pub fn new(dst: &'a mut [u8]) -> Self {
        StateWriter { dst, pos: 0 }
    }

    pub fn write_bytes(&mut self, src: &[u8]) {
        if self.dst.len() - self.pos < src.len() {
            panic!("StateWriter buffer overflow");
        }
        self.dst[self.pos..self.pos + src.len()].copy_from_slice(src);
        self.pos += src.len();
    }

    pub fn write_u8(&mut self, v: u8) {
        self.write_bytes(&[v]);
    }
    pub fn write_i8(&mut self, v: i8) {
        self.write_bytes(&v.to_le_bytes());
    }
    pub fn write_u16(&mut self, v: u16) {
        self.write_bytes(&v.to_le_bytes());
    }
    pub fn write_i16(&mut self, v: i16) {
        self.write_bytes(&v.to_le_bytes());
    }
    pub fn write_u32(&mut self, v: u32) {
        self.write_bytes(&v.to_le_bytes());
    }
    pub fn write_i32(&mut self, v: i32) {
        self.write_bytes(&v.to_le_bytes());
    }
    pub fn write_u64(&mut self, v: u64) {
        self.write_bytes(&v.to_le_bytes());
    }
    pub fn write_i64(&mut self, v: i64) {
        self.write_bytes(&v.to_le_bytes());
    }
    pub fn write_f32(&mut self, v: f32) {
        self.write_bytes(&v.to_le_bytes());
    }
    pub fn write_bool(&mut self, v: bool) {
        self.write_u8(v as u8);
    }

    pub fn advance(&mut self, size: usize) {
        if self.dst.len() - self.pos < size {
            panic!("StateWriter buffer overflow");
        }
        self.pos += size;
    }

    pub fn remaining(&self) -> usize {
        self.dst.len() - self.pos
    }
}

pub struct StateReader<'a> {
    src: &'a [u8],
    pos: usize,
}

impl<'a> StateReader<'a> {
    pub fn new(src: &'a [u8]) -> Self {
        StateReader { src, pos: 0 }
    }

    pub fn read_bytes(&mut self, dst: &mut [u8]) {
        if self.src.len() - self.pos < dst.len() {
            panic!("StateReader buffer underflow");
        }
        dst.copy_from_slice(&self.src[self.pos..self.pos + dst.len()]);
        self.pos += dst.len();
    }

    fn take<const N: usize>(&mut self) -> [u8; N] {
        let mut b = [0u8; N];
        self.read_bytes(&mut b);
        b
    }

    pub fn read_u8(&mut self) -> u8 {
        self.take::<1>()[0]
    }
    pub fn read_i8(&mut self) -> i8 {
        i8::from_le_bytes(self.take())
    }
    pub fn read_u16(&mut self) -> u16 {
        u16::from_le_bytes(self.take())
    }
    pub fn read_i16(&mut self) -> i16 {
        i16::from_le_bytes(self.take())
    }
    pub fn read_u32(&mut self) -> u32 {
        u32::from_le_bytes(self.take())
    }
    pub fn read_i32(&mut self) -> i32 {
        i32::from_le_bytes(self.take())
    }
    pub fn read_u64(&mut self) -> u64 {
        u64::from_le_bytes(self.take())
    }
    pub fn read_i64(&mut self) -> i64 {
        i64::from_le_bytes(self.take())
    }
    pub fn read_f32(&mut self) -> f32 {
        f32::from_le_bytes(self.take())
    }
    pub fn read_bool(&mut self) -> bool {
        self.read_u8() != 0
    }

    pub fn advance(&mut self, size: usize) {
        if self.src.len() - self.pos < size {
            panic!("StateReader buffer underflow");
        }
        self.pos += size;
    }

    pub fn remaining(&self) -> usize {
        self.src.len() - self.pos
    }
}

pub fn require_state(valid: bool) {
    if !valid {
        panic!("Snapshot field outside canonical representation");
    }
}
